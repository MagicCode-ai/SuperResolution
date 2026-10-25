#include "MagicSRViewportBridge.h"
#include "MagicSRFgPresenter.h"

#include "CommonRenderResources.h"
#include "Framework/Application/SlateApplication.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "PixelShaderUtils.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RHIStaticStates.h"
#include "RenderCore.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RendererInterface.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "PooledRenderTarget.h"
#include "ShaderParameterStruct.h"
#include "Slate/SlateViewportProvider.h"
#include "Widgets/SWindow.h"

class FMagicSRFgPresentPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FMagicSRFgPresentPS);
	SHADER_USE_PARAMETER_STRUCT(FMagicSRFgPresentPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_TEXTURE(Texture2D, GeneratedTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, RealTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, ComposedTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER(float, UiThresh)
		SHADER_PARAMETER(uint32, UiMode)
		SHADER_PARAMETER(float, BandY)
		SHADER_PARAMETER(FVector2f, InvSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMagicSRFgPresentPS, "/Plugin/MagicSR/Private/MagicSRFgPresent.usf", "MainPS", SF_Pixel);

namespace
{
void CopyTextureSameSize(FRHICommandListImmediate& RHICmdList, FRHITexture* Src, FRHITexture* Dst);

TRefCountPtr<IPooledRenderTarget> GMagicFgGenerated;
TRefCountPtr<IPooledRenderTarget> GMagicFgRealScene;
TRefCountPtr<FRHITexture> GMagicFgScratch;
TRefCountPtr<FRHITexture> GMagicFgFlashWhite;
FCriticalSection GPresentStatsCs;
volatile int32 GMagicSRFgPerfCollect = 0;

TAutoConsoleVariable<int32> CVarMagicSRFrameGenPresent(
	TEXT("r.MagicSR.FrameGenPresent"),
	0,
	TEXT("0=ShowMid evaluation (default, -MagicSRFgTour). ")
	TEXT("1=DEPRECATED nested engine present (known flicker, unused). ")
	TEXT("2=plugin overlay presenter (D3D11 bitblt swapchain + present thread)."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarMagicSRFgPerfStats(
	TEXT("r.MagicSR.FgPerfStats"),
	0,
	TEXT("Measurement only. 1=extra present-interval kind split is stored. Default 0."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgLatencyProbe(
	TEXT("r.MagicSR.FgLatencyProbe"),
	0,
	TEXT("Measurement only. 1=Off-hook GPU-complete fence + poller (≤0.2 ms). Default 0: no fences, no poller."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarMagicSRFgLatencyFlash(
	TEXT("r.MagicSR.FgLatencyFlash"),
	0,
	TEXT("Measurement only. 1=allow white-flash arming for screen-visible latency. Default 0."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgLatencyFlashFrames(
	TEXT("r.MagicSR.FgLatencyFlashFrames"),
	2,
	TEXT("Measurement only. Engine frames of white flash when armed. Default 2."),
	ECVF_Default);

volatile int32 GFlashRemain = 0;

struct FOffGpuProbeSlot
{
	FGPUFenceRHIRef Fence;
	double TickSec = 0.0;
	double WriteSec = 0.0;
	int32 Pending = 0;
};

constexpr int32 kOffGpuProbeCap = 32;
FOffGpuProbeSlot GOffGpuProbe[kOffGpuProbeCap];
int32 GOffGpuProbeW = 0;
FCriticalSection GOffGpuProbeCs;
FRunnableThread* GOffGpuPollerThread = nullptr;
volatile int32 GOffGpuPollerRun = 0;

class FOffGpuPoller final : public FRunnable
{
public:
	virtual uint32 Run() override
	{
		while (FPlatformAtomics::AtomicRead(&GOffGpuPollerRun) != 0)
		{
			const double Now = FPlatformTime::Seconds();
			double TickSum = 0.0;
			double WriteSum = 0.0;
			int32 N = 0;
			{
				FScopeLock ProbeLock(&GOffGpuProbeCs);
				for (int32 i = 0; i < kOffGpuProbeCap; ++i)
				{
					FOffGpuProbeSlot& S = GOffGpuProbe[i];
					if (S.Pending == 0 || !S.Fence.IsValid() || !S.Fence->Poll())
					{
						continue;
					}
					TickSum += (Now - S.TickSec) * 1000.0;
					WriteSum += (Now - S.WriteSec) * 1000.0;
					S.Pending = 0;
					++N;
				}
			}
			if (N > 0)
			{
				FMagicSRViewportBridge::Get().NoteOffGpuComplete(TickSum, WriteSum, N);
			}
			FPlatformProcess::SleepNoStats(0.00015f);
		}
		return 0;
	}
	virtual void Stop() override
	{
		FPlatformAtomics::InterlockedExchange(&GOffGpuPollerRun, 0);
	}
};

FOffGpuPoller GOffGpuPoller;

void EnsureOffGpuPoller()
{
	if (GOffGpuPollerThread)
	{
		return;
	}
	FPlatformAtomics::InterlockedExchange(&GOffGpuPollerRun, 1);
	GOffGpuPollerThread = FRunnableThread::Create(
		&GOffGpuPoller, TEXT("MagicSROffGpuPoll"), 0, TPri_AboveNormal);
}

void StopOffGpuPoller()
{
	if (!GOffGpuPollerThread)
	{
		return;
	}
	FPlatformAtomics::InterlockedExchange(&GOffGpuPollerRun, 0);
	GOffGpuPollerThread->Kill(true);
	delete GOffGpuPollerThread;
	GOffGpuPollerThread = nullptr;
	FScopeLock ProbeLock(&GOffGpuProbeCs);
	for (int32 i = 0; i < kOffGpuProbeCap; ++i)
	{
		GOffGpuProbe[i].Pending = 0;
		GOffGpuProbe[i].Fence.SafeRelease();
	}
	GOffGpuProbeW = 0;
}

void WriteOffGpuFence(FRHICommandListImmediate& RHICmdList, double TickT)
{
	if (CVarMagicSRFgLatencyProbe.GetValueOnRenderThread() == 0)
	{
		return;
	}
	EnsureOffGpuPoller();
	FScopeLock ProbeLock(&GOffGpuProbeCs);
	const int32 i = GOffGpuProbeW % kOffGpuProbeCap;
	FOffGpuProbeSlot& S = GOffGpuProbe[i];
	if (S.Pending != 0)
	{
		return;
	}
	if (!S.Fence.IsValid())
	{
		S.Fence = RHICreateGPUFence(*FString::Printf(TEXT("MagicSR.OffGpu%d"), i));
	}
	if (!S.Fence.IsValid())
	{
		return;
	}
	S.Fence->Clear();
	RHICmdList.WriteGPUFence(S.Fence);
	S.TickSec = TickT;
	S.WriteSec = FPlatformTime::Seconds();
	S.Pending = 1;
	++GOffGpuProbeW;
}

FRHITexture* EnsureFlashWhite(FRHICommandListImmediate& RHICmdList, FRHITexture* BackBuffer)
{
	if (!BackBuffer)
	{
		return nullptr;
	}
	const FIntPoint Size = BackBuffer->GetSizeXY();
	const EPixelFormat Fmt = BackBuffer->GetFormat();
	if (GMagicFgFlashWhite.IsValid() && GMagicFgFlashWhite->GetSizeXY() == Size &&
		GMagicFgFlashWhite->GetFormat() == Fmt)
	{
		return GMagicFgFlashWhite.GetReference();
	}
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("MagicSR.LatencyFlash"), Size.X, Size.Y, Fmt)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable)
			.SetClearValue(FClearValueBinding(FLinearColor::White))
			.SetInitialState(ERHIAccess::SRVMask);
	GMagicFgFlashWhite = RHICmdList.CreateTexture(Desc);
	return GMagicFgFlashWhite.GetReference();
}

void ApplyLatencyFlash(FRHICommandListImmediate& RHICmdList, FRHITexture* BackBuffer)
{
	if (!BackBuffer || FPlatformAtomics::AtomicRead(&GFlashRemain) <= 0)
	{
		return;
	}
	FPlatformAtomics::InterlockedDecrement(&GFlashRemain);
	FRHITexture* FlashTex = EnsureFlashWhite(RHICmdList, BackBuffer);
	if (!FlashTex)
	{
		return;
	}
	RHICmdList.Transition(FRHITransitionInfo(FlashTex, ERHIAccess::Unknown, ERHIAccess::RTV));
	FRHIRenderPassInfo RPInfo(FlashTex, ERenderTargetActions::Clear_Store);
	RHICmdList.BeginRenderPass(RPInfo, TEXT("MagicSR.LatencyFlash"));
	RHICmdList.EndRenderPass();
	CopyTextureSameSize(RHICmdList, FlashTex, BackBuffer);
}

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterLeanCopy(
	TEXT("r.MagicSR.FgPresenterLeanCopy"),
	1,
	TEXT("1=composite gen into ring (1 CopyTexture of real). 0=legacy 3-copy path."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarMagicSRFrameGenPresentUiThresh(
	TEXT("r.MagicSR.FrameGenPresentUiThresh"),
	0.08f,
	TEXT("UI preserve threshold: distance(Composed, Real) after dilation. Scene pixels stay on gen."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarMagicSRFrameGenPresentBisect(
	TEXT("r.MagicSR.FgPresentBisect"),
	0,
	TEXT("Present-layer flicker bisect. 0=production (layout-safe extra present + UI mask). ")
	TEXT("1=single present of composite gen (no extra EDV). ")
	TEXT("2=single present of real (no composite). ")
	TEXT("3=extra present of unmodified real+UI. ")
	TEXT("4=old extra present, skip Present layout (repro blue). ")
	TEXT("5=old extra present + unique FrameCounter only. ")
	TEXT("6=extra present of solid magenta, layout-safe."),
	ECVF_RenderThreadSafe);

enum class EMagicFgPresentKind : int32
{
	None = 0,
	Generated = 1,
	Real = 2
};

class FMagicSRFgCustomPresent final : public FRHICustomPresent
{
public:
	FMagicSRViewportBridge* Owner = nullptr;

	virtual void OnBackBufferResize() override {}
	virtual bool NeedsNativePresent() override { return true; }
	virtual bool Present(FRHIViewport* Viewport, IRHICommandContext& RHICmdContext, int32& InOutSyncInterval) override
	{
		(void)RHICmdContext;
		InOutSyncInterval = 0;
		if (Owner)
		{
			Owner->OnCustomPresent(Viewport);
		}
		return true;
	}
};

TRefCountPtr<FMagicSRFgCustomPresent> GMagicFgCustomPresent;
bool GMagicFgCustomPresentInstalled = false;

void PreciseWaitUntil(double Target)
{
	const double Now0 = FPlatformTime::Seconds();
	if (Target <= Now0)
	{
		return;
	}
	// 16–40 ms real frames → half-interval wait ≤ 20 ms. Never stall a runaway frame.
	const double Cap = Now0 + 0.022;
	if (Target > Cap)
	{
		Target = Cap;
	}
	for (;;)
	{
		const double Now = FPlatformTime::Seconds();
		const double Remain = Target - Now;
		if (Remain <= 0.0004)
		{
			break;
		}
		FPlatformProcess::SleepNoStats(static_cast<float>(Remain - 0.0004));
	}
	while (FPlatformTime::Seconds() < Target)
	{
	}
}

void CopyTextureSameSize(FRHICommandListImmediate& RHICmdList, FRHITexture* Src, FRHITexture* Dst)
{
	if (!Src || !Dst)
	{
		return;
	}
	const FIntPoint Ss = Src->GetSizeXY();
	const FIntPoint Ds = Dst->GetSizeXY();
	if (Ss.X <= 0 || Ss.Y <= 0 || Ss != Ds)
	{
		return;
	}
	FRHICopyTextureInfo Info;
	Info.Size = FIntVector(Ss.X, Ss.Y, 1);
	RHICmdList.Transition(FRHITransitionInfo(Src, ERHIAccess::Unknown, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(Dst, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(Src, Dst, Info);
	RHICmdList.Transition(FRHITransitionInfo(Src, ERHIAccess::CopySrc, ERHIAccess::SRVGraphics));
	RHICmdList.Transition(FRHITransitionInfo(Dst, ERHIAccess::CopyDest, ERHIAccess::SRVGraphics));
}

void CopyToBackbufferPresent(FRHICommandListImmediate& RHICmdList, FRHITexture* Src, FRHITexture* BackBuffer)
{
	if (!Src || !BackBuffer)
	{
		return;
	}
	const FIntPoint Ss = Src->GetSizeXY();
	const FIntPoint Ds = BackBuffer->GetSizeXY();
	if (Ss.X <= 0 || Ss.Y <= 0 || Ss != Ds)
	{
		return;
	}
	FRHICopyTextureInfo Info;
	Info.Size = FIntVector(Ss.X, Ss.Y, 1);
	RHICmdList.Transition(FRHITransitionInfo(Src, ERHIAccess::Unknown, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(BackBuffer, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(Src, BackBuffer, Info);
	RHICmdList.Transition(FRHITransitionInfo(Src, ERHIAccess::CopySrc, ERHIAccess::SRVGraphics));
	// DelayAcquire CopyImageToBackBuffer requires PRESENT_SRC on the source.
	RHICmdList.Transition(FRHITransitionInfo(BackBuffer, ERHIAccess::CopyDest, ERHIAccess::Present));
}

FRHITexture* EnsureScratchPresent(FRHICommandListImmediate& RHICmdList, FRHITexture* BackBuffer)
{
	if (!BackBuffer)
	{
		return nullptr;
	}
	const FIntPoint Size = BackBuffer->GetSizeXY();
	const EPixelFormat Fmt = BackBuffer->GetFormat();
	if (GMagicFgScratch.IsValid() && GMagicFgScratch->GetSizeXY() == Size &&
		GMagicFgScratch->GetFormat() == Fmt)
	{
		return GMagicFgScratch.GetReference();
	}
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("MagicFG.PresentScratch"), Size.X, Size.Y, Fmt)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable)
			.SetInitialState(ERHIAccess::SRVMask);
	GMagicFgScratch = RHICmdList.CreateTexture(Desc);
	return GMagicFgScratch.GetReference();
}

FRHITexture* EnsureSavedPresent(FRHICommandListImmediate& RHICmdList, FRHITexture* BackBuffer)
{
	if (!BackBuffer)
	{
		return nullptr;
	}
	FMagicSRViewportBridge& Bridge = FMagicSRViewportBridge::Get();
	const FIntPoint Size = BackBuffer->GetSizeXY();
	const EPixelFormat Fmt = BackBuffer->GetFormat();
	FRHITexture* Saved = Bridge.GetSavedPresentRHI();
	if (Saved && Saved->GetSizeXY() == Size && Saved->GetFormat() == Fmt)
	{
		return Saved;
	}
	const FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("MagicFG.SavedPresent"), Size.X, Size.Y, Fmt)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable)
			.SetInitialState(ERHIAccess::SRVMask);
	return Bridge.ReplaceSavedPresent(RHICmdList.CreateTexture(Desc));
}

void CompositeGeneratedToScratch(
	FRHICommandListImmediate& RHICmdList,
	FRHITexture* Gen,
	FRHITexture* Real,
	FRHITexture* Saved,
	FRHITexture* Scratch,
	FRHITexture* BackBuffer,
	uint32 UiMode)
{
	RHICmdList.Transition(FRHITransitionInfo(Gen, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	if (Real)
	{
		RHICmdList.Transition(FRHITransitionInfo(Real, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	}
	RHICmdList.Transition(FRHITransitionInfo(Saved, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	RHICmdList.Transition(FRHITransitionInfo(Scratch, ERHIAccess::Unknown, ERHIAccess::RTV));

	FRHIRenderPassInfo RPInfo(Scratch, ERenderTargetActions::Clear_Store);
	RHICmdList.BeginRenderPass(RPInfo, TEXT("MagicFG.PresentComposite"));
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FMagicSRFgPresentPS> PixelShader(ShaderMap);
	FMagicSRFgPresentPS::FParameters Params;
	FMemory::Memzero(Params);
	Params.GeneratedTexture = Gen;
	Params.RealTexture = Real ? Real : Gen;
	Params.ComposedTexture = Saved;
	Params.PointSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Params.UiThresh = CVarMagicSRFrameGenPresentUiThresh.GetValueOnRenderThread();
	Params.UiMode = UiMode;
	const FIntPoint Size = BackBuffer->GetSizeXY();
	Params.BandY = Size.Y > 0 ? (48.f / float(Size.Y)) : 0.04f;
	Params.InvSize = FVector2f(
		Size.X > 0 ? (1.f / float(Size.X)) : 0.f,
		Size.Y > 0 ? (1.f / float(Size.Y)) : 0.f);
	FPixelShaderUtils::DrawFullscreenPixelShader(
		RHICmdList, ShaderMap, PixelShader, Params, FIntRect(FIntPoint::ZeroValue, Size));
	RHICmdList.EndRenderPass();
}

int32 PresenterDebugRT()
{
	static const auto* CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MagicSR.FgPresenterDebug"));
	return CVar ? CVar->GetValueOnRenderThread() : 0;
}

int32 PresenterAblationRT()
{
	static const auto* CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MagicSR.FgPresenterAblation"));
	return CVar ? CVar->GetValueOnRenderThread() : 0;
}

double GMagicFgLastEnginePush = 0.0;
} // namespace

int32 FMagicSRViewportBridge::GetFrameGenPresentMode()
{
	return IsInRenderingThread() ? CVarMagicSRFrameGenPresent.GetValueOnRenderThread()
								 : CVarMagicSRFrameGenPresent.GetValueOnGameThread();
}

bool FMagicSRViewportBridge::GetFrameGenPresent()
{
	return GetFrameGenPresentMode() != 0;
}

void FMagicSRViewportBridge::NotifyPresenterPresent(int32 Kind)
{
	NotePresentTime(Kind);
}

void FMagicSRViewportBridge::SetPerfCollect(bool bOn)
{
	FPlatformAtomics::InterlockedExchange(&GMagicSRFgPerfCollect, bOn ? 1 : 0);
	if (!bOn)
	{
		StopOffGpuPoller();
	}
}

void FMagicSRViewportBridge::TriggerLatencyFlash()
{
	if (CVarMagicSRFgLatencyFlash.GetValueOnAnyThread() == 0)
	{
		return;
	}
	const int32 Frames = FMath::Clamp(CVarMagicSRFgLatencyFlashFrames.GetValueOnAnyThread(), 1, 64);
	FPlatformAtomics::InterlockedExchange(&GFlashRemain, Frames);
}

void FMagicSRViewportBridge::NoteOffGpuComplete(double TickToGpuMsSum, double WriteToGpuMsSum, int32 Count)
{
	if (Count <= 0)
	{
		return;
	}
	FScopeLock Lock(&GPresentStatsCs);
	OffTickToGpuMsSum += TickToGpuMsSum;
	OffWriteToGpuMsSum += WriteToGpuMsSum;
	OffTickToGpuSamples += Count;
}

bool FMagicSRViewportBridge::IsPerfCollect()
{
	return FPlatformAtomics::AtomicRead(&GMagicSRFgPerfCollect) != 0;
}

void FMagicSRViewportBridge::NoteGameTickTime()
{
	FMagicSRFgPresenter::NoteGameTickTime();
}

void FMagicSRViewportBridge::BeginPerfWindow()
{
	ResetPresentCounters();
	FMagicSRFgPresenter::Get().BeginPerfWindow();
}

void FMagicSRViewportBridge::DumpPresenterTraceCsv(const TCHAR* Path) const
{
	FMagicSRFgPresenter::Get().DumpTraceCsv(Path);
}

int32 FMagicSRViewportBridge::GetPresenterTraceCount() const
{
	return FMagicSRFgPresenter::Get().GetTraceCount();
}

FRHITexture* FMagicSRViewportBridge::GetSavedPresentRHI() const
{
	return FgSavedPresent.GetReference();
}

FRHITexture* FMagicSRViewportBridge::ReplaceSavedPresent(FRHITexture* NewTex)
{
	FgSavedPresent = NewTex;
	return FgSavedPresent.GetReference();
}

void FMagicSRViewportBridge::PushPresentKind(int32 Kind)
{
	const int32 Write = FgPresentKindWrite;
	FgPresentKinds[Write & 7] = Kind;
	FPlatformAtomics::InterlockedIncrement(&FgPresentKindWrite);
}

int32 FMagicSRViewportBridge::PopPresentKind()
{
	const int32 Read = FgPresentKindRead;
	if (Read == FgPresentKindWrite)
	{
		return 0;
	}
	const int32 Kind = FgPresentKinds[Read & 7];
	FPlatformAtomics::InterlockedIncrement(&FgPresentKindRead);
	return Kind;
}

void FMagicSRViewportBridge::NotePresentTime(int32 Kind)
{
	FScopeLock Lock(&GPresentStatsCs);
	const double Now = FPlatformTime::Seconds();
	if (LastPresentTime > 0.0)
	{
		const double Ms = (Now - LastPresentTime) * 1000.0;
		PresentIntervalsMs[PresentIntervalWrite] = Ms;
		if (FPlatformAtomics::AtomicRead(&GMagicSRFgPerfCollect) != 0)
		{
			PresentIntervalKinds[PresentIntervalWrite] = Kind;
		}
		PresentIntervalWrite = (PresentIntervalWrite + 1) % PresentIntervalCap;
		if (PresentIntervalCount < PresentIntervalCap)
		{
			++PresentIntervalCount;
		}
	}
	if (Kind == static_cast<int32>(EMagicFgPresentKind::Generated))
	{
		LastGenPresentTime = Now;
		if (LastRealPresentTime > 0.0 && RealIntervalEma > 1.0e-4)
		{
			const double Phase = (Now - LastRealPresentTime) / RealIntervalEma;
			if (Phase > 0.0 && Phase < 2.0)
			{
				GenPhaseSum += Phase;
				++GenPhaseCount;
			}
		}
	}
	else if (Kind == static_cast<int32>(EMagicFgPresentKind::Real) || Kind == 0)
	{
		if (LastRealPresentTime > 0.0)
		{
			const double RealDt = Now - LastRealPresentTime;
			if (RealDt > 0.008 && RealDt < 0.080)
			{
				if (RealIntervalEma < 0.004)
				{
					RealIntervalEma = RealDt;
				}
				else
				{
					RealIntervalEma = RealIntervalEma * 0.7 + RealDt * 0.3;
				}
			}
		}
		LastRealPresentTime = Now;
	}
	LastPresentTime = Now;
	++PresentCount;
	++WindowPresentCount;
	++HudPresentWindowCount;

	if (HudPresentWindowStart <= 0.0)
	{
		HudPresentWindowStart = Now;
	}
	const double HudDt = Now - HudPresentWindowStart;
	if (HudDt >= 0.5)
	{
		HudPresentFps = static_cast<float>(HudPresentWindowCount / HudDt);
		HudRenderFps = static_cast<float>(HudEngineWindowCount / HudDt);
		HudRealIntervalEmaMs = static_cast<float>(RealIntervalEma * 1000.0);
		double Sum = 0.0;
		double SumSq = 0.0;
		const int32 N = PresentIntervalCount;
		for (int32 i = 0; i < N; ++i)
		{
			Sum += PresentIntervalsMs[i];
			SumSq += PresentIntervalsMs[i] * PresentIntervalsMs[i];
		}
		if (N > 1)
		{
			const double Mean = Sum / double(N);
			const double Var = FMath::Max(0.0, SumSq / double(N) - Mean * Mean);
			HudJitterStdMs = static_cast<float>(FMath::Sqrt(Var));
		}
		HudPresentWindowCount = 0;
		HudEngineWindowCount = 0;
		HudPresentWindowStart = Now;
	}
}

void FMagicSRViewportBridge::OnCustomPresent(FRHIViewport* Viewport)
{
	if (Viewport)
	{
		CachedPresentViewport = Viewport;
	}
	NotePresentTime(PopPresentKind());
}

void FMagicSRViewportBridge::EnsurePresentHook()
{
	if (!bPresentHookBound && FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			PresentHookHandle = Renderer->OnBackBufferReadyToPresent().AddRaw(
				this, &FMagicSRViewportBridge::HandleBackBufferReady);
			bPresentHookBound = true;
			if (!GMagicFgCustomPresent.IsValid())
			{
				GMagicFgCustomPresent = new FMagicSRFgCustomPresent();
				GMagicFgCustomPresent->Owner = this;
			}
		}
	}
	FMagicSRFgPresenter::Get().EnsureTicker();
}

void FMagicSRViewportBridge::ShutdownPresentHook()
{
	FMagicSRFgPresenter::Get().RemoveTicker();
	if (bPresentHookBound && FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			Renderer->OnBackBufferReadyToPresent().Remove(PresentHookHandle);
		}
	}
	PresentHookHandle.Reset();
	bPresentHookBound = false;
	GMagicFgCustomPresentInstalled = false;
	if (GMagicFgCustomPresent.IsValid())
	{
		GMagicFgCustomPresent->Owner = nullptr;
		GMagicFgCustomPresent.SafeRelease();
	}
	CachedPresentViewport.SafeRelease();
	FgSavedPresent.SafeRelease();
	GMagicFgGenerated.SafeRelease();
	GMagicFgRealScene.SafeRelease();
	GMagicFgScratch.SafeRelease();
}

void FMagicSRViewportBridge::ReleasePresentResources_RenderThread()
{
	FgSavedPresent.SafeRelease();
	GMagicFgGenerated.SafeRelease();
	GMagicFgRealScene.SafeRelease();
	GMagicFgScratch.SafeRelease();
	FMagicSRFgPresenter::Get().ReleaseRing_RenderThread();
	FPlatformAtomics::InterlockedExchange(&FgGeneratedReady, 0);
}

void FMagicSRViewportBridge::NoteGeneratedFrame_RenderThread(
	FRDGBuilder& GraphBuilder, FRDGTextureRef FgOut, FRDGTextureRef RealScene)
{
	if (FgOut)
	{
		GraphBuilder.QueueTextureExtraction(FgOut, &GMagicFgGenerated);
	}
	if (RealScene)
	{
		GraphBuilder.QueueTextureExtraction(RealScene, &GMagicFgRealScene);
	}
	FPlatformAtomics::InterlockedExchange(&FgGeneratedReady, FgOut ? 1 : 0);
}

void FMagicSRViewportBridge::ResetPresentCounters()
{
	FScopeLock Lock(&GPresentStatsCs);
	PresentEngineFrames = 0;
	PresentCount = 0;
	WindowEngineFrames = 0;
	WindowPresentCount = 0;
	PresentWindowStart = FPlatformTime::Seconds();
	LastPresentTime = 0.0;
	LastRealPresentTime = 0.0;
	LastGenPresentTime = 0.0;
	RealIntervalEma = 0.0;
	LastFgGpuMs = 0.f;
	FgGpuMsSum = 0.0;
	FgGpuMsCount = 0;
	GenPhaseSum = 0.0;
	GenPhaseCount = 0;
	PresentIntervalCount = 0;
	PresentIntervalWrite = 0;
	HudPresentWindowCount = 0;
	HudEngineWindowCount = 0;
	HudPresentWindowStart = 0.0;
	OffTickToPresentMsSum = 0.0;
	OffTickToPresentSamples = 0;
	OffTickToGpuMsSum = 0.0;
	OffWriteToGpuMsSum = 0.0;
	OffTickToGpuSamples = 0;
	FMemory::Memzero(PresentIntervalsMs, sizeof(PresentIntervalsMs));
	FMemory::Memzero(PresentIntervalKinds, sizeof(PresentIntervalKinds));
}

FMagicSRViewportBridge::FPresentSnapshot FMagicSRViewportBridge::GetPresentSnapshot() const
{
	FScopeLock Lock(&GPresentStatsCs);
	FPresentSnapshot Out;
	Out.EngineFrames = WindowEngineFrames;
	Out.PresentCount = WindowPresentCount;
	Out.WindowSeconds = PresentWindowStart > 0.0 ? (FPlatformTime::Seconds() - PresentWindowStart) : 0.0;
	if (Out.WindowSeconds > 1.0e-4)
	{
		Out.RenderFps = static_cast<float>(Out.EngineFrames / Out.WindowSeconds);
		Out.PresentFps = static_cast<float>(Out.PresentCount / Out.WindowSeconds);
	}
	Out.FgGpuMs = FgGpuMsCount > 0 ? static_cast<float>(FgGpuMsSum / double(FgGpuMsCount)) : LastFgGpuMs;
	Out.RealIntervalEmaMs = static_cast<float>(RealIntervalEma * 1000.0);
	const bool bFgOn = GetFrameGenPresent() && !GetFrameGenShowMid() && GetFrameGenMode() > 0;
	Out.ExtraLatencyMs = bFgOn ? Out.RealIntervalEmaMs * 0.5f : 0.f;
	Out.GenPhaseMean = GenPhaseCount > 0 ? static_cast<float>(GenPhaseSum / double(GenPhaseCount)) : 0.f;

	const int32 N = PresentIntervalCount;
	if (N > 0)
	{
		double Sum = 0.0;
		double SumSq = 0.0;
		double MaxMs = 0.0;
		TArray<double> Sorted;
		Sorted.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			const double V = PresentIntervalsMs[i];
			Sum += V;
			SumSq += V * V;
			MaxMs = FMath::Max(MaxMs, V);
			Sorted.Add(V);
		}
		Out.IntervalMeanMs = static_cast<float>(Sum / double(N));
		if (N > 1)
		{
			const double Var = FMath::Max(0.0, SumSq / double(N) - (Sum / double(N)) * (Sum / double(N)));
			Out.IntervalStdMs = static_cast<float>(FMath::Sqrt(Var));
		}
		Out.IntervalMaxMs = static_cast<float>(MaxMs);
		Sorted.Sort();
		const int32 P95 = FMath::Clamp(int32(double(N - 1) * 0.95), 0, N - 1);
		Out.IntervalP95Ms = static_cast<float>(Sorted[P95]);
	}

	if (FPlatformAtomics::AtomicRead(&GMagicSRFgPerfCollect) != 0 && N >= 2)
	{
		double GrSum = 0.0, GrSq = 0.0, RgSum = 0.0, RgSq = 0.0;
		int32 GrN = 0, RgN = 0;
		const int32 Start = (PresentIntervalCount < PresentIntervalCap)
								? 0
								: PresentIntervalWrite;
		int32 PrevKind = 0;
		for (int32 i = 0; i < N; ++i)
		{
			const int32 Idx = (Start + i) % PresentIntervalCap;
			const int32 CurKind = PresentIntervalKinds[Idx];
			const double V = PresentIntervalsMs[Idx];
			if (i > 0)
			{
				if (PrevKind == static_cast<int32>(EMagicFgPresentKind::Generated) &&
					CurKind == static_cast<int32>(EMagicFgPresentKind::Real))
				{
					GrSum += V;
					GrSq += V * V;
					++GrN;
				}
				else if (PrevKind == static_cast<int32>(EMagicFgPresentKind::Real) &&
						 CurKind == static_cast<int32>(EMagicFgPresentKind::Generated))
				{
					RgSum += V;
					RgSq += V * V;
					++RgN;
				}
			}
			PrevKind = CurKind;
		}
		if (GrN > 0)
		{
			Out.GenToRealMeanMs = static_cast<float>(GrSum / double(GrN));
			if (GrN > 1)
			{
				const double Var = FMath::Max(0.0, GrSq / double(GrN) - (GrSum / double(GrN)) * (GrSum / double(GrN)));
				Out.GenToRealStdMs = static_cast<float>(FMath::Sqrt(Var));
			}
		}
		if (RgN > 0)
		{
			Out.RealToGenMeanMs = static_cast<float>(RgSum / double(RgN));
			if (RgN > 1)
			{
				const double Var = FMath::Max(0.0, RgSq / double(RgN) - (RgSum / double(RgN)) * (RgSum / double(RgN)));
				Out.RealToGenStdMs = static_cast<float>(FMath::Sqrt(Var));
			}
		}
	}

	const FMagicSRFgPresenter::FPresenterPerfSnap Pres = FMagicSRFgPresenter::Get().GetPerfSnap();
	Out.DxgiLastPresentCount = Pres.DxgiLastPresentCount;
	Out.DxgiPresentDelta = Pres.DxgiPresentDelta;
	Out.DxgiStatsHr = Pres.DxgiStatsHr;
	Out.DxgiStatsPresentCount = Pres.DxgiStatsPresentCount;
	Out.PresenterCopyMs = Pres.CopySubmitMsMean;
	Out.PresenterHoldMs = Pres.RealHoldMsMean;
	Out.PresenterThreadCpuPct = Pres.ThreadCpuPct;
	Out.PresenterCopySamples = Pres.CopySamples;
	Out.BlitWaitMs = Pres.BlitWaitMsMean;
	Out.D3dCopyMs = Pres.D3dCopyMsMean;
	Out.DxgiPresentMs = Pres.DxgiPresentMsMean;
	Out.JobsDropped = Pres.JobsDropped;
	Out.QueueDepthMean = Pres.QueueDepthMean;
	Out.QueueDepthP95 = Pres.QueueDepthP95;
	Out.QueueDepthMax = Pres.QueueDepthMax;
	Out.GpuInflightMean = Pres.GpuInflightMean;
	Out.GpuInflightP95 = Pres.GpuInflightP95;
	Out.GpuInflightMax = Pres.GpuInflightMax;
	Out.PresenterBacklogMean = Pres.PresenterBacklogMean;
	Out.PresenterBacklogP95 = Pres.PresenterBacklogP95;
	Out.PresenterBacklogMax = Pres.PresenterBacklogMax;
	Out.TickToReadyMs = Pres.TickToReadyMsMean;
	Out.TickToReadySamples = Pres.TickToReadySamples;
	Out.ReadyToPresentMs = Pres.ReadyToPresentMsMean;
	Out.ReadyToPresentSamples = Pres.ReadyToPresentSamples;
	if (Pres.TickToPresentSamples > 0)
	{
		Out.TickToPresentMs = Pres.TickToPresentMsMean;
		Out.TickToPresentSamples = Pres.TickToPresentSamples;
	}
	else if (OffTickToPresentSamples > 0)
	{
		Out.TickToPresentMs = static_cast<float>(OffTickToPresentMsSum / double(OffTickToPresentSamples));
		Out.TickToPresentSamples = OffTickToPresentSamples;
	}
	if (OffTickToGpuSamples > 0)
	{
		Out.TickToGpuCompleteMs = static_cast<float>(OffTickToGpuMsSum / double(OffTickToGpuSamples));
		Out.TickToGpuCompleteSamples = OffTickToGpuSamples;
		Out.WriteToGpuCompleteMs = static_cast<float>(OffWriteToGpuMsSum / double(OffTickToGpuSamples));
	}
	return Out;
}

void FMagicSRViewportBridge::HandleBackBufferReady(SWindow& Window, ISlateViewportProvider& ViewportProvider)
{
	(void)Window;
	if (!IsInRenderingThread())
	{
		return;
	}

	FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
	FRHITexture* BackBufferEarly = ViewportProvider.GetBackBufferResource();
	ApplyLatencyFlash(RHICmdList, BackBufferEarly);
	const int32 PresentMode = GetFrameGenPresentMode();
	const bool bPresentMode = PresentMode != 0 && !GetFrameGenShowMid() && GetFrameGenMode() > 0;
	double LocalRealEma = 0.0;
	{
		FScopeLock Lock(&GPresentStatsCs);
		++PresentEngineFrames;
		++WindowEngineFrames;
		++HudEngineWindowCount;
		LocalRealEma = RealIntervalEma;
		if (bPresentMode || (FPlatformAtomics::AtomicRead(&GMagicSRFgPerfCollect) != 0 && GetFrameGenMode() > 0))
		{
			FgGpuMsSum += LastFgGpuMs;
			++FgGpuMsCount;
		}
		else
		{
			LastFgGpuMs = 0.f;
		}
	}
	if (!bPresentMode)
	{
		if (GMagicFgCustomPresentInstalled)
		{
			ViewportProvider.SetCustomPresent(nullptr);
			GMagicFgCustomPresentInstalled = false;
		}
		if (FPlatformAtomics::AtomicRead(&GMagicSRFgPerfCollect) != 0)
		{
			const double TickT = FMagicSRFgPresenter::GetGameTickTimeForRenderFrame();
			if (TickT > 0.0)
			{
				WriteOffGpuFence(RHICmdList, TickT);
				FScopeLock TickLock(&GPresentStatsCs);
				OffTickToPresentMsSum += (FPlatformTime::Seconds() - TickT) * 1000.0;
				++OffTickToPresentSamples;
			}
		}
		NotePresentTime(0);
		FPlatformAtomics::InterlockedExchange(&FgGeneratedReady, 0);
		return;
	}

	if (PresentMode == 2)
	{
		if (GMagicFgCustomPresentInstalled)
		{
			ViewportProvider.SetCustomPresent(nullptr);
			GMagicFgCustomPresentInstalled = false;
		}
		FRHITexture* BackBuffer = ViewportProvider.GetBackBufferResource();
		if (!BackBuffer)
		{
			return;
		}
		const int32 Ready = FPlatformAtomics::InterlockedExchange(&FgGeneratedReady, 0);
		FRHITexture* Gen = (Ready && GMagicFgGenerated.IsValid()) ? GMagicFgGenerated->GetRHI() : nullptr;
		FRHITexture* Real = GMagicFgRealScene.IsValid() ? GMagicFgRealScene->GetRHI() : nullptr;
		const double Now = FPlatformTime::Seconds();
		const double EngineDt = (GMagicFgLastEnginePush > 0.0) ? (Now - GMagicFgLastEnginePush) : 1.0;
		GMagicFgLastEnginePush = Now;
		const int32 Debug = PresenterDebugRT();
		const int32 Ablation = PresenterAblationRT();
		const bool bDisallowGen = !Gen || EngineDt > 0.100;
		if (Debug == 1)
		{
			return;
		}
		const bool bWantGen = Debug != 2 && Ablation != 4 && Gen && !bDisallowGen;
		FMagicSRFgPresenter& Presenter = FMagicSRFgPresenter::Get();
		if (CVarMagicSRFgPresenterLeanCopy.GetValueOnRenderThread() != 0)
		{
			const int32 RealSlot = Presenter.AcquireSlot_RenderThread(RHICmdList, BackBuffer);
			FRHITexture* RealSlotTex = Presenter.GetSlotRHI(RealSlot);
			if (RealSlot >= 0 && RealSlotTex)
			{
				CopyTextureSameSize(RHICmdList, BackBuffer, RealSlotTex);
				if (bWantGen)
				{
					const int32 GenSlot = Presenter.AcquireSlot_RenderThread(RHICmdList, BackBuffer);
					FRHITexture* GenSlotTex = Presenter.GetSlotRHI(GenSlot);
					if (GenSlot >= 0 && GenSlotTex)
					{
						CompositeGeneratedToScratch(
							RHICmdList, Gen, Real, RealSlotTex, GenSlotTex, BackBuffer, Real ? 1u : 0u);
						RHICmdList.Transition(
							FRHITransitionInfo(GenSlotTex, ERHIAccess::RTV, ERHIAccess::CopySrc));
						Presenter.FinishSlot_RenderThread(
							RHICmdList, GenSlot, static_cast<int32>(EMagicFgPresentKind::Generated), false);
					}
				}
				RHICmdList.Transition(
					FRHITransitionInfo(RealSlotTex, ERHIAccess::Unknown, ERHIAccess::CopySrc));
				Presenter.FinishSlot_RenderThread(
					RHICmdList, RealSlot, static_cast<int32>(EMagicFgPresentKind::Real), bDisallowGen);
			}
			return;
		}
		if (bWantGen)
		{
			FRHITexture* Saved = EnsureSavedPresent(RHICmdList, BackBuffer);
			FRHITexture* Scratch = EnsureScratchPresent(RHICmdList, BackBuffer);
			if (Saved && Scratch)
			{
				CopyTextureSameSize(RHICmdList, BackBuffer, Saved);
				CompositeGeneratedToScratch(RHICmdList, Gen, Real, Saved, Scratch, BackBuffer, Real ? 1u : 0u);
				Presenter.PushFrame_RenderThread(
					RHICmdList, Scratch, static_cast<int32>(EMagicFgPresentKind::Generated), false);
			}
		}
		Presenter.PushFrame_RenderThread(
			RHICmdList, BackBuffer, static_cast<int32>(EMagicFgPresentKind::Real), bDisallowGen);
		return;
	}

	if (GMagicFgCustomPresent.IsValid() && !GMagicFgCustomPresentInstalled)
	{
		ViewportProvider.SetCustomPresent(GMagicFgCustomPresent.GetReference());
		GMagicFgCustomPresentInstalled = true;
	}

	const int32 Ready = FPlatformAtomics::InterlockedExchange(&FgGeneratedReady, 0);
	FRHITexture* BackBuffer = ViewportProvider.GetBackBufferResource();
	FRHITexture* Gen = (Ready && GMagicFgGenerated.IsValid()) ? GMagicFgGenerated->GetRHI() : nullptr;
	FRHITexture* Real = GMagicFgRealScene.IsValid() ? GMagicFgRealScene->GetRHI() : nullptr;
	FRHIViewport* ViewportRHI = CachedPresentViewport.GetReference();

	if (!BackBuffer || !Gen || !ViewportRHI)
	{
		return;
	}

	FRHITexture* Saved = EnsureSavedPresent(RHICmdList, BackBuffer);
	FRHITexture* Scratch = EnsureScratchPresent(RHICmdList, BackBuffer);
	if (!Saved || !Scratch)
	{
		return;
	}

	const int32 Bisect = CVarMagicSRFrameGenPresentBisect.GetValueOnRenderThread();
	const bool bComposite =
		Bisect != 2 && Bisect != 3;
	const bool bExtraPresent = Bisect != 1 && Bisect != 2;
	const bool bLayoutToPresent = Bisect != 4 && Bisect != 5;
	const bool bUniqueFrameCounter = Bisect != 4;
	const bool bRestore = bExtraPresent && Bisect != 1 && Bisect != 2 && Bisect != 3;
	const bool bPace = bExtraPresent;
	const bool bSaveCopy = Bisect != 2 && Bisect != 3;

	if (bSaveCopy)
	{
		CopyTextureSameSize(RHICmdList, BackBuffer, Saved);
	}

	if (bComposite)
	{
		RHICmdList.Transition(FRHITransitionInfo(Gen, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
		if (Real)
		{
			RHICmdList.Transition(FRHITransitionInfo(Real, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
		}
		RHICmdList.Transition(FRHITransitionInfo(Saved, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
		RHICmdList.Transition(FRHITransitionInfo(Scratch, ERHIAccess::Unknown, ERHIAccess::RTV));

		// Real is post-tonemap SceneColor (often FloatRGBA / R11G11B10); the
		// backbuffer is typically BGRA8. Format equality is not required — both
		// are sampled in the shader. The old 48px band (UiMode 2) dropped
		// mid-screen Slate ("Next Camera") on generated presents.
		uint32 UiMode = 1u;
		if (Bisect == 6)
		{
			UiMode = 4u;
		}
		else if (!Real)
		{
			UiMode = 0u;
		}

		// Never BeginRenderPass on DelayAcquire RenderingBackBuffer. Slate
		// already drew into it; Vulkan CopyImageToBackBuffer then assumes
		// PRESENT_SRC. A plugin RT pass desyncs that layout and presents a
		// cleared swapchain image (solid navy/black, HUD gone).
		FRHIRenderPassInfo RPInfo(Scratch, ERenderTargetActions::Clear_Store);
		RHICmdList.BeginRenderPass(RPInfo, TEXT("MagicFG.PresentComposite"));
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FMagicSRFgPresentPS> PixelShader(ShaderMap);
		FMagicSRFgPresentPS::FParameters Params;
		FMemory::Memzero(Params);
		Params.GeneratedTexture = Gen;
		Params.RealTexture = Real ? Real : Gen;
		Params.ComposedTexture = Saved;
		Params.PointSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Params.UiThresh = CVarMagicSRFrameGenPresentUiThresh.GetValueOnRenderThread();
		Params.UiMode = UiMode;
		const FIntPoint Size = BackBuffer->GetSizeXY();
		Params.BandY = Size.Y > 0 ? (48.f / float(Size.Y)) : 0.04f;
		Params.InvSize = FVector2f(
			Size.X > 0 ? (1.f / float(Size.X)) : 0.f,
			Size.Y > 0 ? (1.f / float(Size.Y)) : 0.f);
		FPixelShaderUtils::DrawFullscreenPixelShader(
			RHICmdList, ShaderMap, PixelShader, Params, FIntRect(FIntPoint::ZeroValue, Size));
		RHICmdList.EndRenderPass();
		if (bLayoutToPresent)
		{
			CopyToBackbufferPresent(RHICmdList, Scratch, BackBuffer);
		}
		else
		{
			CopyTextureSameSize(RHICmdList, Scratch, BackBuffer);
		}
	}

	if (bExtraPresent)
	{
		if (bLayoutToPresent && !bComposite)
		{
			RHICmdList.Transition(
				FRHITransitionInfo(BackBuffer, ERHIAccess::Unknown, ERHIAccess::Present));
		}
		PushPresentKind(static_cast<int32>(EMagicFgPresentKind::Generated));
		const uint64 GenFrame = bUniqueFrameCounter
			? ((GFrameCounterRenderThread << 1) | 1ull)
			: GFrameCounterRenderThread;
		RHICmdList.EndDrawingViewport(
			ViewportRHI, FRHIPresentArgs(GenFrame, true, false));
	}

	if (bPace && RealIntervalEma > 0.008 && RealIntervalEma < 0.080)
	{
		PreciseWaitUntil(FPlatformTime::Seconds() + RealIntervalEma * 0.5);
	}

	if (bRestore)
	{
		CopyToBackbufferPresent(RHICmdList, Saved, BackBuffer);
		PushPresentKind(static_cast<int32>(EMagicFgPresentKind::Real));
	}
}
