#include "MagicSRSceneViewExtension.h"

#include "MagicSRTemporalUpscaler.h"
#include "MagicSRViewportBridge.h"

#include "SceneRendering.h"
#include "SceneView.h"
#include "HAL/IConsoleManager.h"
#include "PostProcess/PostProcessInputs.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "SceneTexturesConfig.h"

static TAutoConsoleVariable<int32> CVarMagicSRResetViewState(
	TEXT("r.MagicSR.ResetViewState"),
	0,
	TEXT("One-shot. Next SetupView resets the primary view state so the temporal sample index and history match a repeat capture."),
	ECVF_Default);

FMagicSRSceneViewExtension::FMagicSRSceneViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FMagicSRSceneViewExtension::IsActiveThisFrame_Internal(
	const FSceneViewExtensionContext& Context) const
{
	(void)Context;
	// Stay active when MagicSR is off so the resolution log can see TSR / native views.
	// SetupView and BeginRenderViewFamily still no-op unless the viewport bridge is on.
	return true;
}

namespace
{
bool MagicSRIsPrimaryGameView(const FSceneView& View)
{
	// SceneCapture / reflections must not share the singleton Temporal handle.
	if (View.bIsSceneCapture || View.bIsReflectionCapture || View.bIsPlanarReflection ||
		View.bIsSceneCaptureCube)
	{
		return false;
	}
	return true;
}

void MagicSRInstallUpscaler(FSceneViewFamily* Family)
{
	if (Family == nullptr || !FMagicSRViewportBridge::Get().IsEnabled())
	{
		return;
	}
	if (Family->GetTemporalUpscalerInterface() == nullptr)
	{
		Family->SetTemporalUpscalerInterface(new FMagicSRTemporalUpscaler());
		static int32 InstallLogs = 0;
		if (InstallLogs < 4)
		{
			++InstallLogs;
			UE_LOG(LogTemp, Display, TEXT("[MagicSR] UPSCALER_INSTALL"));
		}
		return;
	}
	static int32 MissLogs = 0;
	if (MissLogs < 3)
	{
		const TCHAR* Name = Family->GetTemporalUpscalerInterface()->GetDebugName();
		if (Name && FCString::Strcmp(Name, TEXT("MagicSR")) == 0)
		{
			return;
		}
		++MissLogs;
		UE_LOG(LogTemp, Display, TEXT("[MagicSR] UPSCALER_MISS name=%s"), Name ? Name : TEXT("none"));
	}
}

bool MagicSRFamilyHasPrimaryGameView(const FSceneViewFamily& Family)
{
	for (int32 i = 0; i < Family.Views.Num(); ++i)
	{
		if (const FSceneView* V = Family.Views[i])
		{
			if (MagicSRIsPrimaryGameView(*V))
			{
				return true;
			}
		}
	}
	return false;
}
} // namespace

void FMagicSRSceneViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	if (MagicSRIsPrimaryGameView(InView) && CVarMagicSRResetViewState.GetValueOnGameThread() != 0 &&
		InView.State != nullptr)
	{
		const uint32 Before = InView.State->GetCurrentTemporalAASampleIndex();
		InView.State->ResetViewState();
		const uint32 After = InView.State->GetCurrentTemporalAASampleIndex();
		CVarMagicSRResetViewState->Set(0, ECVF_SetByCode);
		UE_LOG(LogTemp, Display, TEXT("[MagicSR] VIEW_RESET sample %u -> %u"), Before, After);
	}

	if (!FMagicSRViewportBridge::Get().IsEnabled() || !MagicSRIsPrimaryGameView(InView))
	{
		return;
	}

	// TSR, not Gen4 TAA: the jitter length is 8 scaled by 1/fraction^2 then the
	// next prime (37 at 50%). The upscaler interface is installed later, in
	// PrePostProcess, so this view still takes the TSR branch while the
	// subpixel sequence and the material mip bias are chosen.
	InView.AntiAliasingMethod = EAntiAliasingMethod::AAM_TSR;
	InView.PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::TemporalUpscale;
}

void FMagicSRSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	if (!FMagicSRViewportBridge::Get().IsEnabled() || !MagicSRFamilyHasPrimaryGameView(InViewFamily))
	{
		return;
	}
	// Do not install the upscaler yet. GetMainTAAPassConfig returns ThirdParty
	// as soon as the interface is set, and that shortens the jitter sequence
	// from TSR's 37 phases to r.TemporalAASamples scaled to 32.
	PendingUpscaleFamily = &InViewFamily;
}

void FMagicSRSceneViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
	(void)GraphBuilder;
	if (!MagicSRIsPrimaryGameView(InView))
	{
		return;
	}

	// FViewInfo::ViewRect is the internal render size after screen percentage.
	// UnscaledViewRect is the output size. Log only when either changes.
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
	if (ViewInfo.ViewRect.Width() > 0 && ViewInfo.ViewRect.Height() > 0)
	{
		FMagicSRViewportBridge::Get().SetPrimaryRenderViewRect(ViewInfo.ViewRect);
	}
	const FIntPoint InSz = ViewInfo.ViewRect.Size();
	const FIntPoint OutSz = InView.UnscaledViewRect.Size();
	const int32 AA = static_cast<int32>(InView.AntiAliasingMethod);
	const int32 Method = static_cast<int32>(InView.PrimaryScreenPercentageMethod);
	if (InSz.X <= 0 || InSz.Y <= 0 || OutSz.X <= 0 || OutSz.Y <= 0)
	{
		return;
	}

	const float MipBias = ViewInfo.MaterialTextureMipBias;
	const int32 JLen = ViewInfo.TemporalJitterSequenceLength;
	const int32 JIdx = ViewInfo.TemporalJitterIndex;
	const FVector2D Jit = ViewInfo.TemporalJitterPixels;
	static FIntPoint LastIn(ForceInit);
	static FIntPoint LastOut(ForceInit);
	static int32 LastAA = -1;
	static int32 LastMethod = -1;
	static int32 LastJLen = -1;
	static int32 LastMipQ = 0x7fffffff;
	const int32 MipQ = FMath::RoundToInt(MipBias * 1000.f);
	if (InSz == LastIn && OutSz == LastOut && AA == LastAA && Method == LastMethod &&
		JLen == LastJLen && MipQ == LastMipQ)
	{
		return;
	}
	LastIn = InSz;
	LastOut = OutSz;
	LastAA = AA;
	LastMethod = Method;
	LastJLen = JLen;
	LastMipQ = MipQ;
	UE_LOG(LogTemp, Display,
		   TEXT("[MagicSR] VIEW_RES in=%dx%d out=%dx%d aa=%d spmethod=%d mipbias=%.4f jlen=%d jidx=%d jitter=(%.4f,%.4f)"),
		   InSz.X, InSz.Y, OutSz.X, OutSz.Y, AA, Method, MipBias, JLen, JIdx, Jit.X, Jit.Y);
}

void FMagicSRSceneViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	(void)GraphBuilder;
	(void)RenderTargets;
	(void)SceneTextures;
	if (!MagicSRIsPrimaryGameView(InView))
	{
		return;
	}
	MagicSRInstallUpscaler(const_cast<FSceneViewFamily*>(InView.Family));
}

void FMagicSRSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	(void)bIsPassEnabled;
	if (Pass == EPostProcessingPass::Tonemap && MagicSRIsPrimaryGameView(InView) &&
		FMagicSRViewportBridge::GetFrameGenMode() > 0)
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(
			this, &FMagicSRSceneViewExtension::PostProcessPassAfterTonemap_RenderThread));
	}
}

FScreenPassTexture FMagicSRSceneViewExtension::PostProcessPassAfterTonemap_RenderThread(
	FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessMaterialInputs& Inputs)
{
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
	FScreenPassTexture SceneColor =
		FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	if (!SceneColor.IsValid() || !SceneColor.Texture)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	FRDGTextureRef SceneDepth = nullptr;
	FRDGTextureRef Velocity = nullptr;
	if (Inputs.SceneTextures.SceneTextures)
	{
		const FSceneTextureUniformParameters* SceneTex = Inputs.SceneTextures.SceneTextures->GetContents();
		if (SceneTex)
		{
			SceneDepth = SceneTex->SceneDepthTexture;
			Velocity = SceneTex->GBufferVelocityTexture;
		}
	}

	FMagicSRViewportBridge::Get().NoteCameraStill_RenderThread(
		InView.ViewLocation, InView.ViewRotation, InView.bCameraCut);
	FIntRect DepthRect = ViewInfo.ViewRect;
	if (FMagicSRViewportBridge::Get().HasPrimaryRenderViewRect())
	{
		const FIntRect Primary = FMagicSRViewportBridge::Get().GetPrimaryRenderViewRect();
		const FIntPoint DepthExtent = SceneDepth ? SceneDepth->Desc.Extent : FIntPoint::ZeroValue;
		if (Primary.Width() > 0 && Primary.Height() > 0 &&
			(DepthExtent.X <= 0 || (Primary.Max.X <= DepthExtent.X && Primary.Max.Y <= DepthExtent.Y)))
		{
			DepthRect = Primary;
		}
	}
	const FIntPoint OutExtent = SceneColor.ViewRect.Size().X > 0 ? SceneColor.ViewRect.Size()
																 : SceneColor.Texture->Desc.Extent;
	FRDGTextureRef FgOut = FMagicSRViewportBridge::Get().AddFrameGenPass_RenderThread(
		GraphBuilder, InView, SceneColor.Texture, SceneDepth, Velocity, DepthRect, OutExtent,
		/*bVelocityAlreadyDecoded=*/false, InView.bCameraCut);

	if (FgOut && FMagicSRViewportBridge::GetFrameGenShowMid())
	{
		if (Inputs.OverrideOutput.IsValid())
		{
			AddDrawTexturePass(GraphBuilder, ViewInfo, FgOut, Inputs.OverrideOutput.Texture,
							   FIntPoint::ZeroValue, FgOut->Desc.Extent, Inputs.OverrideOutput.ViewRect.Min,
							   Inputs.OverrideOutput.ViewRect.Size());
			return FScreenPassTexture(Inputs.OverrideOutput);
		}
		return FScreenPassTexture(FgOut, FIntRect(FIntPoint::ZeroValue, FgOut->Desc.Extent));
	}
	return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
}

void FMagicSRSceneViewExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs)
{
	(void)GraphBuilder;
	(void)Inputs;
	// Jitter is chosen before the base pass. Install after the base pass so the
	// TSR flickering scope (which still sees a TSR config until an upscaler is
	// set) does not run, and again in PrePostProcess in case that family was forked.
	if (!MagicSRIsPrimaryGameView(InView))
	{
		return;
	}

	FSceneViewFamily* RenderFamily = const_cast<FSceneViewFamily*>(InView.Family);
	MagicSRInstallUpscaler(RenderFamily);
	PendingUpscaleFamily = nullptr;
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
	const FIntPoint InSz = ViewInfo.ViewRect.Size();
	const FIntPoint OutSz = InView.UnscaledViewRect.Size();
	if (InSz.X <= 0 || InSz.Y <= 0 || OutSz.X <= 0 || OutSz.Y <= 0)
	{
		return;
	}
	const float MipBias = ViewInfo.MaterialTextureMipBias;
	const int32 JLen = ViewInfo.TemporalJitterSequenceLength;
	const int32 MipQ = FMath::RoundToInt(MipBias * 1000.f);
	const int32 AA = static_cast<int32>(InView.AntiAliasingMethod);
	static int32 LastAA = -2;
	static int32 LastJLen = -2;
	static int32 LastMipQ = 0x7fffffff;
	static FIntPoint LastIn(ForceInit);
	if (AA == LastAA && JLen == LastJLen && MipQ == LastMipQ && InSz == LastIn)
	{
		return;
	}
	LastAA = AA;
	LastJLen = JLen;
	LastMipQ = MipQ;
	LastIn = InSz;
	const FVector2D Jit = ViewInfo.TemporalJitterPixels;
	UE_LOG(LogTemp, Display,
		   TEXT("[MagicSR] VIEW_INPUT in=%dx%d out=%dx%d aa=%d mipbias=%.4f jlen=%d jidx=%d jitter=(%.4f,%.4f)"),
		   InSz.X, InSz.Y, OutSz.X, OutSz.Y, AA, MipBias, JLen, ViewInfo.TemporalJitterIndex, Jit.X, Jit.Y);
}
