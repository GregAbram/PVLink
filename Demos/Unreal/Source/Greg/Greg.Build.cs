using UnrealBuildTool;
using System.IO;

public class Greg : ModuleRules
{
    public Greg(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // All viz source files sit directly in ModuleDirectory (Source/Greg/).
        // No subdirectories needed now that the game variants are removed.
        PublicIncludePaths.Add(ModuleDirectory);

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "InputCore",
            "Sockets", "Networking",
            "ProceduralMeshComponent",
            "EnhancedInput",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate", "SlateCore",
        });
    }
}
