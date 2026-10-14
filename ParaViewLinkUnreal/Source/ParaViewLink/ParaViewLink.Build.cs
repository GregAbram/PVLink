using System.IO;
using UnrealBuildTool;

public class ParaViewLink : ModuleRules
{
    public ParaViewLink(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // All plugin source files sit directly in the module directory.
        PublicIncludePaths.Add(ModuleDirectory);

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Json",
            "ProceduralMeshComponent",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",   // IPluginManager, to find the DataManager library
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

        // DataManager client library (C API; see ThirdParty/DataManager/README.md).
        string DM = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "ThirdParty", "DataManager"));
        PublicIncludePaths.Add(Path.Combine(DM, "include"));
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string Lib = Path.Combine(DM, "lib", "Win64");
            PublicAdditionalLibraries.Add(Path.Combine(Lib, "dmcore.lib"));
            PublicDelayLoadDLLs.Add("dmcore.dll");   // loaded explicitly in StartupModule
            RuntimeDependencies.Add("$(BinaryOutputDir)/dmcore.dll", Path.Combine(Lib, "dmcore.dll"));
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string Dylib = Path.Combine(DM, "lib", "Mac", "libdmcore.dylib");
            PublicAdditionalLibraries.Add(Dylib);
            RuntimeDependencies.Add("$(BinaryOutputDir)/libdmcore.dylib", Dylib);
        }
        else if (Target.Platform == UnrealTargetPlatform.Android)
        {
            PublicAdditionalLibraries.Add(Path.Combine(DM, "lib", "Android", "arm64-v8a", "libdmcore.so"));
            AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(ModuleDirectory, "ParaViewLink_APL.xml"));
        }
    }
}
