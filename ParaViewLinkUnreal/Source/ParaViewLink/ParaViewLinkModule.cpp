#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

/** Loads the DataManager client library (delay-loaded on Windows) before first use. */
class FParaViewLinkModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
#if PLATFORM_WINDOWS
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ParaViewLink"));
        if (!Plugin.IsValid()) return;
        const FString Base = Plugin->GetBaseDir();
        for (const FString& Candidate : {
                 FPaths::Combine(Base, TEXT("Binaries/Win64/dmcore.dll")),
                 FPaths::Combine(Base, TEXT("Source/ThirdParty/DataManager/lib/Win64/dmcore.dll")) })
        {
            if (FPaths::FileExists(Candidate) && (Handle = FPlatformProcess::GetDllHandle(*Candidate)) != nullptr)
                return;
        }
        UE_LOG(LogTemp, Error, TEXT("ParaViewLink: cannot load dmcore.dll"));
#endif
    }

    virtual void ShutdownModule() override
    {
        if (Handle)
        {
            FPlatformProcess::FreeDllHandle(Handle);
            Handle = nullptr;
        }
    }

private:
    void* Handle = nullptr;
};

IMPLEMENT_MODULE(FParaViewLinkModule, ParaViewLink)
