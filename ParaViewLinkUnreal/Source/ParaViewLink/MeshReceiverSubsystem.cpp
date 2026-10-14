#include "MeshReceiverSubsystem.h"
#include "KeyDataGeometry.h"
#include "Async/Async.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/Paths.h"
#include "dm/dm.h"

#include <atomic>
#include "SimContainerActor.h"
#include "StreamedMeshActor.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "TextureResource.h"
#include "Engine/Texture2D.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

// ---------------------------------------------------------------------------
// Auto-generate M_ScalarField inside the plugin's Content folder.
// Called once on first Initialize() when the asset doesn't yet exist.
// The saved .uasset is then versioned alongside the plugin source.
// ---------------------------------------------------------------------------
static UMaterial* CreateScalarFieldMaterial()
{
    const FString AssetPath = TEXT("/ParaViewLink/M_ScalarField");
    const FString FullRef   = AssetPath + TEXT(".M_ScalarField");

    // Return existing asset if it was already created (e.g. second PIE session).
    if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *FullRef))
        return Existing;

    UPackage* Package = CreatePackage(*AssetPath);
    Package->FullyLoad();

    UMaterial* Mat = NewObject<UMaterial>(Package, TEXT("M_ScalarField"),
                                          RF_Public | RF_Standalone);
    // BaseColor + DefaultLit (not Emissive) -- matches how both ParaView itself
    // and the Unity receiver's actual shader (ScalarFieldLit.shader, despite
    // M_ScalarField.mat's plain name) render this data: as a normally-lit
    // surface. Emissive was tried first but made on-screen brightness purely a
    // function of scene exposure/tonemapping settings rather than actual scene
    // lighting, unlike either other platform.
    //
    // Not TwoSided: two-sided rendering is handled by AStreamedMeshActor
    // duplicating geometry with reversed winding + flipped normals (see
    // UpdateMesh), not by a two-sided material shader trick -- a TwoSided
    // material renders both faces but does NOT flip the normal used for
    // lighting on the back face, which reads as unexpectedly dark/wrong.
    // Standard single-sided backface culling is exactly correct here since
    // every visible face now has its own real, correctly-oriented copy.

    // VertexColor node — R channel carries the normalised scalar value [0,1].
    UMaterialExpressionVertexColor* VC =
        NewObject<UMaterialExpressionVertexColor>(Mat);
    VC->MaterialExpressionEditorX = -400;
    VC->MaterialExpressionEditorY =    0;
    Mat->GetExpressionCollection().AddExpression(VC);

    // Explicit float4 -> float2 mask (R, G) — VertexColor outputs float4, and
    // TextureSampleParameter2D's Coordinates input requires float2; this engine's
    // material compiler rejects the implicit truncation, so mask it explicitly.
    // G is always 0 in the uploaded vertex colours, giving UV = (T, 0).
    UMaterialExpressionComponentMask* UVMask =
        NewObject<UMaterialExpressionComponentMask>(Mat);
    UVMask->MaterialExpressionEditorX = -300;
    UVMask->MaterialExpressionEditorY =    0;
    UVMask->Input.Expression = VC;
    UVMask->R = 1; UVMask->G = 1; UVMask->B = 0; UVMask->A = 0;
    Mat->GetExpressionCollection().AddExpression(UVMask);

    // Texture parameter — sampled at UV = (VertexColor.R, 0).
    // The subsystem sets the "Colormap" parameter to a 1D gradient texture.
    UMaterialExpressionTextureSampleParameter2D* TS =
        NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
    TS->ParameterName             = TEXT("Colormap");
    TS->MaterialExpressionEditorX = -200;
    TS->MaterialExpressionEditorY =    0;
    TS->Coordinates.Expression    = UVMask;
    // Material compilation requires a valid default texture on a Texture Sample
    // node even though it's always overridden at runtime via
    // SetTextureParameterValue -- use the engine's built-in default so this
    // doesn't depend on any project-specific content existing.
    TS->Texture = GEngine ? GEngine->DefaultTexture : nullptr;
    Mat->GetExpressionCollection().AddExpression(TS);

    // Wire texture sample straight to BaseColor -- a raw [0,1] sample is
    // already the natural, correct range for diffuse albedo, no boost needed
    // (unlike Emissive, which this material used originally; see comment above).
    Mat->GetEditorOnlyData()->BaseColor.Expression = TS;

    // Normal input is left at its default (tangent-space, unconnected = the
    // mesh's own per-vertex normal from CachedNormals) -- correct as-is now
    // that every face has a real, correctly-oriented copy.

    Mat->PreEditChange(nullptr);
    Mat->PostEditChange();

    // Register with the asset system and save to disk.
    FAssetRegistryModule::AssetCreated(Mat);
    Package->MarkPackageDirty();

    const FString FilePath = FPackageName::LongPackageNameToFilename(
        AssetPath, FPackageName::GetAssetPackageExtension());
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    UPackage::SavePackage(Package, Mat, *FilePath, Args);

    UE_LOG(LogTemp, Log, TEXT("ParaViewLink: created M_ScalarField at %s"), *FilePath);
    return Mat;
}
#endif // WITH_EDITOR

// ---------------------------------------------------------------------------
// Poll thread: receives events from the DataManager client, assembles batches
// and builds their geometry, so the game thread only applies them.
// ---------------------------------------------------------------------------

namespace
{
    FString Utf8(const char* S) { return S ? FString(UTF8_TO_TCHAR(S)) : FString(); }
}

class FDMPollRunnable : public FRunnable
{
public:
    FDMPollRunnable(dm_client* InClient, float InPointSize, float InLineWidth)
        : Client(InClient), PointSize(InPointSize), LineWidth(InLineWidth) {}

    TQueue<TUniquePtr<FDMBatch>, EQueueMode::Spsc> Batches;
    TQueue<FString, EQueueMode::Spsc> Messages;     // log lines for the game thread
    FThreadSafeBool bConnected = false;
    FThreadSafeBool bStop = false;
    FString Project;                                 // written before bConnected is set
    std::atomic<bool> bHaveState { false };
    FCriticalSection StateLock;
    dm_playback_state State {};

    virtual uint32 Run() override
    {
        while (!bStop)
        {
            dm_event Ev;
            if (!dm_poll(Client, &Ev))
            {
                FPlatformProcess::Sleep(0.002f);
                continue;
            }
            Handle(Ev);
            dm_release(Client, &Ev);
        }
        return 0;
    }
    virtual void Stop() override { bStop = true; }

private:
    struct FRawItem
    {
        FString Key;
        TArray<char> Meta;
        TArray<uint8> Data;
    };

    dm_client* Client;
    float PointSize, LineWidth;
    TUniquePtr<FDMBatch> Building;
    TArray<FRawItem> RawKeyData;
    TSet<FString> LastManifest, Manifest;
    FBox DomainPV = FBox(ForceInit);

    void Handle(const dm_event& Ev)
    {
        switch (Ev.type)
        {
        case DM_EV_CONNECTED:
            Project = Utf8(Ev.message);
            bConnected = true;
            Messages.Enqueue(FString::Printf(TEXT("connected to DataManager, project '%s'"), *Project));
            break;
        case DM_EV_DISCONNECTED:
            bConnected = false;
            Building.Reset();
            RawKeyData.Reset();
            Messages.Enqueue(FString::Printf(TEXT("DataManager connection lost (%s); reconnecting"), *Utf8(Ev.message)));
            break;
        case DM_EV_REJECTED:
        case DM_EV_ERROR:
            Messages.Enqueue(FString::Printf(TEXT("DataManager: %s"), *Utf8(Ev.message)));
            break;
        case DM_EV_PLAYBACK_STATE:
        {
            FScopeLock L(&StateLock);
            State = Ev.state;
            State.project = nullptr;   // not valid after release
            bHaveState = true;
            break;
        }
        case DM_EV_BATCH_BEGIN:
            Building = MakeUnique<FDMBatch>();
            Building->Id = Ev.batch_id;
            Building->Time = Ev.time;
            RawKeyData.Reset();
            break;
        case DM_EV_MANIFEST:
            Manifest.Reset();
            for (uint32 i = 0; i < Ev.count; ++i) Manifest.Add(Utf8(Ev.keys[i]));
            break;
        case DM_EV_ITEM:
            if (Building) HandleItem(Ev);
            break;
        case DM_EV_BATCH_END:
            if (Building && Building->Id == Ev.batch_id) FinishBatch();
            break;
        default:
            break;
        }
    }

    void HandleItem(const dm_event& Ev)
    {
        const FString Key = Utf8(Ev.key);
        int32 Slash;
        if (!Key.FindChar(TEXT('/'), Slash)) return;
        const FString Type = Key.Left(Slash), Name = Key.Mid(Slash + 1);
        if (Type == TEXT("KeyData"))
        {
            // Built at the end of the batch, once the domain size is known.
            FRawItem& R = RawKeyData.AddDefaulted_GetRef();
            R.Key = Key;
            R.Meta.Append(Ev.meta, Ev.meta_length);
            R.Data.Append(Ev.data, int64(Ev.data_length));
        }
        else if (Type == TEXT("Colormap"))
        {
            FDMColormap C;
            if (PVLinkKeyData::ParseColormap(Ev.data, Ev.data_length, C)) Building->Colormaps.Add(MoveTemp(C));
        }
        else if (Type == TEXT("Bounds"))
        {
            FBox B;
            if (PVLinkKeyData::ParseBounds(Ev.data, Ev.data_length, B))
            {
                DomainPV = B;
                Building->Bounds = FBox(PVLinkKeyData::ToUnreal(B.Min.X, B.Max.Y, B.Min.Z),
                                        PVLinkKeyData::ToUnreal(B.Max.X, B.Min.Y, B.Max.Z));
            }
        }
        else if (Type == TEXT("Visibility"))
        {
            Building->Visibility.Add(Name, Ev.data_length > 0 && Ev.data[0] != 0);
        }
    }

    void FinishBatch()
    {
        if (!DomainPV.IsValid)
            for (const FRawItem& R : RawKeyData)
            {
                FBox B;
                if (PVLinkKeyData::HeaderBounds(R.Meta.GetData(), R.Meta.Num(), B)) DomainPV += B;
            }
        const float Diag = DomainPV.IsValid ? float(DomainPV.GetSize().Size()) : 1.f;
        for (const FRawItem& R : RawKeyData)
        {
            FDMMesh M;
            FString Err;
            const FString Name = R.Key.Mid(8);   // after "KeyData/"
            if (PVLinkKeyData::Build(Name, R.Meta.GetData(), R.Meta.Num(), R.Data.GetData(), R.Data.Num(),
                                     PointSize * Diag, LineWidth * Diag, M, Err))
                Building->Meshes.Add(MoveTemp(M));
            else if (!Err.IsEmpty())
                // Not reported as dropped: a malformed item would only be resent.
                Messages.Enqueue(FString::Printf(TEXT("%s: %s"), *R.Key, *Err));
        }
        RawKeyData.Reset();
        for (const FString& K : LastManifest)
            if (!Manifest.Contains(K)) Building->Removed.Add(K);
        LastManifest = Manifest;
        Batches.Enqueue(MoveTemp(Building));
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void UMeshReceiverSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    BaseScalarMaterial = LoadObject<UMaterial>(nullptr, TEXT("/ParaViewLink/M_ScalarField.M_ScalarField"));
#if WITH_EDITOR
    // First time the plugin is loaded in a new project: create and save the material.
    if (!BaseScalarMaterial)
        BaseScalarMaterial = CreateScalarFieldMaterial();
#endif
    if (!BaseScalarMaterial)
        UE_LOG(LogTemp, Warning, TEXT("MeshReceiver: M_ScalarField not found - colormap display unavailable."));

    UE_LOG(LogTemp, Log, TEXT("MeshReceiver: DataManager client %s"), *Utf8(dm_version()));
    bActive = true;

    if (!LocalStorePath.IsEmpty())
    {
        FString Store = FPaths::IsRelative(LocalStorePath)
            ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / LocalStorePath) : LocalStorePath;
        char Err[512] = {};
        dm_client* C = dm_open_store(TCHAR_TO_UTF8(*Store), TCHAR_TO_UTF8(*Project), PlaybackRate, Err, sizeof Err);
        if (C)
        {
            UE_LOG(LogTemp, Log, TEXT("MeshReceiver: playing store %s"), *Store);
            StartClient(C);
        }
        else
            UE_LOG(LogTemp, Error, TEXT("MeshReceiver: cannot open store %s: %s"), *Store, *Utf8(Err));
    }
    else if (!DataManagerHost.IsEmpty())
        ConnectToDataManager(DataManagerHost, DataManagerPort);
    else
        StartDiscovery();
}

void UMeshReceiverSubsystem::Deinitialize()
{
    bActive = false;
    bDiscovering = false;
    if (Discovery.IsValid()) Discovery.Wait();
    StopClient();
    LiveActors.Empty();
    Colormaps.Empty();
    VarMaterials.Empty();
    ColormapRanges.Empty();
    Visible.Empty();
    Super::Deinitialize();
}

void UMeshReceiverSubsystem::StartClient(dm_client* NewClient)
{
    StopClient();
    Client = NewClient;
    Poller = new FDMPollRunnable(Client, PointSize, LineWidth);
    PollerThread = FRunnableThread::Create(Poller, TEXT("PVLink DataManager poll"));
}

void UMeshReceiverSubsystem::StopClient()
{
    if (PollerThread)
    {
        PollerThread->Kill(true);   // calls Stop(), waits for Run() to return
        delete PollerThread;
        PollerThread = nullptr;
    }
    delete Poller;
    Poller = nullptr;
    if (Client)
    {
        dm_close(Client);
        Client = nullptr;
    }
}

void UMeshReceiverSubsystem::ConnectToDataManager(const FString& Host, int32 Port)
{
    UE_LOG(LogTemp, Log, TEXT("MeshReceiver: connecting to DataManager %s:%d, project '%s'"), *Host, Port, *Project);
    const FString Peer = FString::Printf(TEXT("Unreal %s"), FPlatformProcess::ComputerName());
    StartClient(dm_connect(TCHAR_TO_UTF8(*Host), uint16(Port), TCHAR_TO_UTF8(*Project),
                           TCHAR_TO_UTF8(*Peer), "", bCompress ? DM_WANT_COMPRESSION : 0));
}

void UMeshReceiverSubsystem::StartDiscovery()
{
    bDiscovering = true;
    UE_LOG(LogTemp, Log, TEXT("MeshReceiver: looking for a DataManager on UDP %d"), DiscoveryPort);
    const int32 Port = DiscoveryPort;
    TWeakObjectPtr<UMeshReceiverSubsystem> Self(this);
    Discovery = Async(EAsyncExecution::Thread, [this, Self, Port]()
    {
        int32 LastCount = -1;
        while (bDiscovering)
        {
            char Buf[16384] = {};
            const int32 Count = dm_discover(uint16(Port), 2500, Buf, sizeof Buf);
            if (!bDiscovering) return;
            if (Count == 1)
            {
                TArray<FString> F;
                FString(UTF8_TO_TCHAR(Buf)).TrimEnd().ParseIntoArray(F, TEXT("\t"), false);
                if (F.Num() >= 2)
                {
                    const FString Host = F[0];
                    const int32 DMPort = FCString::Atoi(*F[1]);
                    bDiscovering = false;
                    AsyncTask(ENamedThreads::GameThread, [Self, Host, DMPort]()
                    {
                        if (Self.IsValid() && Self->bActive) Self->ConnectToDataManager(Host, DMPort);
                    });
                    return;
                }
            }
            if (Count != LastCount && Count > 1)
                UE_LOG(LogTemp, Warning, TEXT("MeshReceiver: %d DataManagers announcing; set DataManagerHost to choose one"), Count);
            LastCount = Count;
        }
    });
}

bool UMeshReceiverSubsystem::IsConnected() const { return Poller && Poller->bConnected; }
void UMeshReceiverSubsystem::Play()  { if (Client) dm_play(Client); }
void UMeshReceiverSubsystem::Pause() { if (Client) dm_pause(Client); }
void UMeshReceiverSubsystem::Seek(float Time) { if (Client) dm_seek(Client, Time); }
void UMeshReceiverSubsystem::Step(int32 Delta) { if (Client) dm_step(Client, Delta); }
void UMeshReceiverSubsystem::SetRate(float R) { if (Client) dm_set_rate(Client, R); }

TStatId UMeshReceiverSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UMeshReceiverSubsystem, STATGROUP_Tickables);
}

// ---------------------------------------------------------------------------
// Game thread: apply at most one batch per tick, whole, then acknowledge it.
// ---------------------------------------------------------------------------

void UMeshReceiverSubsystem::Tick(float DeltaTime)
{
    if (!bActive || !Poller) return;
    FString Msg;
    while (Poller->Messages.Dequeue(Msg))
        UE_LOG(LogTemp, Log, TEXT("MeshReceiver: %s"), *Msg);

    if (bPlayOnStart && !bStartedPlaying && Poller->bHaveState)
    {
        bStartedPlaying = true;
        SetRate(PlaybackRate);
        Play();
    }

    TUniquePtr<FDMBatch> Batch;
    if (Poller->Batches.Dequeue(Batch))
    {
        ApplyBatch(*Batch);
        dm_ack(Client, Batch->Id, nullptr, 0);
    }
}

void UMeshReceiverSubsystem::ApplyBatch(FDMBatch& B)
{
    UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
    if (!World) return;

    // Removals.
    for (const FString& Key : B.Removed)
    {
        int32 Slash;
        if (!Key.FindChar(TEXT('/'), Slash)) continue;
        const FString Type = Key.Left(Slash), Name = Key.Mid(Slash + 1);
        if (Type == TEXT("KeyData"))
        {
            if (AStreamedMeshActor* A = LiveActors.FindRef(Name)) if (IsValid(A)) A->Destroy();
            LiveActors.Remove(Name);
        }
        else if (Type == TEXT("Visibility")) Visible.Remove(Name);
    }

    // Domain, colormaps, visibility.
    if (B.Bounds.IsSet())
    {
        StoredPVBounds = B.Bounds.GetValue();
        ComputeCoordTransform();
    }
    for (FDMColormap& C : B.Colormaps)
    {
        UTexture2D* Tex = CreateOrUpdateColormapTexture(C.Variable, C.RGB);
        Colormaps.Add(C.Variable, Tex);
        ColormapRanges.Add(C.Variable, FVector2f(C.Min, C.Max));
        UMaterialInstanceDynamic* MID = EnsureVarMaterial(C.Variable, Tex);
        for (auto& [Name, Actor] : LiveActors)
            if (IsValid(Actor) && Actor->GetActiveVariable() == C.Variable)
                Actor->SetActiveVariable(C.Variable, MID, C.Min, C.Max);
    }
    for (auto& [Name, bVis] : B.Visibility) Visible.Add(Name, bVis);

    // No domain bounds item: fit the data itself.
    if (!StoredPVBounds.IsValid && B.Meshes.Num() > 0)
    {
        for (const FDMMesh& M : B.Meshes) StoredPVBounds += FBox(M.Vertices);
        ComputeCoordTransform();
    }

    // Meshes. Every change of this batch lands in this tick, so it shows in one frame.
    static const FString Solid = TEXT("__solid");
    for (FDMMesh& M : B.Meshes)
    {
        AStreamedMeshActor* Actor = LiveActors.FindRef(M.Name);
        if (!IsValid(Actor))
        {
            Actor = World->SpawnActor<AStreamedMeshActor>();
#if WITH_EDITOR
            Actor->SetActorLabel(M.Name);
#endif
            LiveActors.Add(M.Name, Actor);
        }
        Actor->SetActorTransform(CoordTransform);
        // Scalars first: UpdateMesh re-applies the active variable to the new vertices.
        const bool bSolid = M.ColorBy.IsEmpty();
        const FString& Var = bSolid ? Solid : M.ColorBy;
        if (bSolid)
        {
            if (!Colormaps.Contains(Solid))
                Colormaps.Add(Solid, CreateOrUpdateColormapTexture(Solid, { SolidColor, SolidColor }));
            M.ColorValues.SetNumZeroed(M.Vertices.Num());
        }
        Actor->StoreScalars(Var, M.ColorValues);
        Actor->UpdateMesh(M.Vertices, M.Triangles, M.Normals);
        const FVector2f* R = bSolid ? nullptr : ColormapRanges.Find(Var);
        const FVector2f Range = bSolid ? FVector2f(0.f, 1.f) : R ? *R : M.DataRange;
        Actor->SetActiveVariable(Var, EnsureVarMaterial(Var, Colormaps.FindRef(Var)),
                                 Range.X, Range.Y > Range.X ? Range.Y : Range.X + 1.f);
    }
    ApplyVisibility();
    CurrentTime = B.Time;
    UE_LOG(LogTemp, Verbose, TEXT("MeshReceiver: batch %llu t=%g: %d mesh(es), %d removed, %d shown"),
           B.Id, B.Time, B.Meshes.Num(), B.Removed.Num(), LiveActors.Num());
}

void UMeshReceiverSubsystem::ApplyVisibility()
{
    for (auto& [Name, Actor] : LiveActors)
        if (IsValid(Actor))
        {
            const bool* V = Visible.Find(Name);
            Actor->SetActorHiddenInGame(V && !*V);
        }
}

// ---------------------------------------------------------------------------
// Materials and colormaps (game thread)
// ---------------------------------------------------------------------------

UMaterialInstanceDynamic* UMeshReceiverSubsystem::EnsureVarMaterial(
    const FString& VarName, UTexture2D* ColormapTex)
{
    UMaterialInstanceDynamic* MID = VarMaterials.FindRef(VarName);
    if (!MID || !IsValid(MID))
    {
        if (!BaseScalarMaterial)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("MeshReceiver: BaseScalarMaterial not assigned — assign it in the "
                     "Blueprint subclass of UMeshReceiverSubsystem"));
            return nullptr;
        }
        MID = UMaterialInstanceDynamic::Create(BaseScalarMaterial, this);
        VarMaterials.Add(VarName, MID);
        UE_LOG(LogTemp, Log, TEXT("MeshReceiver: Created MID for variable '%s'"), *VarName);
    }
    if (ColormapTex)
        MID->SetTextureParameterValue(TEXT("Colormap"), ColormapTex);
    return MID;
}

UTexture2D* UMeshReceiverSubsystem::CreateOrUpdateColormapTexture(
    const FString& VarName, const TArray<FLinearColor>& RGB)
{
    const int32 Width = RGB.Num();

    // Reuse existing texture if it exists and is the same size
    UTexture2D* Tex = Colormaps.FindRef(VarName);
    if (!Tex || Tex->GetSizeX() != Width)
    {
        Tex = UTexture2D::CreateTransient(Width, 1, PF_B8G8R8A8,
            FName(*FString::Printf(TEXT("Colormap_%s"), *VarName)));
        // ParaView's colormap RGB values are display-ready sRGB-encoded colors,
        // like any colormap LUT meant for direct screen display -- SRGB must be
        // true so the engine gamma-decodes them to linear before lighting.
        // Leaving this false (as it was) reads the sRGB-encoded bytes as if
        // already linear, which washes out contrast/saturation.
        Tex->SRGB = 1;
        Tex->CompressionSettings = TC_VectorDisplacementmap;
        Tex->Filter = TF_Bilinear;
        Tex->AddressX = TA_Clamp;   // prevent u=1.0 wrapping to the first texel
    }

    // Write BGRA8 data -- PF_B8G8R8A8's in-memory byte order is B,G,R,A, not
    // R,G,B,A (a very easy pixel-format gotcha to miss: writing R into byte 0
    // and B into byte 2 silently swaps red and blue, producing e.g. a reversed
    // cool-to-warm colormap that can look merely "off"/oversaturated rather
    // than obviously wrong).
    FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
    void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
    uint8* Pixels = static_cast<uint8*>(Data);
    for (int32 i = 0; i < Width; i++)
    {
        Pixels[i * 4 + 0] = FMath::Clamp(FMath::RoundToInt(RGB[i].B * 255.f), 0, 255);
        Pixels[i * 4 + 1] = FMath::Clamp(FMath::RoundToInt(RGB[i].G * 255.f), 0, 255);
        Pixels[i * 4 + 2] = FMath::Clamp(FMath::RoundToInt(RGB[i].R * 255.f), 0, 255);
        Pixels[i * 4 + 3] = 255;
    }
    Mip.BulkData.Unlock();
    Tex->UpdateResource();

    return Tex;
}

// ---------------------------------------------------------------------------
// Coordinate transform (game thread). StoredPVBounds is in Unreal's frame.
// ---------------------------------------------------------------------------

void UMeshReceiverSubsystem::ComputeCoordTransform()
{
    if (!StoredPVBounds.IsValid)
    {
        UE_LOG(LogTemp, Warning, TEXT("MeshReceiver: ComputeCoordTransform — no PV bounds stored yet"));
        return;
    }

    UWorld* World = GetGameInstance()->GetWorld();
    if (!World) return;

    // Find the first ASimContainerActor tagged "SimContainer".
    TArray<AActor*> Found;
    UGameplayStatics::GetAllActorsWithTag(World, FName("SimContainer"), Found);

    ASimContainerActor* Container = nullptr;
    for (AActor* A : Found)
    {
        Container = Cast<ASimContainerActor>(A);
        if (Container) break;
    }

    if (!Container)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("MeshReceiver: ComputeCoordTransform — no ASimContainerActor with tag "
                 "'SimContainer' in level.  Place one and re-send bounds from ParaView."));
        return;
    }

    const FVector PVCenter = StoredPVBounds.GetCenter();
    const FVector PVExtent = StoredPVBounds.GetExtent();   // half-size

    const FVector UECenter = Container->GetActorLocation();
    const FVector UEExtent = Container->ContainerBox->GetScaledBoxExtent();

    // Per-axis scale: maps PV half-extent → UE container half-extent.
    // Guard against degenerate (zero-size) PV dimensions.
    const FVector Scale(
        PVExtent.X > SMALL_NUMBER ? UEExtent.X / PVExtent.X : 1.f,
        PVExtent.Y > SMALL_NUMBER ? UEExtent.Y / PVExtent.Y : 1.f,
        PVExtent.Z > SMALL_NUMBER ? UEExtent.Z / PVExtent.Z : 1.f
    );

    // Translation: map PV center to UE container center.
    // FTransform applies: Result = Rotation * (Scale * P) + Translation
    // With identity rotation: Result = Scale * P + Translation
    // We want UECenter = Scale * PVCenter + Translation → solve for Translation.
    const FVector Translation(
        UECenter.X - Scale.X * PVCenter.X,
        UECenter.Y - Scale.Y * PVCenter.Y,
        UECenter.Z - Scale.Z * PVCenter.Z
    );

    CoordTransform = FTransform(FQuat::Identity, Translation, Scale);

    // Reposition any actors already in the world so bounds and mesh
    // can arrive in any order without leaving geometry misplaced.
    for (auto& [Name, Actor] : LiveActors)
        if (IsValid(Actor))
            Actor->SetActorTransform(CoordTransform);

    UE_LOG(LogTemp, Verbose,
        TEXT("MeshReceiver: CoordTransform set — "
             "Scale=(%.3g, %.3g, %.3g)  Translate=(%.1f, %.1f, %.1f)"),
        Scale.X, Scale.Y, Scale.Z,
        Translation.X, Translation.Y, Translation.Z);
}
