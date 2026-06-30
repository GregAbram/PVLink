#include "SimContainerActor.h"
#include "Components/BoxComponent.h"

ASimContainerActor::ASimContainerActor()
{
    PrimaryActorTick.bCanEverTick = false;

    ContainerBox = CreateDefaultSubobject<UBoxComponent>(TEXT("ContainerBox"));
    SetRootComponent(ContainerBox);

    // Default: 100 cm cube (50 cm half-extent), matching UE's default cube primitive.
    // Resize by scaling the actor or editing Box Extent — both work.
    ContainerBox->SetBoxExtent(FVector(50.f));
    ContainerBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ContainerBox->ShapeColor = FColor(0, 200, 255);   // cyan wireframe

    // Hide the box at runtime — it's a design-time guide only.
    ContainerBox->SetHiddenInGame(true);

    // Tag used by UMeshReceiverSubsystem::ComputeCoordTransform() to locate this actor.
    Tags.Add(FName("SimContainer"));
}
 