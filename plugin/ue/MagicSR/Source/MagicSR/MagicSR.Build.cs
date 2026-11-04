using System.IO;
using UnrealBuildTool;

public class MagicSR : ModuleRules
{
    public MagicSR(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "RHI",
                "RenderCore",
                "Renderer",
                "Projects",
                "InputCore",
                "Slate",
                "SlateCore",
                "ApplicationCore"
            });

        // FViewInfo::ViewRect (internal render size) is in the renderer private headers.
        PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(Target.RelativeEnginePath, "Source", "Runtime", "Renderer", "Private")));
        PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(Target.RelativeEnginePath, "Source", "Runtime", "Renderer", "Internal")));

        // Needed for GetIVulkanDynamicRHI() (device / cmd buffer import).
        if (Target.Platform == UnrealTargetPlatform.Win64 ||
            Target.Platform == UnrealTargetPlatform.Linux)
        {
            PrivateDependencyModuleNames.Add("VulkanRHI");
            PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(Target.UEThirdPartySourceDirectory, "Vulkan", "Include")));
            PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(Target.UEThirdPartySourceDirectory, "Vulkan", "Include", "vulkan")));
        }

        string thirdPartyRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/MagicSR"));
        string includeDir = Path.Combine(thirdPartyRoot, "include");
        if (!Directory.Exists(includeDir))
        {
            throw new System.Exception("MagicSR ThirdParty include missing: " + includeDir);
        }
        PublicIncludePaths.Add(includeDir);

        PublicDefinitions.Add("MAGIC_SR_HAS_NSCALER=1");

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string winDir = Path.Combine(thirdPartyRoot, "windows");
            string importLib = Path.Combine(winDir, "libmagic_sr.dll.lib");
            string staticLib = Path.Combine(winDir, "libmagic_sr.lib");
            string dllPath = Path.Combine(winDir, "libmagic_sr.dll");

            // Prefer the delay-load import lib (libmagic_sr.dll.lib). Static
            // libmagic_sr.lib is the fallback. FG lives inside the 3.0.0 DLL;
            // do not also link magic_fg_vulkan.lib (duplicate MC_*FrameGen).
            if (File.Exists(importLib))
            {
                PublicAdditionalLibraries.Add(importLib);
            }
            else if (File.Exists(staticLib))
            {
                PublicAdditionalLibraries.Add(staticLib);
            }
            else
            {
                throw new System.Exception("MagicSR Windows library missing under " + winDir);
            }

            if (File.Exists(dllPath))
            {
                RuntimeDependencies.Add("$(BinaryOutputDir)/libmagic_sr.dll", dllPath);
                PublicDelayLoadDLLs.Add("libmagic_sr.dll");
            }

            PublicDefinitions.Add("MAGIC_SR_HAS_FRAMEGEN=1");

            string modelsDir = Path.Combine(thirdPartyRoot, "models");
            if (Directory.Exists(modelsDir))
            {
                foreach (string model in Directory.GetFiles(modelsDir, "*.bin"))
                {
                    string fileName = Path.GetFileName(model);
                    RuntimeDependencies.Add("$(ProjectDir)/Content/MagicSRModels/" + fileName, model);
                }
            }

            PublicSystemLibraries.AddRange(new[] { "d3d11.lib", "dxgi.lib", "opengl32.lib", "user32.lib", "gdi32.lib" });
            PublicDefinitions.Add("MAGIC_SR_WINDOWS=1");
            PublicDefinitions.Add("MAGIC_SR_LINUX=0");
            PublicDefinitions.Add("MAGIC_SR_ANDROID=0");
            PublicDefinitions.Add("MAGIC_SR_IOS=0");
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            string linuxDir = Path.Combine(thirdPartyRoot, "linux");
            string staticLib = Path.Combine(linuxDir, "libmagic_sr.a");
            string sharedLib = Path.Combine(linuxDir, "libmagic_sr.so");

            if (File.Exists(staticLib))
            {
                PublicAdditionalLibraries.Add(staticLib);
            }
            else if (File.Exists(sharedLib))
            {
                PublicAdditionalLibraries.Add(sharedLib);
                RuntimeDependencies.Add("$(BinaryOutputDir)/libmagic_sr.so", sharedLib);
            }
            else
            {
                throw new System.Exception("MagicSR Linux library missing under " + linuxDir);
            }

            PublicSystemLibraries.AddRange(new[] { "m", "pthread", "dl" });
            PublicDefinitions.Add("MAGIC_SR_WINDOWS=0");
            PublicDefinitions.Add("MAGIC_SR_LINUX=1");
            PublicDefinitions.Add("MAGIC_SR_ANDROID=0");
            PublicDefinitions.Add("MAGIC_SR_IOS=0");
        }
        else if (Target.Platform == UnrealTargetPlatform.Android)
        {
            PublicDefinitions.Add("MAGIC_SR_WINDOWS=0");
            PublicDefinitions.Add("MAGIC_SR_LINUX=0");
            PublicDefinitions.Add("MAGIC_SR_ANDROID=1");
            PublicDefinitions.Add("MAGIC_SR_IOS=0");
        }
        else if (Target.Platform == UnrealTargetPlatform.IOS)
        {
            PublicDefinitions.Add("MAGIC_SR_WINDOWS=0");
            PublicDefinitions.Add("MAGIC_SR_LINUX=0");
            PublicDefinitions.Add("MAGIC_SR_ANDROID=0");
            PublicDefinitions.Add("MAGIC_SR_IOS=1");
        }
        else
        {
            PublicDefinitions.Add("MAGIC_SR_WINDOWS=0");
            PublicDefinitions.Add("MAGIC_SR_LINUX=0");
            PublicDefinitions.Add("MAGIC_SR_ANDROID=0");
            PublicDefinitions.Add("MAGIC_SR_IOS=0");
        }

        bool supported =
            Target.Platform == UnrealTargetPlatform.Win64 ||
            Target.Platform == UnrealTargetPlatform.Linux;
        PublicDefinitions.Add(supported ? "MAGIC_SR_SUPPORTED=1" : "MAGIC_SR_SUPPORTED=0");

        // Measurement-only: MAGICSR_FG_BASELINE=1 compiles the pre-FrameGenLowRes default path
        // (no extra CVar read). Default 0 is the current plugin DLL.
        string baseline = System.Environment.GetEnvironmentVariable("MAGICSR_FG_BASELINE");
        PrivateDefinitions.Add(baseline == "1" ? "MAGICSR_FG_BASELINE=1" : "MAGICSR_FG_BASELINE=0");
    }
}
