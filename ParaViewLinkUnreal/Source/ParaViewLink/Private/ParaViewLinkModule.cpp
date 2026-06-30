#include "Modules/ModuleManager.h"

/**
 * Minimal module implementation — the plugin has no editor extensions or
 * startup/shutdown logic of its own.  All runtime behaviour lives in
 * UMeshReceiverSubsystem (a UGameInstanceSubsystem that auto-registers).
 */
IMPLEMENT_MODULE(FDefaultModuleImpl, ParaViewLink)
