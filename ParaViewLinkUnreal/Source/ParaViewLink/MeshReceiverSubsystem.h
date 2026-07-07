#pragma once
#include "CoreMinimal.h"
#include "Tickable.h"
#include "SocketReceiverSubsystem.h"
#include "MeshTypes.h"
#include "MeshReceiverSubsystem.generated.h"

class ASimContainerActor;
class AStreamedMeshActor;
class UMaterial;
class UMaterialInstanceDynamic;
class UTexture2D;

UCLASS()
class PARAVIEWLINK_API UMeshReceiverSubsystem
    : public USocketReceiverSubsystem
    , public FTickableGameObject
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // FTickableGameObject
    virtual void    Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool    IsTickable() const override { return !IsTemplate() && bActive; }

    /**
     * Transform applied to every spawned AStreamedMeshActor as its world transform.
     * Use this to map ParaView / VTK coordinates into UE world space:
     *
     *   Scale    — unit conversion, e.g. (100,100,100) for metres → cm
     *   Rotation — axis remapping, e.g. 90° around X if VTK Z-up ≠ UE Z-up
     *   Location — world offset, e.g. to centre the dataset at the origin
     *
     * Note: a negative scale component flips mesh handedness; if faces appear
     * inside-out after flipping an axis, also reverse winding on the Python side
     * (swap triangle index 1 and 2 in each triplet).
     *
     * Defaults to identity — ParaView coordinates passed through unchanged.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Viz|Coordinates")
    FTransform CoordTransform;

    /**
     * The one base UMaterial containing the scalar-field shader graph.
     * Must have a Texture2D parameter named "Colormap".
     * Assign in the Blueprint subclass or via editor defaults.
     * A MID per variable is created from this at runtime — no disk writes needed
     * beyond this single saved asset.
     */
    UPROPERTY(EditDefaultsOnly, Category="ScalarField|Material")
    UMaterial* BaseScalarMaterial = nullptr;

protected:
    virtual void HandleRawMessage(int32 Cmd, TArray<uint8> Payload) override;

private:
    // Set true in Initialize, false in Deinitialize.
    // Checked in IsTickable and at the top of Tick to prevent any game-thread
    // work after the subsystem has been torn down.
    bool bActive = false;

    // Manual-reset event used to synchronise the UPDATE ack with the game-thread
    // buffer swap.  The IO thread (HandleRawMessage for CMD_UPDATE) waits on this
    // event after enqueuing the Flip command; Tick() triggers it after the swap is
    // complete, allowing HandleRawMessage to return and SocketReceiverRunnable::Run()
    // to send the ack to ParaView.
    FEvent* FlipEvent = nullptr;

    // --- install queue (network thread → game thread) ---
    TQueue<TUniquePtr<FPendingCommand>, EQueueMode::Spsc> InstallQueue;

    // --- game-thread state ---
    // Non-UObject data — no UPROPERTY needed
    TMap<FString, FParsedMeshData>  PendingMeshes;   // back buffer (CMD_MESH path)
    TMap<FString, FPendingCommand>  PendingPVData;   // back buffer (CMD_PVMESH path)
    TMap<FString, FVector2f>        ColormapRanges;  // var name → (min, max)
    FBox                            StoredPVBounds = FBox(ForceInit); // last domain AABB from ParaView

    // UObject pointers — UPROPERTY so GC does not collect them while we hold
    // raw references.  Without this, a GC pass during PIE teardown can free
    // textures or MIDs that are still reachable through our maps, causing a
    // crash when Deinitialize (or a final Tick) touches them.
    UPROPERTY()
    TMap<FString, AStreamedMeshActor*>       LiveActors;

    UPROPERTY()
    TMap<FString, UTexture2D*>               Colormaps;       // var name → 1D colormap texture

    UPROPERTY()
    TMap<FString, UMaterialInstanceDynamic*> VarMaterials;    // var name → shared MID

    // --- parsing (network thread) ---
    static FParsedMeshData ParseMeshPayload    (const TArray<uint8>& Payload);
    static bool            ParseScalarsPayload (const TArray<uint8>& Payload,
                                                FString& OutMeshName,
                                                FString& OutVarName,
                                                TArray<float>& OutScalars);
    static bool            ParseVariablePayload(const TArray<uint8>& Payload,
                                                FString& OutMeshName,
                                                FString& OutVarName);
    static bool            ParseColormapPayload(const TArray<uint8>& Payload,
                                                FString& OutVarName,
                                                float& OutMin, float& OutMax,
                                                TArray<FLinearColor>& OutRGB);

    // Parses the all-in-one payload produced by the ParaView UE5MeshSender plugin.
    static bool            ParsePVMeshPayload  (const TArray<uint8>& Payload,
                                                FParsedMeshData& OutMesh,
                                                TArray<float>&   OutScalars,
                                                float&           OutScalarMin,
                                                float&           OutScalarMax,
                                                FString&         OutColorName,
                                                int32&           OutScalarLocation);

    // Parses the domain bounding box sent by UE5DomainBoundsFilter.
    static bool            ParseBoundsPayload     (const TArray<uint8>& Payload,
                                                   FBox& OutBounds);

    // Parses a visibility toggle sent when a UE5MeshSender is shown/hidden in PV.
    static bool            ParseVisibilityPayload (const TArray<uint8>& Payload,
                                                   FString& OutMeshName,
                                                   bool& bOutVisible);

    // Shared string reader used by all parsers
    static bool ReadString(const uint8*& Ptr, const uint8* End, FString& OutStr);

    // --- game-thread handlers ---

    /**
     * Recomputes CoordTransform from StoredPVBounds and the first ASimContainerActor
     * found in the level (matched by tag "SimContainer").
     * Called whenever a CMD_BOUNDS message is processed in Tick.
     */
    void ComputeCoordTransform();

    void InstallOrUpdate  (UWorld* World, const FString& Name, const FParsedMeshData& Data);
    void HandleScalarsCmd (const FString& MeshName, const FString& VarName,
                           TArray<float>& Scalars);
    void HandleVariableCmd(const FString& MeshName, const FString& VarName);
    void HandleColormapCmd(const FString& VarName, float Min, float Max,
                           TArray<FLinearColor>& RGB);

    UTexture2D* CreateOrUpdateColormapTexture(const FString& VarName,
                                              const TArray<FLinearColor>& RGB);

    /** Get or create the per-variable MID and set its Colormap texture parameter. */
    UMaterialInstanceDynamic* EnsureVarMaterial(const FString& VarName, UTexture2D* ColormapTex);
};
