#include "MeshReceiverSubsystem.h"
#include "SimContainerActor.h"
#include "StreamedMeshActor.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
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
// Lifecycle
// ---------------------------------------------------------------------------

void UMeshReceiverSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    UE_LOG(LogTemp, Warning, TEXT("MeshReceiverSubsystem: Initializing"));

    BaseScalarMaterial = LoadObject<UMaterial>(nullptr,
        TEXT("/ParaViewLink/M_ScalarField.M_ScalarField"));

#if WITH_EDITOR
    // First time the plugin is loaded in a new project: auto-create the material
    // asset and save it into the plugin's Content folder.  Subsequent loads just
    // pick up the saved .uasset via the LoadObject call above.
    if (!BaseScalarMaterial)
        BaseScalarMaterial = CreateScalarFieldMaterial();
#endif

    if (BaseScalarMaterial)
    {
        UE_LOG(LogTemp, Warning, TEXT("MeshReceiverSubsystem: Loaded M_ScalarField"));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("MeshReceiverSubsystem: M_ScalarField not found — colormap display unavailable."));
    }

    // Create the flip-sync event (auto-reset so each Wait() consumes one Trigger()).
    FlipEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/false);

    UE_LOG(LogTemp, Warning, TEXT("MeshReceiverSubsystem: Ready"));
    bActive = true;
}

void UMeshReceiverSubsystem::Deinitialize()
{
    // Stop ticking immediately so no game-thread Tick() fires after this point.
    bActive = false;

    // If the IO thread is blocked in HandleRawMessage waiting for the flip event,
    // unblock it so the thread can exit cleanly when Super calls Stop() + WaitForCompletion().
    if (FlipEvent)
        FlipEvent->Trigger();

    // Stop the network thread and wait for it to exit BEFORE touching any state.
    // Super::Deinitialize() clears OnMessageReceived, calls Stop(), then
    // WaitForCompletion() — so when it returns the receiver thread is fully gone
    // and HandleRawMessage() can never be called again.
    Super::Deinitialize();

    // Return the event to the pool now that the IO thread is guaranteed gone.
    if (FlipEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(FlipEvent);
        FlipEvent = nullptr;
    }

    // Now it is safe to drain and clear. The thread is dead, the callback is
    // null — nothing can enqueue new commands or hold references to our UObjects.
    { TUniquePtr<FPendingCommand> Cmd; while (InstallQueue.Dequeue(Cmd)) {} }

    LiveActors.Empty();
    PendingMeshes.Empty();
    PendingPVData.Empty();
    Colormaps.Empty();
    VarMaterials.Empty();
    ColormapRanges.Empty();
}

// ---------------------------------------------------------------------------
// Network thread — dispatch incoming messages
// ---------------------------------------------------------------------------

void UMeshReceiverSubsystem::HandleRawMessage(int32 Cmd, TArray<uint8> Payload)
{
    auto PendCmd = MakeUnique<FPendingCommand>();

    if (Cmd == MeshCmd::Mesh)
    {
        FParsedMeshData Data = ParseMeshPayload(Payload);
        if (Data.Name.IsEmpty())
        {
            UE_LOG(LogTemp, Error, TEXT("MeshReceiver: Failed to parse CMD_MESH"));
            return;
        }
        PendCmd->Type     = EPendingCmdType::Mesh;
        PendCmd->MeshData = MoveTemp(Data);
    }
    else if (Cmd == MeshCmd::Update)
    {
        PendCmd->Type = EPendingCmdType::Flip;
        InstallQueue.Enqueue(MoveTemp(PendCmd));

        // Block the IO thread here until Tick() completes the buffer swap and
        // triggers FlipEvent.  SocketReceiverRunnable::Run() sends the ack to
        // ParaView only after this function returns, so this ensures the ack is
        // not sent until the flip is fully done.
        if (FlipEvent)
            FlipEvent->Wait();
        return;
    }
    else if (Cmd == MeshCmd::Scalars)
    {
        FString MeshName, VarName;
        TArray<float> Scalars;
        if (!ParseScalarsPayload(Payload, MeshName, VarName, Scalars))
        {
            UE_LOG(LogTemp, Error, TEXT("MeshReceiver: Failed to parse CMD_SCALARS"));
            return;
        }
        PendCmd->Type         = EPendingCmdType::Scalars;
        PendCmd->MeshName     = MoveTemp(MeshName);
        PendCmd->VariableName = MoveTemp(VarName);
        PendCmd->Scalars      = MoveTemp(Scalars);
    }
    else if (Cmd == MeshCmd::Variable)
    {
        FString MeshName, VarName;
        if (!ParseVariablePayload(Payload, MeshName, VarName))
        {
            UE_LOG(LogTemp, Error, TEXT("MeshReceiver: Failed to parse CMD_VARIABLE"));
            return;
        }
        PendCmd->Type         = EPendingCmdType::Variable;
        PendCmd->MeshName     = MoveTemp(MeshName);
        PendCmd->VariableName = MoveTemp(VarName);
    }
    else if (Cmd == MeshCmd::Ping)
    {
        // Handled entirely on the network thread — no game-thread work needed.
        // Payload is an optional UTF-8 message; empty payload is also fine.
        FString Msg;
        if (Payload.Num() > 0)
        {
            Payload.Add(0);
            Msg = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Payload.GetData()));
        }
        UE_LOG(LogTemp, Warning, TEXT("MeshReceiver: PING%s%s"),
            Msg.IsEmpty() ? TEXT("") : TEXT(" — "), *Msg);
        return;   // ack already sent by the socket layer; nothing to enqueue
    }
    else if (Cmd == MeshCmd::Colormap)
    {
        FString VarName;
        float Min = 0.f, Max = 1.f;
        TArray<FLinearColor> RGB;
        if (!ParseColormapPayload(Payload, VarName, Min, Max, RGB))
        {
            UE_LOG(LogTemp, Error, TEXT("MeshReceiver: Failed to parse CMD_COLORMAP"));
            return;
        }
        PendCmd->Type         = EPendingCmdType::Colormap;
        PendCmd->VariableName = MoveTemp(VarName);
        PendCmd->ColormapMin  = Min;
        PendCmd->ColormapMax  = Max;
        PendCmd->ColormapRGB  = MoveTemp(RGB);
    }
    else if (Cmd == MeshCmd::PVMesh)
    {
        FParsedMeshData Mesh;
        TArray<float>   Scalars;
        float           ScalarMin = 0.f, ScalarMax = 1.f;
        FString         ColorName;
        int32           ScalarLocation = -1;

        if (!ParsePVMeshPayload(Payload, Mesh, Scalars, ScalarMin, ScalarMax,
                                ColorName, ScalarLocation))
        {
            UE_LOG(LogTemp, Error, TEXT("MeshReceiver: Failed to parse CMD_PVMESH"));
            return;
        }
        PendCmd->Type            = EPendingCmdType::PVMesh;
        PendCmd->MeshData        = MoveTemp(Mesh);
        PendCmd->Scalars         = MoveTemp(Scalars);
        PendCmd->ColormapMin     = ScalarMin;
        PendCmd->ColormapMax     = ScalarMax;
        PendCmd->VariableName    = MoveTemp(ColorName);
        PendCmd->ScalarLocation  = ScalarLocation;
    }
    else if (Cmd == MeshCmd::Bounds)
    {
        FBox Bounds(ForceInit);
        if (!ParseBoundsPayload(Payload, Bounds))
        {
            UE_LOG(LogTemp, Error, TEXT("MeshReceiver: Failed to parse CMD_BOUNDS"));
            return;
        }
        PendCmd->Type      = EPendingCmdType::Bounds;
        PendCmd->BoundsBox = Bounds;
    }
    else if (Cmd == MeshCmd::Visibility)
    {
        FString MeshName;
        bool bVisible = true;
        if (!ParseVisibilityPayload(Payload, MeshName, bVisible))
        {
            UE_LOG(LogTemp, Error, TEXT("MeshReceiver: Failed to parse CMD_VISIBILITY"));
            return;
        }
        PendCmd->Type     = EPendingCmdType::Visibility;
        PendCmd->MeshName = MoveTemp(MeshName);
        PendCmd->bVisible = bVisible;
    }
    else
    {
        Super::HandleRawMessage(Cmd, MoveTemp(Payload));
        return;
    }

    InstallQueue.Enqueue(MoveTemp(PendCmd));
}

// ---------------------------------------------------------------------------
// Game thread tick — drain the install queue
// ---------------------------------------------------------------------------

void UMeshReceiverSubsystem::Tick(float DeltaTime)
{
    TUniquePtr<FPendingCommand> Cmd;
    while (InstallQueue.Dequeue(Cmd))
    {
        switch (Cmd->Type)
        {
        case EPendingCmdType::Mesh:
        {
            FString Name = Cmd->MeshData.Name;
            PendingMeshes.Add(Name, MoveTemp(Cmd->MeshData));
            break;
        }
        case EPendingCmdType::Flip:
        {
            UWorld* World = GetGameInstance()->GetWorld();
            if (World)
            {
                // Classic CMD_MESH path — geometry only, no scalars bundled.
                for (auto& [Name, Data] : PendingMeshes)
                    InstallOrUpdate(World, Name, Data);

                // CMD_PVMESH path — geometry + scalars in one packet.
                for (auto& [Name, PVCmd] : PendingPVData)
                {
                    InstallOrUpdate(World, Name, PVCmd.MeshData);

                    if (PVCmd.ScalarLocation >= 0 && !PVCmd.VariableName.IsEmpty()
                        && PVCmd.Scalars.Num() > 0)
                    {
                        ColormapRanges.Add(PVCmd.VariableName,
                                           FVector2f(PVCmd.ColormapMin, PVCmd.ColormapMax));
                        TArray<float> ScalarsCopy = PVCmd.Scalars;
                        HandleScalarsCmd(Name, PVCmd.VariableName, ScalarsCopy);
                        HandleVariableCmd(Name, PVCmd.VariableName);
                    }
                }

                int32 Total = PendingMeshes.Num() + PendingPVData.Num();
                UE_LOG(LogTemp, Warning,
                    TEXT("MeshReceiver: Flip installed %d mesh(es)"), Total);
            }
            PendingMeshes.Empty();
            PendingPVData.Empty();

            // Unblock the IO thread so SocketReceiverRunnable can send the
            // UPDATE ack to ParaView.
            if (FlipEvent)
                FlipEvent->Trigger();
            break;
        }
        case EPendingCmdType::Scalars:
            HandleScalarsCmd(Cmd->MeshName, Cmd->VariableName, Cmd->Scalars);
            break;

        case EPendingCmdType::Variable:
            HandleVariableCmd(Cmd->MeshName, Cmd->VariableName);
            break;

        case EPendingCmdType::Colormap:
            HandleColormapCmd(Cmd->VariableName, Cmd->ColormapMin, Cmd->ColormapMax,
                              Cmd->ColormapRGB);
            break;

        case EPendingCmdType::PVMesh:
        {
            // Buffer — do not install yet.  The next Flip (CMD_UPDATE) will
            // drain both PendingMeshes and PendingPVData together so all meshes
            // from one ParaView pipeline execution become active atomically.
            const FString& Name = Cmd->MeshData.Name;
            PendingPVData.Add(Name, MoveTemp(*Cmd));
            UE_LOG(LogTemp, Log, TEXT("MeshReceiver: PVMesh '%s' buffered"), *Name);
            break;
        }

        case EPendingCmdType::Visibility:
        {
            AStreamedMeshActor** ActorPtr = LiveActors.Find(Cmd->MeshName);
            if (ActorPtr && IsValid(*ActorPtr))
            {
                (*ActorPtr)->SetActorHiddenInGame(!Cmd->bVisible);
                UE_LOG(LogTemp, Log, TEXT("MeshReceiver: '%s' %s"),
                    *Cmd->MeshName, Cmd->bVisible ? TEXT("shown") : TEXT("hidden"));
            }
            break;
        }

        case EPendingCmdType::Bounds:
        {
            StoredPVBounds = Cmd->BoundsBox;
            FVector Min = StoredPVBounds.Min;
            FVector Max = StoredPVBounds.Max;
            UE_LOG(LogTemp, Log,
                TEXT("MeshReceiver: Domain bounds received  "
                     "X[%.4g, %.4g]  Y[%.4g, %.4g]  Z[%.4g, %.4g]"),
                Min.X, Max.X, Min.Y, Max.Y, Min.Z, Max.Z);
            ComputeCoordTransform();
            break;
        }
        }
    }
}

TStatId UMeshReceiverSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UMeshReceiverSubsystem, STATGROUP_Tickables);
}

// ---------------------------------------------------------------------------
// Game-thread handlers
// ---------------------------------------------------------------------------

void UMeshReceiverSubsystem::InstallOrUpdate(UWorld* World,
                                              const FString& Name,
                                              const FParsedMeshData& Data)
{
    AStreamedMeshActor** Existing = LiveActors.Find(Name);
    if (Existing && IsValid(*Existing))
    {
        (*Existing)->SetActorTransform(CoordTransform);
        (*Existing)->UpdateMesh(Data.Vertices, Data.Triangles, Data.Normals);
        UE_LOG(LogTemp, Log, TEXT("MeshReceiver: Updated '%s'"), *Name);
    }
    else
    {
        AStreamedMeshActor* Actor = World->SpawnActor<AStreamedMeshActor>();
        Actor->SetActorLabel(Name);
        Actor->SetActorTransform(CoordTransform);
        Actor->UpdateMesh(Data.Vertices, Data.Triangles, Data.Normals);
        LiveActors.Add(Name, Actor);
        UE_LOG(LogTemp, Log, TEXT("MeshReceiver: Spawned '%s'"), *Name);
    }
}

void UMeshReceiverSubsystem::HandleScalarsCmd(const FString& MeshName,
                                               const FString& VarName,
                                               TArray<float>& Scalars)
{
    AStreamedMeshActor** ActorPtr = LiveActors.Find(MeshName);
    if (!ActorPtr || !IsValid(*ActorPtr))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("MeshReceiver: CMD_SCALARS - mesh '%s' not found, scalars will be lost"),
            *MeshName);
        return;
    }
    (*ActorPtr)->StoreScalars(VarName, Scalars);
}

void UMeshReceiverSubsystem::HandleVariableCmd(const FString& MeshName,
                                                const FString& VarName)
{
    AStreamedMeshActor** ActorPtr = LiveActors.Find(MeshName);
    if (!ActorPtr || !IsValid(*ActorPtr))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("MeshReceiver: CMD_VARIABLE - mesh '%s' not found"), *MeshName);
        return;
    }

    UMaterialInstanceDynamic* MID = VarMaterials.FindRef(VarName);

    // If the colormap message hasn't arrived yet (or EnsureVarMaterial failed
    // earlier), try to create the MID now with whatever texture we have.
    if (!MID)
    {
        UTexture2D* Tex = Colormaps.FindRef(VarName);
        MID = EnsureVarMaterial(VarName, Tex);
    }

    // Use the colormap range if one has been received; fall back to [0,1].
    // When the colormap arrives later, HandleColormapCmd will re-upload with
    // the correct range.
    FVector2f Range = ColormapRanges.FindRef(VarName);
    float Min = Range.X;
    float Max = (Range.Y > Range.X) ? Range.Y : Range.X + 1.f;

    (*ActorPtr)->SetActiveVariable(VarName, MID, Min, Max);
}

void UMeshReceiverSubsystem::HandleColormapCmd(const FString& VarName,
                                                float Min, float Max,
                                                TArray<FLinearColor>& RGB)
{
    UTexture2D* Tex = CreateOrUpdateColormapTexture(VarName, RGB);
    Colormaps.Add(VarName, Tex);
    ColormapRanges.Add(VarName, FVector2f(Min, Max));

    UMaterialInstanceDynamic* MID = EnsureVarMaterial(VarName, Tex);

    // Re-upload vertex colours for any live actor currently displaying this variable
    // so its texture coordinates reflect the new range.
    for (auto& [Name, Actor] : LiveActors)
    {
        if (IsValid(Actor) && Actor->GetActiveVariable() == VarName)
            Actor->SetActiveVariable(VarName, MID, Min, Max);
    }

    UE_LOG(LogTemp, Log, TEXT("MeshReceiver: Updated colormap '%s' [%.4g, %.4g] (%d samples)"),
        *VarName, Min, Max, RGB.Num());
}

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
// Coordinate transform  (game thread)
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

    UE_LOG(LogTemp, Log,
        TEXT("MeshReceiver: CoordTransform set — "
             "Scale=(%.3g, %.3g, %.3g)  Translate=(%.1f, %.1f, %.1f)"),
        Scale.X, Scale.Y, Scale.Z,
        Translation.X, Translation.Y, Translation.Z);
}

// ---------------------------------------------------------------------------
// Payload parsers  (network thread — no UObject access)
// ---------------------------------------------------------------------------

bool UMeshReceiverSubsystem::ReadString(const uint8*& Ptr, const uint8* End, FString& OutStr)
{
    if (Ptr + 4 > End) return false;
    int32 Len = *reinterpret_cast<const int32*>(Ptr);
    Ptr += 4;
    if (Len < 0 || Ptr + Len > End) return false;
    TArray<uint8> Bytes(Ptr, Len);
    Bytes.Add(0);
    OutStr = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
    Ptr += Len;
    return true;
}

FParsedMeshData UMeshReceiverSubsystem::ParseMeshPayload(const TArray<uint8>& Payload)
{
    FParsedMeshData Data;
    const uint8* Ptr = Payload.GetData();
    const uint8* End = Ptr + Payload.Num();

    auto ReadInt32 = [&](int32& Out) -> bool {
        if (Ptr + 4 > End) return false;
        Out = *reinterpret_cast<const int32*>(Ptr);
        Ptr += 4;
        return true;
    };

    if (!ReadString(Ptr, End, Data.Name)) return Data;

    int32 VertexCount = 0;
    if (!ReadInt32(VertexCount) || VertexCount < 0 || Ptr + VertexCount * 12 > End)
        return FParsedMeshData{};

    Data.Vertices.SetNumUninitialized(VertexCount);
    for (int32 i = 0; i < VertexCount; i++)
    {
        float X = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        float Y = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        float Z = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        Data.Vertices[i] = FVector(X, Y, Z);
    }

    int32 IndexCount = 0;
    if (!ReadInt32(IndexCount) || IndexCount < 0 || Ptr + IndexCount * 4 > End)
        return FParsedMeshData{};

    Data.Triangles.SetNumUninitialized(IndexCount);
    FMemory::Memcpy(Data.Triangles.GetData(), Ptr, IndexCount * 4);
    Ptr += IndexCount * 4;

    // Normals — count of 0 means let UE auto-compute (backward-compatible)
    int32 NormalCount = 0;
    if (ReadInt32(NormalCount) && NormalCount > 0 && Ptr + NormalCount * 12 <= End)
    {
        Data.Normals.SetNumUninitialized(NormalCount);
        for (int32 i = 0; i < NormalCount; i++)
        {
            float X = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
            float Y = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
            float Z = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
            Data.Normals[i] = FVector(X, Y, Z);
        }
    }

    return Data;
}

bool UMeshReceiverSubsystem::ParseScalarsPayload(const TArray<uint8>& Payload,
                                                   FString& OutMeshName,
                                                   FString& OutVarName,
                                                   TArray<float>& OutScalars)
{
    const uint8* Ptr = Payload.GetData();
    const uint8* End = Ptr + Payload.Num();

    if (!ReadString(Ptr, End, OutMeshName)) return false;
    if (!ReadString(Ptr, End, OutVarName))  return false;

    if (Ptr + 4 > End) return false;
    int32 Count = *reinterpret_cast<const int32*>(Ptr); Ptr += 4;
    if (Count < 0 || Ptr + Count * 4 > End) return false;

    OutScalars.SetNumUninitialized(Count);
    FMemory::Memcpy(OutScalars.GetData(), Ptr, Count * 4);
    return true;
}

bool UMeshReceiverSubsystem::ParseVariablePayload(const TArray<uint8>& Payload,
                                                    FString& OutMeshName,
                                                    FString& OutVarName)
{
    const uint8* Ptr = Payload.GetData();
    const uint8* End = Ptr + Payload.Num();
    return ReadString(Ptr, End, OutMeshName) && ReadString(Ptr, End, OutVarName);
}

bool UMeshReceiverSubsystem::ParsePVMeshPayload(const TArray<uint8>& Payload,
                                                 FParsedMeshData& OutMesh,
                                                 TArray<float>&   OutScalars,
                                                 float&           OutScalarMin,
                                                 float&           OutScalarMax,
                                                 FString&         OutColorName,
                                                 int32&           OutScalarLocation)
{
    // Wire layout (all little-endian, produced by UE5MeshSender.py — protocol v1.1):
    //   int32   num_points
    //   int32   num_triangles
    //   int32   scalar_location   (-1=none, 0=per-point, 1=per-cell)
    //   int32   has_normals       (0=none, 1=per-point normals follow positions)
    //   int32   color_name_len
    //   int32   mesh_name_len
    //   float32[num_points * 3]   positions
    //   float32[num_points * 3]   normals   (only present if has_normals == 1)
    //   int32[num_triangles * 3]  triangle indices
    //   float32[num_scalars]      scalar values  (present iff scalar_location >= 0)
    //   float32[2]                [scalar_min, scalar_max]
    //   utf8[color_name_len]      color-array name
    //   utf8[mesh_name_len]       mesh/actor name

    const uint8* Ptr = Payload.GetData();
    const uint8* End = Ptr + Payload.Num();

    auto ReadInt32 = [&](int32& Out) -> bool {
        if (Ptr + 4 > End) return false;
        Out = *reinterpret_cast<const int32*>(Ptr);
        Ptr += 4;
        return true;
    };
    auto ReadFloat = [&](float& Out) -> bool {
        if (Ptr + 4 > End) return false;
        Out = *reinterpret_cast<const float*>(Ptr);
        Ptr += 4;
        return true;
    };
    auto ReadUtf8 = [&](FString& Out, int32 Len) -> bool {
        if (Len < 0 || Ptr + Len > End) return false;
        TArray<uint8> Bytes(Ptr, Len);
        Bytes.Add(0);
        Out = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
        Ptr += Len;
        return true;
    };

    // --- header ---
    int32 NumPoints, NumTris, ScalarLoc, HasNormals, ColorNameLen, MeshNameLen;
    if (!ReadInt32(NumPoints)     || NumPoints  < 0) return false;
    if (!ReadInt32(NumTris)       || NumTris    < 0) return false;
    if (!ReadInt32(ScalarLoc))                       return false;
    if (!ReadInt32(HasNormals))                      return false;
    if (!ReadInt32(ColorNameLen)  || ColorNameLen < 0) return false;
    if (!ReadInt32(MeshNameLen)   || MeshNameLen  < 0) return false;

    // --- positions ---
    if (Ptr + NumPoints * 12 > End) return false;
    OutMesh.Vertices.SetNumUninitialized(NumPoints);
    for (int32 i = 0; i < NumPoints; ++i)
    {
        float X = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        float Y = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        float Z = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        OutMesh.Vertices[i] = FVector(X, Y, Z);
    }

    // --- normals (optional, per-point) ---
    if (HasNormals == 1)
    {
        if (Ptr + NumPoints * 12 > End) return false;
        OutMesh.Normals.SetNumUninitialized(NumPoints);
        for (int32 i = 0; i < NumPoints; ++i)
        {
            float X = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
            float Y = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
            float Z = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
            OutMesh.Normals[i] = FVector(X, Y, Z);
        }
    }

    // --- triangle indices ---
    if (Ptr + NumTris * 12 > End) return false;   // 3 int32s per triangle
    OutMesh.Triangles.SetNumUninitialized(NumTris * 3);
    FMemory::Memcpy(OutMesh.Triangles.GetData(), Ptr, NumTris * 12);
    Ptr += NumTris * 12;

    // --- scalars (optional) ---
    OutScalarLocation = ScalarLoc;
    if (ScalarLoc >= 0)
    {
        int32 NumScalars = (ScalarLoc == 0) ? NumPoints : NumTris;
        if (Ptr + NumScalars * 4 + 8 > End) return false;
        OutScalars.SetNumUninitialized(NumScalars);
        FMemory::Memcpy(OutScalars.GetData(), Ptr, NumScalars * 4);
        Ptr += NumScalars * 4;
        if (!ReadFloat(OutScalarMin) || !ReadFloat(OutScalarMax)) return false;
    }
    else
    {
        OutScalars.Empty();
        OutScalarMin = 0.f;
        OutScalarMax = 1.f;
    }

    // --- names ---
    if (!ReadUtf8(OutColorName,    ColorNameLen)) return false;
    if (!ReadUtf8(OutMesh.Name,    MeshNameLen))  return false;
    if (OutMesh.Name.IsEmpty()) OutMesh.Name = TEXT("ParaViewMesh");

    return true;
}

bool UMeshReceiverSubsystem::ParseVisibilityPayload(const TArray<uint8>& Payload,
                                                     FString& OutMeshName,
                                                     bool& bOutVisible)
{
    // Payload: int32 name_len, utf8[name_len] mesh_name, int32 visible
    const uint8* Ptr = Payload.GetData();
    const uint8* End = Ptr + Payload.Num();
    if (!ReadString(Ptr, End, OutMeshName)) return false;
    if (Ptr + 4 > End) return false;
    bOutVisible = (*reinterpret_cast<const int32*>(Ptr) != 0);
    return true;
}

bool UMeshReceiverSubsystem::ParseBoundsPayload(const TArray<uint8>& Payload,
                                                 FBox& OutBounds)
{
    // Payload: float32[6]  { xmin, xmax, ymin, ymax, zmin, zmax }
    if (Payload.Num() < 24) return false;
    const float* F = reinterpret_cast<const float*>(Payload.GetData());
    OutBounds = FBox(
        FVector(F[0], F[2], F[4]),   // Min: xmin, ymin, zmin
        FVector(F[1], F[3], F[5])    // Max: xmax, ymax, zmax
    );
    return true;
}

bool UMeshReceiverSubsystem::ParseColormapPayload(const TArray<uint8>& Payload,
                                                    FString& OutVarName,
                                                    float& OutMin, float& OutMax,
                                                    TArray<FLinearColor>& OutRGB)
{
    const uint8* Ptr = Payload.GetData();
    const uint8* End = Ptr + Payload.Num();

    if (!ReadString(Ptr, End, OutVarName)) return false;

    // min and max data values that bracket the colormap ends
    if (Ptr + 8 > End) return false;
    OutMin = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
    OutMax = *reinterpret_cast<const float*>(Ptr); Ptr += 4;

    if (Ptr + 4 > End) return false;
    int32 Count = *reinterpret_cast<const int32*>(Ptr); Ptr += 4;
    if (Count < 0 || Ptr + Count * 12 > End) return false;   // 3 floats per sample

    OutRGB.SetNumUninitialized(Count);
    for (int32 i = 0; i < Count; i++)
    {
        float R = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        float G = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        float B = *reinterpret_cast<const float*>(Ptr); Ptr += 4;
        OutRGB[i] = FLinearColor(R, G, B, 1.f);
    }
    return true;
}
