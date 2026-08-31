#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "StreamedMeshActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInstanceDynamic;

UCLASS()
class PARAVIEWLINK_API AStreamedMeshActor : public AActor
{
    GENERATED_BODY()

public:
    AStreamedMeshActor();

    /**
     * Upload new geometry.  Clears any existing mesh sections.
     * Normals may be empty — UE will auto-compute them from the triangle winding.
     */
    void UpdateMesh(const TArray<FVector>& Vertices, const TArray<int32>& Triangles,
                    const TArray<FVector>& Normals);

    /** Store a named per-vertex scalar array.  Does not trigger a re-render. */
    void StoreScalars(const FString& VarName, const TArray<float>& Scalars);

    /**
     * Switch the displayed variable.  Remaps the stored scalar array using the
     * provided [ScalarMin, ScalarMax] range, uploads as vertex colour (R channel),
     * and assigns MID as the section material.
     * MID is owned by UMeshReceiverSubsystem — this actor holds only a non-owning
     * UPROPERTY reference so GC doesn't collect it.
     * MID may be nullptr (vertex colours still upload; material unchanged).
     * ScalarMin/Max are stored so they can be reused when geometry is re-uploaded.
     */
    void SetActiveVariable(const FString& VarName, UMaterialInstanceDynamic* MID,
                           float ScalarMin, float ScalarMax);

    /** Returns the name of the currently active variable, or empty string if none. */
    const FString& GetActiveVariable() const { return ActiveVariable; }

private:
    UPROPERTY()
    UProceduralMeshComponent* MeshComponent;

    /** Non-owning reference to the subsystem-managed MID for the active variable. */
    UPROPERTY()
    UMaterialInstanceDynamic* ActiveMID = nullptr;

    // Cached geometry — needed to re-submit when vertex colours change.
    // Two-sided rendering is done by duplicating geometry with reversed
    // winding + flipped normals (see UpdateMesh), not by a two-sided material
    // shader trick -- so CachedVertices/CachedNormals are 2x NumOriginalVertices;
    // vertex j and vertex (j + NumOriginalVertices) are the same source point,
    // front and back copies.
    TArray<FVector> CachedVertices;
    TArray<int32>   CachedTriangles;
    TArray<FVector> CachedNormals;    // never empty -- always computed, see UpdateMesh
    int32            NumOriginalVertices = 0;

    // Per-variable scalar storage (raw, un-normalised)
    TMap<FString, TArray<float>> ScalarArrays;

    FString ActiveVariable;
    float   ActiveScalarMin = 0.f;   // colormap range stored for geometry re-uploads
    float   ActiveScalarMax = 1.f;

    /** Remap scalars by [ScalarMin, ScalarMax] and upload as vertex colours (R channel). */
    void UploadScalarsAsVertexColors(const TArray<float>& Scalars,
                                     float ScalarMin, float ScalarMax);
};
