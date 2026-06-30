#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SimContainerActor.generated.h"

class UBoxComponent;

/**
 * A designer-placed actor that defines where ParaView simulation data
 * should appear in UE world space.
 *
 * Usage:
 *   1. Drag this actor into any level.
 *   2. Scale and position it to cover the region you want the data to occupy.
 *   3. UMeshReceiverSubsystem finds it automatically via the "SimContainer" tag
 *      and computes CoordTransform whenever a domain bounds packet arrives from
 *      the UE5DomainBoundsFilter ParaView plugin.
 *
 * The box is rendered as a cyan wireframe in-editor and hidden at runtime.
 * Multiple instances are allowed; the subsystem uses the first one found.
 */
UCLASS(Blueprintable, BlueprintType, ClassGroup="ParaViewLink",
       meta=(DisplayName="Sim Container"))
class PARAVIEWLINK_API ASimContainerActor : public AActor
{
    GENERATED_BODY()

public:
    ASimContainerActor();

    /**
     * The box that defines the UE-world destination for the simulation data.
     * Resize it with the standard UE scale handles — extent is read at runtime
     * via GetScaledBoxExtent().
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ParaViewLink")
    UBoxComponent* ContainerBox;
};
