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
    CachedVertices  = Vertices;
    CachedNormals   = Normals;

    // Flip winding: ParaView (right-handed) → UE (left-handed) reverses triangle orientation.
    // Swap the 2nd and 3rd index of every triangle.
    CachedTriangles = Triangles;
    for (int32 i = 0; i + 2 < CachedTriangles.Num(); i += 3)
        Swap(CachedTriangles[i + 1], CachedTriangles[i + 2]);

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

    if (Scalars.Num() != CachedVertices.Num())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("StreamedMeshActor '%s': scalar count (%d) != vertex count (%d) - skipping"),
            *GetActorLabel(), Scalars.Num(), CachedVertices.Num());
        return;
    }

    const float Range = FMath::Max(ScalarMax - ScalarMin, KINDA_SMALL_NUMBER);

    TArray<FLinearColor> Colors;
    Colors.SetNumUninitialized(Scalars.Num());
    for (int32 i = 0; i < Scalars.Num(); i++)
    {
        // Clamp to [0,1] so values outside the colormap range saturate at the ends.
        const float T = FMath::Clamp((Scalars[i] - ScalarMin) / Range, 0.f, 1.f);
        Colors[i] = FLinearColor(T, 0.f, 0.f, 1.f);   // R = colormap UV coordinate
    }

    // Re-submit full geometry with new vertex colours, reusing cached normals.
    TArray<FVector2D>        UVs;
    TArray<FProcMeshTangent> Tangents;

    MeshComponent->CreateMeshSection_LinearColor(
        0, CachedVertices, CachedTriangles, CachedNormals, UVs, Colors, Tangents, false);
}
