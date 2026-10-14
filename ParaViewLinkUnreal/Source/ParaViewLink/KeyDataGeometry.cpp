#include "KeyDataGeometry.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // ABR DataTopology (SPEC.md 6.1)
    enum ETopology : int32 { Triangles = 0, Quads = 2, Lines = 3, LineStrip = 4, Points = 5, Voxels = 100 };

    struct FHeader
    {
        int32 Topology = 0, NumPoints = 0, NumCells = 0, NumCellIndices = 0;
        TArray<FString> Scalars, Vectors;
        TArray<double> Mins, Maxes;
        FVector Center = FVector::ZeroVector, Extent = FVector::ZeroVector;
        FString ColorBy;
    };

    bool ParseHeader(const char* Meta, uint32 Len, FHeader& H)
    {
        FUTF8ToTCHAR Conv(Meta, Len);
        FString Json(Conv.Length(), Conv.Get());
        TSharedPtr<FJsonObject> O;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), O) || !O.IsValid())
            return false;
        H.Topology = O->GetIntegerField(TEXT("meshTopology"));
        H.NumPoints = O->GetIntegerField(TEXT("num_points"));
        H.NumCells = O->GetIntegerField(TEXT("num_cells"));
        H.NumCellIndices = O->GetIntegerField(TEXT("num_cell_indices"));
        O->TryGetStringArrayField(TEXT("scalarArrayNames"), H.Scalars);
        O->TryGetStringArrayField(TEXT("vectorArrayNames"), H.Vectors);
        const TArray<TSharedPtr<FJsonValue>>* A;
        if (O->TryGetArrayField(TEXT("scalarMins"), A)) for (auto& V : *A) H.Mins.Add(V->AsNumber());
        if (O->TryGetArrayField(TEXT("scalarMaxes"), A)) for (auto& V : *A) H.Maxes.Add(V->AsNumber());
        const TSharedPtr<FJsonObject>* B;
        if (O->TryGetObjectField(TEXT("bounds"), B))
        {
            auto Vec = [](const TSharedPtr<FJsonObject>& P, const TCHAR* Name) {
                const TSharedPtr<FJsonObject>* V;
                return P->TryGetObjectField(Name, V)
                    ? FVector((*V)->GetNumberField(TEXT("x")), (*V)->GetNumberField(TEXT("y")), (*V)->GetNumberField(TEXT("z")))
                    : FVector::ZeroVector;
            };
            H.Center = Vec(*B, TEXT("m_Center"));
            H.Extent = Vec(*B, TEXT("m_Extent"));
        }
        const TSharedPtr<FJsonObject>* C;
        if (O->TryGetObjectField(TEXT("colorBy"), C)) (*C)->TryGetStringField(TEXT("array"), H.ColorBy);
        return H.NumPoints >= 0 && H.NumCellIndices >= 0;
    }

    const FVector OctDirs[6] = { FVector(1,0,0), FVector(-1,0,0), FVector(0,1,0), FVector(0,-1,0), FVector(0,0,1), FVector(0,0,-1) };
    const int32 OctTris[24] = { 0,2,4, 2,1,4, 1,3,4, 3,0,4, 2,0,5, 1,2,5, 3,1,5, 0,3,5 };
    constexpr int32 TubeSides = 6;

    FVector Tangent(const TArray<FVector>& P, int32 J)
    {
        FVector D = J == 0 ? P[1] - P[0] : J == P.Num() - 1 ? P[J] - P[J - 1] : P[J + 1] - P[J - 1];
        return D.IsNearlyZero() ? FVector(0, 0, 1) : D.GetSafeNormal();
    }
    FVector Perpendicular(const FVector& T)
    {
        return FVector::CrossProduct(T, FMath::Abs(T.Z) < 0.9 ? FVector(0, 0, 1) : FVector(1, 0, 0)).GetSafeNormal();
    }
}

namespace PVLinkKeyData
{

bool ParseBounds(const uint8* Data, uint64 Len, FBox& Out)
{
    if (Len < 24) return false;
    float F[6];
    FMemory::Memcpy(F, Data, 24);
    Out = FBox(FVector(F[0], F[2], F[4]), FVector(F[1], F[3], F[5]));
    return true;
}

bool ParseColormap(const uint8* Data, uint64 Len, FDMColormap& Out)
{
    const uint8* P = Data;
    const uint8* End = Data + Len;
    auto Read = [&](void* Dst, int64 N) { if (P + N > End) return false; FMemory::Memcpy(Dst, P, N); P += N; return true; };
    uint32 NameLen;
    if (!Read(&NameLen, 4) || P + NameLen > End) return false;
    FUTF8ToTCHAR Conv(reinterpret_cast<const char*>(P), NameLen);
    Out.Variable = FString(Conv.Length(), Conv.Get());
    P += NameLen;
    int32 N;
    if (!Read(&Out.Min, 4) || !Read(&Out.Max, 4) || !Read(&N, 4) || N < 0 || P + int64(N) * 12 > End) return false;
    Out.RGB.SetNum(N);
    for (int32 i = 0; i < N; ++i)
    {
        float C[3];
        Read(C, 12);
        Out.RGB[i] = FLinearColor(C[0], C[1], C[2], 1.f);
    }
    return true;
}

bool HeaderBounds(const char* Meta, uint32 MetaLen, FBox& Out)
{
    FHeader H;
    if (!ParseHeader(Meta, MetaLen, H)) return false;
    Out = FBox(H.Center - H.Extent, H.Center + H.Extent);
    return true;
}

bool Build(const FString& Name, const char* Meta, uint32 MetaLen, const uint8* Data, uint64 DataLen,
           float PointRadius, float LineRadius, FDMMesh& Out, FString& OutError)
{
    FHeader H;
    if (!ParseHeader(Meta, MetaLen, H)) { OutError = TEXT("bad KeyData header"); return false; }
    if (H.Topology == Voxels) return false;   // volume rendering deferred (SPEC.md 6.2)

    const int64 N = H.NumPoints;
    const int64 Need = 4 * (3 * N + H.NumCellIndices + int64(H.Scalars.Num()) * N + 3 * int64(H.Vectors.Num()) * N);
    if (Need != int64(DataLen))
    {
        OutError = FString::Printf(TEXT("KeyData size %llu != %lld expected from its header"), DataLen, Need);
        return false;
    }
    const float* Pos = reinterpret_cast<const float*>(Data);
    const int32* Cells = reinterpret_cast<const int32*>(Data + 12 * N);
    const float* After = reinterpret_cast<const float*>(Data + 12 * N + 4 * int64(H.NumCellIndices));
    const float* ColorScalars = nullptr;
    const float* ColorVectors = nullptr;
    const float* NormalArray = nullptr;
    for (int32 s = 0; s < H.Scalars.Num(); ++s)
        if (H.Scalars[s] == H.ColorBy) ColorScalars = After + s * N;
    const float* Vecs = After + H.Scalars.Num() * N;
    for (int32 v = 0; v < H.Vectors.Num(); ++v)
    {
        if (H.Vectors[v] == H.ColorBy) ColorVectors = Vecs + v * 3 * N;
        if (H.Vectors[v] == TEXT("Normals")) NormalArray = Vecs + v * 3 * N;
    }

    // Validate cells before indexing with them.
    for (int64 i = 0; i < H.NumCellIndices;)
    {
        const int32 K = Cells[i];
        if (K <= 0 || i + 1 + K > H.NumCellIndices) { OutError = TEXT("malformed cell list"); return false; }
        for (int32 j = 1; j <= K; ++j)
            if (Cells[i + j] < 0 || Cells[i + j] >= N) { OutError = TEXT("cell index out of range"); return false; }
        i += K + 1;
    }

    auto P = [&](int32 i) { return FVector(Pos[3 * i], Pos[3 * i + 1], Pos[3 * i + 2]); };
    auto Value = [&](int32 i) -> float {
        if (ColorScalars) return ColorScalars[i];
        if (ColorVectors) return FVector(ColorVectors[3 * i], ColorVectors[3 * i + 1], ColorVectors[3 * i + 2]).Size();
        return 0.f;
    };
    const bool bColor = ColorScalars || ColorVectors;

    Out.Name = Name;
    Out.ColorBy = bColor ? H.ColorBy : FString();
    if (bColor)
    {
        const int32 Idx = H.Scalars.IndexOfByKey(H.ColorBy);
        if (Idx != INDEX_NONE && H.Mins.IsValidIndex(Idx) && H.Maxes.IsValidIndex(Idx))
            Out.DataRange = FVector2f(float(H.Mins[Idx]), float(H.Maxes[Idx]));
        else
        {
            float Lo = TNumericLimits<float>::Max(), Hi = TNumericLimits<float>::Lowest();
            for (int32 i = 0; i < N; ++i) { Lo = FMath::Min(Lo, Value(i)); Hi = FMath::Max(Hi, Value(i)); }
            Out.DataRange = FVector2f(Lo, Hi);
        }
    }

    // Geometry is built in ParaView's frame, then converted (y negated).
    TArray<FVector> V, Nrm;
    TArray<float> Col;
    TArray<int32> Tri;

    if (H.Topology == Triangles || H.Topology == Quads)
    {
        V.Reserve(N);
        for (int32 i = 0; i < N; ++i) { V.Add(P(i)); if (bColor) Col.Add(Value(i)); }
        if (NormalArray)
            for (int32 i = 0; i < N; ++i) Nrm.Add(FVector(NormalArray[3 * i], NormalArray[3 * i + 1], NormalArray[3 * i + 2]));
        for (int64 i = 0; i < H.NumCellIndices;)
        {
            const int32 K = Cells[i];
            for (int32 j = 1; j + 1 < K; ++j)   // fan (triangles have K == 3)
            {
                Tri.Add(Cells[i + 1]); Tri.Add(Cells[i + 1 + j]); Tri.Add(Cells[i + 2 + j]);
            }
            i += K + 1;
        }
    }
    else if (H.Topology == Points)
    {
        for (int64 i = 0; i < H.NumCellIndices;)
        {
            const int32 K = Cells[i];
            for (int32 j = 1; j <= K; ++j)
            {
                const int32 Id = Cells[i + j];
                const FVector C = P(Id);
                const int32 Base = V.Num();
                for (int32 k = 0; k < 6; ++k)
                {
                    V.Add(C + PointRadius * OctDirs[k]);
                    Nrm.Add(OctDirs[k]);
                    if (bColor) Col.Add(Value(Id));
                }
                for (int32 k = 0; k < 24; ++k) Tri.Add(Base + OctTris[k]);
            }
            i += K + 1;
        }
    }
    else if (H.Topology == LineStrip || H.Topology == Lines)
    {
        for (int64 i = 0; i < H.NumCellIndices;)
        {
            const int32 K = Cells[i];
            if (K >= 2)
            {
                TArray<FVector> Pts;
                for (int32 j = 0; j < K; ++j) Pts.Add(P(Cells[i + 1 + j]));
                FVector Normal = Perpendicular(Tangent(Pts, 0));
                const int32 Base = V.Num();
                for (int32 j = 0; j < K; ++j)
                {
                    const FVector T = Tangent(Pts, j);
                    Normal -= FVector::DotProduct(Normal, T) * T;   // parallel transport
                    Normal = Normal.IsNearlyZero() ? Perpendicular(T) : Normal.GetSafeNormal();
                    const FVector Binormal = FVector::CrossProduct(T, Normal);
                    for (int32 s = 0; s < TubeSides; ++s)
                    {
                        const float A = 2.f * UE_PI * s / TubeSides;
                        const FVector Dir = FMath::Cos(A) * Normal + FMath::Sin(A) * Binormal;
                        V.Add(Pts[j] + LineRadius * Dir);
                        Nrm.Add(Dir);
                        if (bColor) Col.Add(Value(Cells[i + 1 + j]));
                    }
                }
                for (int32 j = 0; j < K - 1; ++j)
                    for (int32 s = 0; s < TubeSides; ++s)
                    {
                        const int32 A0 = Base + j * TubeSides + s, A1 = Base + j * TubeSides + (s + 1) % TubeSides;
                        const int32 B0 = A0 + TubeSides, B1 = A1 + TubeSides;
                        Tri.Add(A0); Tri.Add(B0); Tri.Add(A1);
                        Tri.Add(A1); Tri.Add(B0); Tri.Add(B1);
                    }
            }
            i += K + 1;
        }
    }
    else
    {
        OutError = FString::Printf(TEXT("unsupported topology %d"), H.Topology);
        return false;
    }

    Out.Vertices.SetNum(V.Num());
    for (int32 i = 0; i < V.Num(); ++i) Out.Vertices[i] = ToUnreal(V[i].X, V[i].Y, V[i].Z);
    Out.Normals.SetNum(Nrm.Num());
    for (int32 i = 0; i < Nrm.Num(); ++i) Out.Normals[i] = ToUnreal(Nrm[i].X, Nrm[i].Y, Nrm[i].Z);
    Out.Triangles = MoveTemp(Tri);
    Out.ColorValues = MoveTemp(Col);
    return true;
}

}  // namespace PVLinkKeyData
