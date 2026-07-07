#include "VizGameMode.h"
#include "OrbitCameraPawn.h"

AVizGameMode::AVizGameMode()
{
    DefaultPawnClass = AOrbitCameraPawn::StaticClass();
}
