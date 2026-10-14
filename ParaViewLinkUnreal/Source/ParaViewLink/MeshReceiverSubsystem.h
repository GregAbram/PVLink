#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Containers/Queue.h"
#include "Async/Future.h"
#include "HAL/ThreadSafeBool.h"
#include "MeshReceiverSubsystem.generated.h"

class AStreamedMeshActor;
class UMaterial;
class UMaterialInstanceDynamic;
class UTexture2D;
struct dm_client;
struct FDMBatch;
class FDMPollRunnable;

/**
 * Shows a DataManager project (see SPEC.md in the DataManager repository):
 * every KeyData item becomes an AStreamedMeshActor placed into the level's
 * SimContainer, coloured through M_ScalarField by its colour-by array and
 * ParaView colormap.
 *
 * Data comes from a DataManager over the network (DataManagerHost, or
 * discovery when empty) or, with LocalStorePath set, from a store on this
 * device with no network. A background thread receives batches and builds
 * their geometry; the game thread applies each batch whole in one tick and
 * then acknowledges it (SPEC.md 9).
 *
 * DefaultGame.ini:
 *   [/Script/ParaViewLink.MeshReceiverSubsystem]
 *   DataManagerHost=          ; empty: discovery
 *   DataManagerPort=9010
 *   DiscoveryPort=9011
 *   Project=                  ; empty: the DataManager's default
 *   bCompress=True
 *   LocalStorePath=           ; play a store in-process instead
 *   bPlayOnStart=False
 *   PlaybackRate=5
 *   PointSize=0.004           ; fraction of the domain diagonal
 *   LineWidth=0.0015
 */
UCLASS(Config = Game)
class PARAVIEWLINK_API UMeshReceiverSubsystem
    : public UGameInstanceSubsystem
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

    // ---- settings (DefaultGame.ini) ----
    UPROPERTY(Config, EditAnywhere, Category = "DataManager") FString DataManagerHost;
    UPROPERTY(Config, EditAnywhere, Category = "DataManager") int32   DataManagerPort = 9010;
    UPROPERTY(Config, EditAnywhere, Category = "DataManager") int32   DiscoveryPort = 9011;
    UPROPERTY(Config, EditAnywhere, Category = "DataManager") FString Project;
    UPROPERTY(Config, EditAnywhere, Category = "DataManager") bool    bCompress = true;
    UPROPERTY(Config, EditAnywhere, Category = "DataManager") FString LocalStorePath;
    UPROPERTY(Config, EditAnywhere, Category = "DataManager") bool    bPlayOnStart = false;
    UPROPERTY(Config, EditAnywhere, Category = "DataManager") float   PlaybackRate = 5.f;
    UPROPERTY(Config, EditAnywhere, Category = "Appearance")  float   PointSize = 0.004f;
    UPROPERTY(Config, EditAnywhere, Category = "Appearance")  float   LineWidth = 0.0015f;
    UPROPERTY(Config, EditAnywhere, Category = "Appearance")  FLinearColor SolidColor = FLinearColor(0.8f, 0.8f, 0.8f);

    /** World transform of every mesh actor: maps the domain into the SimContainer. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Viz|Coordinates")
    FTransform CoordTransform;

    /** Base material with a Texture2D parameter "Colormap" sampled at (VertexColor.R, 0). */
    UPROPERTY(EditDefaultsOnly, Category = "ScalarField|Material")
    UMaterial* BaseScalarMaterial = nullptr;

    // ---- connection and playback (SPEC.md 5.5) ----
    /** Connects (or re-connects) to a specific DataManager. */
    UFUNCTION(BlueprintCallable, Category = "PVLink") void ConnectToDataManager(const FString& Host, int32 Port);
    UFUNCTION(BlueprintCallable, Category = "PVLink") void Play();
    UFUNCTION(BlueprintCallable, Category = "PVLink") void Pause();
    UFUNCTION(BlueprintCallable, Category = "PVLink") void Seek(float Time);
    UFUNCTION(BlueprintCallable, Category = "PVLink") void Step(int32 Delta);
    UFUNCTION(BlueprintCallable, Category = "PVLink") void SetRate(float TimestepsPerSecond);
    UFUNCTION(BlueprintPure, Category = "PVLink") float GetCurrentTime() const { return float(CurrentTime); }
    UFUNCTION(BlueprintPure, Category = "PVLink") bool IsConnected() const;

private:
    bool bActive = false;
    bool bStartedPlaying = false;
    double CurrentTime = 0.0;

    dm_client*        Client = nullptr;
    FDMPollRunnable*  Poller = nullptr;
    FRunnableThread*  PollerThread = nullptr;
    TFuture<void>     Discovery;
    FThreadSafeBool   bDiscovering = false;

    void StartClient(dm_client* NewClient);
    void StopClient();
    void StartDiscovery();
    void ApplyBatch(FDMBatch& Batch);

    // ---- scene state (game thread) ----
    FBox StoredPVBounds = FBox(ForceInit);   // Unreal frame
    TMap<FString, FVector2f> ColormapRanges;
    TMap<FString, bool>      Visible;

    UPROPERTY() TMap<FString, AStreamedMeshActor*>       LiveActors;
    UPROPERTY() TMap<FString, UTexture2D*>               Colormaps;
    UPROPERTY() TMap<FString, UMaterialInstanceDynamic*> VarMaterials;

    void ComputeCoordTransform();
    void ApplyVisibility();
    UTexture2D* CreateOrUpdateColormapTexture(const FString& VarName, const TArray<FLinearColor>& RGB);
    UMaterialInstanceDynamic* EnsureVarMaterial(const FString& VarName, UTexture2D* ColormapTex);
};
