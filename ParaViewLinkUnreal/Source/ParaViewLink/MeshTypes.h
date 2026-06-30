#pragma once
#include "CoreMinimal.h"

namespace MeshCmd {
    constexpr int32 String   = 0;
    constexpr int32 Mesh     = 1;
    constexpr int32 Update   = 2;
    constexpr int32 Scalars  = 3;   // mesh name, variable name, float array
    constexpr int32 Variable = 4;   // mesh name, variable name (switch active)
    constexpr int32 Colormap = 5;   // variable name, min, max, RGB sample array
    constexpr int32 Ping     = 6;   // optional UTF-8 message; UE logs and acks

    // All-in-one message from the ParaView UE5MeshSender plugin.
    // Bundles geometry + scalars + scalar range + names into a single packet.
    // Installs immediately — no separate CMD_UPDATE flip needed.
    constexpr int32 PVMesh   = 10;

    // Computational domain bounding box, sent by UE5DomainBoundsFilter.
    // Payload: float32[6]  { xmin, xmax, ymin, ymax, zmin, zmax }
    // UE5 uses this together with a designer-placed container actor to derive
    // the CoordTransform that maps ParaView space → UE world space.
    constexpr int32 Bounds     = 11;

    // Actor visibility toggle, sent when the PV user shows/hides a UE5MeshSender.
    // Payload: int32 name_len, utf8[name_len] mesh_name, int32 visible (0=hidden, 1=visible)
    constexpr int32 Visibility = 12;
}

struct FParsedMeshData {
    FString         Name;
    TArray<FVector> Vertices;
    TArray<int32>   Triangles;
    TArray<FVector> Normals;    // empty → UE auto-computes
};

enum class EPendingCmdType : uint8
{
    Flip,       // CMD_UPDATE  — install accumulated meshes
    Mesh,       // CMD_MESH    — geometry
    Scalars,    // CMD_SCALARS — per-vertex scalar array
    Variable,   // CMD_VARIABLE — switch active variable on a mesh
    Colormap,   // CMD_COLORMAP — 1D RGB colormap for a variable
    PVMesh,     // CMD_PVMESH  — all-in-one from the ParaView plugin (geometry + scalars)
    Bounds,      // CMD_BOUNDS      — computational domain AABB
    Visibility,  // CMD_VISIBILITY  — show/hide a named mesh actor
};

struct FPendingCommand
{
    EPendingCmdType Type = EPendingCmdType::Flip;

    // Mesh (EPendingCmdType::Mesh)
    FParsedMeshData MeshData;

    // Scalars + Variable  (share MeshName / VariableName)
    FString MeshName;
    FString VariableName;
    TArray<float> Scalars;          // EPendingCmdType::Scalars only

    // Colormap  (VariableName is reused as the key)
    TArray<FLinearColor> ColormapRGB;   // EPendingCmdType::Colormap only
    float ColormapMin = 0.f;            // data value that maps to the low end of the map
    float ColormapMax = 1.f;            // data value that maps to the high end

    // PVMesh: -1 = no scalars, 0 = per-point, 1 = per-cell
    int32 ScalarLocation = -1;

    // Bounds (EPendingCmdType::Bounds)
    FBox BoundsBox = FBox(ForceInit);

    // Visibility (EPendingCmdType::Visibility)  — MeshName is reused as the key
    bool bVisible = true;
};
