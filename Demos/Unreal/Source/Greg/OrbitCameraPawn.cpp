#include "OrbitCameraPawn.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AOrbitCameraPawn::AOrbitCameraPawn()
{
    PrimaryActorTick.bCanEverTick = true;

    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    SetRootComponent(Camera);

    // The camera's world rotation is computed explicitly in UpdateCameraTransform;
    // don't let the controller clobber it.
    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw   = false;
    bUseControllerRotationRoll  = false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AOrbitCameraPawn::BeginPlay()
{
    Super::BeginPlay();

    // Initialise from editable defaults
    FocalPoint = InitialFocalPoint;
    Radius     = FMath::Clamp(InitialRadius,     MinRadius, MaxRadius);
    AzimuthDeg = InitialAzimuthDeg;
    ElevDeg    = FMath::Clamp(InitialElevationDeg, -89.f, 89.f);
    UpdateCameraTransform();

    // Show the cursor so the user can see where they're clicking, and let
    // input reach both the viewport and any future HUD panels.
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        PC->bShowMouseCursor = true;
        FInputModeGameAndUI Mode;
        Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        Mode.SetHideCursorDuringCapture(false);
        PC->SetInputMode(Mode);
    }
}

// ---------------------------------------------------------------------------
// Tick — orbit and pan (polled so no project input mappings needed)
// ---------------------------------------------------------------------------

void AOrbitCameraPawn::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC) return;

    float MouseX = 0.f, MouseY = 0.f;
    PC->GetInputMouseDelta(MouseX, MouseY);
    if (MouseX == 0.f && MouseY == 0.f) return;

    const bool bLeft   = PC->IsInputKeyDown(EKeys::LeftMouseButton);
    const bool bMiddle = PC->IsInputKeyDown(EKeys::MiddleMouseButton);

    if (bLeft)
    {
        // Orbit: drag horizontally → azimuth, vertically → elevation
        AzimuthDeg += MouseX * OrbitSensitivity;
        ElevDeg    -= MouseY * OrbitSensitivity;   // screen Y is inverted
        ElevDeg     = FMath::Clamp(ElevDeg, -89.f, 89.f);
        UpdateCameraTransform();
    }
    else if (bMiddle)
    {
        // Pan: translate the focal point in the camera's local right/up plane.
        // Scale by radius so panning feels the same speed at any zoom level.
        const FRotator Rot   = Camera->GetComponentRotation();
        const FVector  Right = FRotationMatrix(Rot).GetScaledAxis(EAxis::Y);
        const FVector  Up    = FRotationMatrix(Rot).GetScaledAxis(EAxis::Z);
        FocalPoint -= Right * (MouseX * PanSensitivity * Radius);
        FocalPoint += Up    * (MouseY * PanSensitivity * Radius);
        UpdateCameraTransform();
    }
}

// ---------------------------------------------------------------------------
// Input — scroll only (BindKey works without project input settings)
// ---------------------------------------------------------------------------

void AOrbitCameraPawn::SetupPlayerInputComponent(UInputComponent* IC)
{
    Super::SetupPlayerInputComponent(IC);
    IC->BindKey(EKeys::MouseScrollUp,   IE_Pressed, this, &AOrbitCameraPawn::OnZoomIn);
    IC->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AOrbitCameraPawn::OnZoomOut);
}

void AOrbitCameraPawn::OnZoomIn()
{
    Radius = FMath::Clamp(Radius * (1.f - ZoomFactor), MinRadius, MaxRadius);
    UpdateCameraTransform();
}

void AOrbitCameraPawn::OnZoomOut()
{
    Radius = FMath::Clamp(Radius * (1.f + ZoomFactor), MinRadius, MaxRadius);
    UpdateCameraTransform();
}

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

void AOrbitCameraPawn::FocusOnActor(AActor* Target)
{
    if (!Target) return;
    FVector Origin, Extent;
    Target->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
    FocalPoint = Origin;
    Radius = FMath::Clamp(Extent.Size() * 2.5f, MinRadius, MaxRadius);
    UpdateCameraTransform();
    UE_LOG(LogTemp, Log, TEXT("OrbitCamera: focused on '%s', radius=%.1f"),
        *Target->GetActorLabel(), Radius);
}

void AOrbitCameraPawn::ResetView()
{
    FocalPoint = InitialFocalPoint;
    Radius     = FMath::Clamp(InitialRadius, MinRadius, MaxRadius);
    AzimuthDeg = InitialAzimuthDeg;
    ElevDeg    = FMath::Clamp(InitialElevationDeg, -89.f, 89.f);
    UpdateCameraTransform();
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void AOrbitCameraPawn::UpdateCameraTransform()
{
    // Convert spherical → Cartesian offset from FocalPoint
    const float AzRad = FMath::DegreesToRadians(AzimuthDeg);
    const float ElRad = FMath::DegreesToRadians(ElevDeg);
    const float CosEl = FMath::Cos(ElRad);

    const FVector Offset(
        Radius * CosEl * FMath::Cos(AzRad),
        Radius * CosEl * FMath::Sin(AzRad),
        Radius * FMath::Sin(ElRad)
    );

    const FVector CamPos = FocalPoint + Offset;
    SetActorLocation(CamPos);
    Camera->SetWorldRotation((FocalPoint - CamPos).ToOrientationRotator());
}
