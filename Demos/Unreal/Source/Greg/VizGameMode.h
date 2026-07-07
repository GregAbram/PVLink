#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "VizGameMode.generated.h"

/**
 * Minimal game mode for the ParaView → UE visualisation pipeline.
 * Sets AOrbitCameraPawn as the default pawn so players can orbit
 * streamed meshes out of the box.
 *
 * Assign this in Project Settings → Maps & Modes → Default Game Mode,
 * or per-level in World Settings → Game Mode Override.
 */
UCLASS()
class GREG_API AVizGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    AVizGameMode();
};
