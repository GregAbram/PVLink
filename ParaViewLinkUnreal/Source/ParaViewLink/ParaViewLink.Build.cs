using UnrealBuildTool;

public class ParaViewLink : ModuleRules
{
    public ParaViewLink(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // All plugin source files sit directly in the module directory.
        // Public/ and Private/ subdirectories are optional — if you prefer
        // to keep headers and .cpp files together, leave this as-is.
        PublicIncludePaths.Add(ModuleDirectory);

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Sockets",
            "Networking",
            "ProceduralMeshComponent",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
        });

        // Editor-only dependencies for auto-generating the M_ScalarField material asset.
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "UnrealEd",
                "AssetRegistry",
            });
        }
    }
}
