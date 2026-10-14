#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

/**
 * DataManager batches, decoded into what the game thread needs (SPEC.md 6 in
 * the DataManager repository). Built on the poll thread; no UObject access.
 *
 * Coordinates: ParaView's right-handed frame becomes Unreal's left-handed
 * Z-up frame by negating y (SPEC.md 4). Triangles keep ParaView's winding:
 * AStreamedMeshActor::UpdateMesh reverses it.
 */
struct FDMMesh
{
    FString Name;
    TArray<FVector> Vertices;    // Unreal frame
    TArray<int32>   Triangles;   // ParaView winding
    TArray<FVector> Normals;     // Unreal frame; empty = computed by the actor
    FString         ColorBy;     // colour-by array, empty if none
    TArray<float>   ColorValues; // one per vertex (raw values)
    FVector2f       DataRange = FVector2f(0.f, 1.f);
};

struct FDMColormap
{
    FString Variable;
    float Min = 0.f, Max = 1.f;
    TArray<FLinearColor> RGB;
};

struct FDMBatch
{
    uint64 Id = 0;
    double Time = 0.0;
    TArray<FString>     Removed;      // keys in the previous manifest but not this one
    TArray<FDMMesh>     Meshes;
    TArray<FDMColormap> Colormaps;
    TOptional<FBox>     Bounds;       // Unreal frame
    TMap<FString, bool> Visibility;   // item name -> visible
};

namespace PVLinkKeyData
{
    inline FVector ToUnreal(double X, double Y, double Z) { return FVector(X, -Y, Z); }

    /** ParaView-frame AABB from a Bounds/domain item (f32[6]). */
    bool ParseBounds(const uint8* Data, uint64 Len, FBox& OutPVBounds);
    bool ParseColormap(const uint8* Data, uint64 Len, FDMColormap& Out);

    /**
     * Decodes a KeyData item and builds its triangles. Points become
     * octahedra of half-size PointRadius and line strips tubes of radius
     * LineRadius (both ParaView units): Unreal has no wide points or lines.
     * Returns false with OutError set if the item is malformed; voxels
     * (volume rendering deferred) return false with an empty error.
     */
    bool Build(const FString& Name, const char* Meta, uint32 MetaLen, const uint8* Data, uint64 DataLen,
               float PointRadius, float LineRadius, FDMMesh& Out, FString& OutError);

    /** ParaView-frame AABB from a KeyData header, for sizing points/lines before any Bounds item. */
    bool HeaderBounds(const char* Meta, uint32 MetaLen, FBox& OutPVBounds);
}
