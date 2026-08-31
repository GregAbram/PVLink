#include "StreamedMeshActor.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

AStreamedMeshActor::AStreamedMeshActor()
{
    PrimaryActorTick.bCanEverTick = false;
    MeshComponent = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MeshComponent"));
    SetRootComponent(MeshComponent);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void AStreamedMeshActor::UpdateMesh(const TArray<FVector>& Vertices,
                                     const TArray<int32>&   Triangles,
                                     const TArray<FVector>& Normals)
{
    // Flip winding: ParaView (right-handed) → UE (left-handed) reverses triangle orientation.
    // Swap the 2nd and 3rd index of every triangle.
    TArray<int32> FrontTriangles = Triangles;
    for (int32 i = 0; i + 2 < FrontTriangles.Num(); i += 3)
        Swap(FrontTriangles[i + 1], FrontTriangles[i + 2]);

    // Two-sided rendering needs real per-vertex normals to flip for the back
    // copy (see below) -- compute simple area-weighted face normals if the
    // source didn't provide any, rather than relying on ProceduralMeshComponent
    // to auto-compute them (it doesn't, for an empty normals array).
    TArray<FVector> FrontNormals = Normals;
    if (FrontNormals.Num() == Vertices.Num())
    {
        // Triangle winding is purely a rasterizer/culling convention (which
        // vertex order counts as "front"); the normal is a physical vector.
        // Since vertex positions aren't mirrored on any axis, ParaView's raw
        // normals are already correct as given -- the winding swap above
        // doesn't require negating them. (Verified against a Sphere source:
        // negating here produced exactly-inverted lighting -- lit patch grew
        // only brighter, never spread, as light intensity increased.)
    }
    else
    {
        FrontNormals.SetNumZeroed(Vertices.Num());
        for (int32 t = 0; t + 2 < FrontTriangles.Num(); t += 3)
        {
            const int32 a = FrontTriangles[t], b = FrontTriangles[t + 1], c = FrontTriangles[t + 2];
            const FVector N = FVector::CrossProduct(Vertices[b] - Vertices[a], Vertices[c] - Vertices[a]);
            FrontNormals[a] += N; FrontNormals[b] += N; FrontNormals[c] += N;
        }
        for (FVector& N : FrontNormals)
            N = N.IsNearlyZero() ? FVector::UpVector : N.GetSafeNormal();
    }

    // Two-sided rendering via duplicated geometry, not a two-sided material
    // shader trick: a "back" copy of every vertex with a flipped normal, plus
    // a mirrored (reversed-winding) triangle set indexing into it, so plain
    // single-sided backface culling shows the correct, correctly-lit face
    // from either side. A TwoSided material renders both faces but does NOT
    // flip the normal used for lighting on the back face, which reads as
    // unexpectedly dark/wrong -- this sidesteps that entirely.
    const int32 N = Vertices.Num();
    NumOriginalVertices = N;

    CachedVertices = Vertices;
    CachedVertices.Append(Vertices);

    CachedNormals = FrontNormals;
    for (int32 i = 0; i < N; i++)
        CachedNormals.Add(-FrontNormals[i]);

    CachedTriangles = FrontTriangles;
    for (int32 t = 0; t + 2 < FrontTriangles.Num(); t += 3)
    {
        // Offset into the duplicated vertex range and reverse winding.
        CachedTriangles.Add(FrontTriangles[t]     + N);
        CachedTriangles.Add(FrontTriangles[t + 2] + N);
        CachedTriangles.Add(FrontTriangles[t + 1] + N);
    }

    // Upload with empty vertex colours — scalar colouring applied later via
    // SetActiveVariable if a variable is already active.
    TArray<FVector2D>        UVs;
    TArray<FLinearColor>     Colors;
    TArray<FProcMeshTangent> Tangents;

    MeshComponent->CreateMeshSection_LinearColor(
        0, CachedVertices, CachedTriangles, CachedNormals, UVs, Colors, Tangents, false);

    // Re-apply the active variable if we already have scalars for it.
    // Uses the stored min/max so the colormap range is preserved across geometry updates.
    if (!ActiveVariable.IsEmpty() && ScalarArrays.Contains(ActiveVariable))
        SetActiveVariable(ActiveVariable, ActiveMID, ActiveScalarMin, ActiveScalarMax);
}

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------

void AStreamedMeshActor::StoreScalars(const FString& VarName,
                                       const TArray<float>& Scalars)
{
    ScalarArrays.Add(VarName, Scalars);
    UE_LOG(LogTemp, Log, TEXT("StreamedMeshActor '%s': stored %d scalars for '%s'"),
        *GetActorLabel(), Scalars.Num(), *VarName);
}

void AStreamedMeshActor::SetActiveVariable(const FString& VarName,
                                            UMaterialInstanceDynamic* MID,
                                            float ScalarMin, float ScalarMax)
{
    const TArray<float>* ScalarsPtr = ScalarArrays.Find(VarName);
    if (!ScalarsPtr)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("StreamedMeshActor '%s': no scalars stored for variable '%s'"),
            *GetActorLabel(), *VarName);
        return;
    }

    ActiveVariable  = VarName;
    ActiveMID       = MID;
    ActiveScalarMin = ScalarMin;
    ActiveScalarMax = ScalarMax;

    UploadScalarsAsVertexColors(*ScalarsPtr, ScalarMin, ScalarMax);

    if (MID)
        MeshComponent->SetMaterial(0, MID);

    UE_LOG(LogTemp, Log,
        TEXT("StreamedMeshActor '%s': active variable '%s' [%.4g, %.4g]"),
        *GetActorLabel(), *VarName, ScalarMin, ScalarMax);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void AStreamedMeshActor::UploadScalarsAsVertexColors(const TArray<float>& Scalars,
                                                      float ScalarMin, float ScalarMax)
{
    if (CachedVertices.Num() == 0) return;

    // Scalars are per-ORIGINAL-vertex; CachedVertices is 2x that (front+back
    // copies for two-sided rendering, see UpdateMesh) -- compare against
    // NumOriginalVertices, not the doubled CachedVertices count.
    if (Scalars.Num() != NumOriginalVertices)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("StreamedMeshActor '%s': scalar count (%d) != vertex count (%d) - skipping"),
            *GetActorLabel(), Scalars.Num(), NumOriginalVertices);
        return;
    }

    const float Range = FMath::Max(ScalarMax - ScalarMin, KINDA_SMALL_NUMBER);

    TArray<FLinearColor> Colors;
    Colors.SetNumUninitialized(CachedVertices.Num());
    for (int32 i = 0; i < CachedVertices.Num(); i++)
    {
        // Front copy (i < NumOriginalVertices) and its back twin
        // (i + NumOriginalVertices) share the same source scalar value.
        const float S = Scalars[i % NumOriginalVertices];
        // Clamp to [0,1] so values outside the colormap range saturate at the ends.
        const float T = FMath::Clamp((S - ScalarMin) / Range, 0.f, 1.f);
        Colors[i] = FLinearColor(T, 0.f, 0.f, 1.f);   // R = colormap UV coordinate
    }

    // Re-submit full geometry with new vertex colours, reusing cached normals.
    TArray<FVector2D>        UVs;
    TArray<FProcMeshTangent> Tangents;

    MeshComponent->CreateMeshSection_LinearColor(
        0, CachedVertices, CachedTriangles, CachedNormals, UVs, Colors, Tangents, false);
}
