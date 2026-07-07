#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "OrbitCameraPawn.generated.h"

class UCameraComponent;

/**
 * Viewport-style orbit camera.
 *
 *   Left-drag   — orbit (azimuth / elevation) around FocalPoint
 *   Middle-drag — pan FocalPoint in the camera's local right/up plane
 *   Scroll      — zoom (dolly) along the radial axis
 *
 * Designed to require no entries in Project Settings → Input.
 * Scroll is bound via BindKey(); orbit and pan are polled each tick
 * from the PlayerController so they work in any project without
 * action/axis mappings.
 *
 * Typical setup: in World Settings → Game Mode Override, set
 * Default Pawn Class to BP_OrbitCameraPawn (a Blueprint subclass
 * that exposes the feel parameters to the editor).
 */
UCLASS()
class GREG_API AOrbitCameraPawn : public APawn
{
    GENERATED_BODY()

public:
    AOrbitCameraPawn();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;
    virtual void SetupPlayerInputComponent(UInputComponent* IC) override;

    /**
     * Move the focal point to the actor's bounding-box centre and set
     * the orbit radius so the entire mesh fits comfortably in view.
     * Call this after CMD_UPDATE has spawned / updated the actors you
     * want to inspect.
     */
    UFUNCTION(BlueprintCallable, Category="Orbit")
    void FocusOnActor(AActor* Target);

    /** Reset orbit to the initial angle but keep the current focal point. */
    UFUNCTION(BlueprintCallable, Category="Orbit")
    void ResetView();

    // -----------------------------------------------------------------------
    // Feel parameters — safe to edit in a Blueprint subclass
    // -----------------------------------------------------------------------

    /** Degrees of rotation per pixel of mouse movement. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Feel")
    float OrbitSensitivity = 0.4f;

    /**
     * Pan speed expressed as a fraction of the current orbit radius per pixel.
     * Scales automatically so panning feels consistent at any zoom level.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Feel")
    float PanSensitivity = 0.002f;

    /** Fraction of the current radius added/removed per scroll click. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Feel")
    float ZoomFactor = 0.12f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Feel")
    float MinRadius = 1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Feel")
    float MaxRadius = 10000000.f;

    // -----------------------------------------------------------------------
    // Initial pose — set these before BeginPlay (e.g. in a subclass CDO)
    // -----------------------------------------------------------------------

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Initial")
    FVector InitialFocalPoint = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Initial")
    float InitialRadius = 500.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Initial")
    float InitialAzimuthDeg = 45.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Orbit|Initial")
    float InitialElevationDeg = 25.f;

private:
    UPROPERTY(VisibleAnywhere)
    UCameraComponent* Camera;

    // Current spherical state
    FVector FocalPoint = FVector::ZeroVector;
    float   Radius     = 500.f;
    float   AzimuthDeg = 45.f;
    float   ElevDeg    = 25.f;       // clamped to (-89, 89)

    void UpdateCameraTransform();

    // Scroll callbacks (BindKey — no project input settings needed)
    void OnZoomIn();
    void OnZoomOut();
};
