#include "MagicSRModule.h"

#include "MagicSRSceneViewExtension.h"
#include "MagicSRViewportBridge.h"

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"
#include "Misc/Parse.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif

IMPLEMENT_MODULE(FMagicSRModule, MagicSR)

#if PLATFORM_WINDOWS
static void MagicSREnablePhysicalDpiForTours()
{
	int32 GpuProbe = 0;
	float PresentTestSec = 0.f;
	const bool bWant =
		FParse::Param(FCommandLine::Get(), TEXT("MagicSRYawTour")) ||
		FParse::Param(FCommandLine::Get(), TEXT("MagicSRFgTour")) ||
		FParse::Param(FCommandLine::Get(), TEXT("MagicSRFgPresentTest")) ||
		FParse::Value(FCommandLine::Get(), TEXT("-MagicSRFgPresentTest="), PresentTestSec) ||
		FParse::Param(FCommandLine::Get(), TEXT("MagicSRFgPerf")) ||
		FParse::Param(FCommandLine::Get(), TEXT("MagicSRStatic64")) ||
		FParse::Param(FCommandLine::Get(), TEXT("MagicSRTsrTour")) ||
		FParse::Param(FCommandLine::Get(), TEXT("MagicSRShotTour")) ||
		FParse::Param(FCommandLine::Get(), TEXT("MagicSRBench")) ||
		(FParse::Value(FCommandLine::Get(), TEXT("-MagicSRGpuProbe="), GpuProbe) && GpuProbe > 0);
	if (!bWant)
	{
		return;
	}
	// UnrealEditor -game follows the editor HDPI ini, which leaves this process
	// DPI-unaware. A 1920x1080 request is then scaled by 150% to 2880x1620 and
	// Windows shrinks or minimizes it. Set awareness before any window exists.
	const BOOL bOk = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] DPI_AWARE_V2 ok=%d err=%u"),
		   bOk ? 1 : 0, bOk ? 0u : GetLastError());
}

static void MagicSRLogLoadedModule(void* Handle)
{
	wchar_t ModPath[MAX_PATH];

	if (Handle == nullptr)
	{
		return;
	}
	ModPath[0] = 0;
	GetModuleFileNameW(static_cast<HMODULE>(Handle), ModPath, MAX_PATH);
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] module path=%s"), ModPath);
}
#include "Windows/HideWindowsPlatformTypes.h"
#endif

static void* TryLoadMagicDll(const FString& Path)
{
	if (!FPaths::FileExists(Path))
	{
		return nullptr;
	}
	return FPlatformProcess::GetDllHandle(*Path);
}

void FMagicSRModule::StartupModule()
{
#if PLATFORM_WINDOWS
	MagicSREnablePhysicalDpiForTours();
#endif
#if PLATFORM_WINDOWS && MAGIC_SR_SUPPORTED
	TArray<FString> Candidates;

	Candidates.Add(FPaths::Combine(FPaths::GetPath(FPlatformProcess::ExecutablePath()), TEXT("libmagic_sr.dll")));

	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MagicSR")))
	{
		Candidates.Add(FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("ThirdParty/MagicSR/windows/libmagic_sr.dll")));
	}

	Candidates.Add(FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins/MagicSR/ThirdParty/MagicSR/windows/libmagic_sr.dll")));

	void* Handle = nullptr;
	FString LoadedFrom;
	for (const FString& Path : Candidates)
	{
		Handle = TryLoadMagicDll(Path);
		if (Handle)
		{
			LoadedFrom = Path;
			break;
		}
	}

	if (Handle)
	{
		UE_LOG(LogTemp, Display, TEXT("[MagicSR] load %s -> ok"), *LoadedFrom);
		MagicSRLogLoadedModule(Handle);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[MagicSR] libmagic_sr.dll not found (tried %d paths)"),
			   Candidates.Num());
		for (const FString& Path : Candidates)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MagicSR]   missing: %s"), *Path);
		}
	}
#endif

	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MagicSR")))
	{
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/MagicSR"), ShaderDir);
	}

	// GEngine is null during plugin StartupModule — defer SVE registration.
	FCoreDelegates::GetOnPostEngineInit().AddLambda(
		[]()
		{
			if (FModuleManager::Get().IsModuleLoaded(TEXT("MagicSR")))
			{
				FMagicSRModule::Get().EnsureSceneViewExtension();
			}
		});
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] StartupModule (SVE deferred to PostEngineInit)"));
}

void FMagicSRModule::ShutdownModule()
{
	SetViewportTemporalEnabled(false);
	ENQUEUE_RENDER_COMMAND(MagicSRViewportShutdown)(
		[](FRHICommandListImmediate&)
		{
			FMagicSRViewportBridge::Get().DestroyHandle_RenderThread();
		});
	FlushRenderingCommands();
	FMagicSRViewportBridge::Get().ShutdownPresentHook();
	SceneViewExtension.Reset();
}

void FMagicSRModule::EnsureSceneViewExtension()
{
	if (SceneViewExtension.IsValid())
	{
		return;
	}
	if (GEngine == nullptr)
	{
		return;
	}
	SceneViewExtension = FSceneViewExtensions::NewExtension<FMagicSRSceneViewExtension>();
	FMagicSRViewportBridge::Get().EnsurePresentHook();
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] SceneViewExtension registered"));
}

namespace
{
TAutoConsoleVariable<float> CVarMagicSRSharpenAmount(
	TEXT("r.MagicSR.SharpenAmount"),
	0.0f,
	TEXT("Engine tonemapper sharpen while viewport SR is on. Default 0. Live-tunable; re-applied on next enable."),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<float> CVarMagicSRNativeSharpenAmount(
	TEXT("r.MagicSR.NativeSharpenAmount"),
	0.3f,
	TEXT("Engine tonemapper sharpen in Native compare mode."),
	ECVF_RenderThreadSafe);
} // namespace

void FMagicSRModule::SetViewportTemporalEnabled(bool bEnabled, float InScreenPercentage)
{
	EnsureSceneViewExtension();

	FMagicSRViewportBridge& Bridge = FMagicSRViewportBridge::Get();
	if (Bridge.IsEnabled() == bEnabled)
	{
		return;
	}

	IConsoleVariable* SP = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
	if (bEnabled)
	{
		if (IConsoleVariable* TAASamples =
				IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAASamples")))
		{
			TAASamples->Set(8, ECVF_SetByCode); // with SP=50 → ~32 unique jitters
		}
		if (SP && !SavedScreenPercentage.IsSet())
		{
			SavedScreenPercentage = SP->GetFloat();
			SP->Set(InScreenPercentage, ECVF_SetByCode);
		}
		// Vulkan temporal has no library sharpen pass: use the engine tonemapper
		// sharpen (already in the post chain, no new pass) while SR is active.
		if (IConsoleVariable* Sharpen =
				IConsoleManager::Get().FindConsoleVariable(TEXT("r.Tonemapper.Sharpen")))
		{
			if (!SavedTonemapSharpen.IsSet())
			{
				SavedTonemapSharpen = Sharpen->GetFloat();
			}
			Sharpen->Set(CVarMagicSRSharpenAmount.GetValueOnGameThread(), ECVF_SetByCode);
		}
		Bridge.SetEnabled(true);
		UE_LOG(LogTemp, Display,
			   TEXT("[MagicSR] Viewport Temporal ON (r.ScreenPercentage=%.1f)"),
			   InScreenPercentage);
	}
	else
	{
		Bridge.SetEnabled(false);
		if (SP && SavedScreenPercentage.IsSet())
		{
			SP->Set(SavedScreenPercentage.GetValue(), ECVF_SetByCode);
			SavedScreenPercentage.Reset();
		}
		if (IConsoleVariable* Sharpen =
				IConsoleManager::Get().FindConsoleVariable(TEXT("r.Tonemapper.Sharpen")))
		{
			if (SavedTonemapSharpen.IsSet())
			{
				Sharpen->Set(SavedTonemapSharpen.GetValue(), ECVF_SetByCode);
				SavedTonemapSharpen.Reset();
			}
		}
		UE_LOG(LogTemp, Display, TEXT("[MagicSR] Viewport Temporal OFF"));
	}
}

bool FMagicSRModule::IsViewportTemporalEnabled() const
{
	return FMagicSRViewportBridge::Get().IsEnabled();
}
