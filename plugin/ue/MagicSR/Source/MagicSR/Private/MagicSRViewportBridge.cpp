#include "MagicSRViewportBridge.h"

#include "MagicSRBlueprintLibrary.h"
#include "MagicSRRHI.h"
#include "MagicSRShaders.h"

#include "SceneRendering.h"
#include "SceneView.h"
#include "ScreenPass.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "RenderGraphBuilder.h"
#include "RenderGraph.h"
#include "RenderGraphUtils.h"
#include "RendererInterface.h"
#include "RenderCore.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHITransition.h"

#if PLATFORM_WINDOWS || PLATFORM_LINUX
#include "IVulkanDynamicRHI.h"
#endif

#if MAGIC_SR_SUPPORTED
extern "C" {
#include "mc_interface.h"
}
#endif

namespace
{
TRefCountPtr<IPooledRenderTarget> GMagicSRZeroMask;
TRefCountPtr<IPooledRenderTarget> GMagicSRReactiveMask;

TAutoConsoleVariable<int32> CVarMagicSRTemporalMode(
	TEXT("r.MagicSR.TemporalMode"),
	0,
	TEXT("Viewport temporal SR bits. 0=SR_SPEED_MODE_FOR_GRAPHIC (was TEMPORAL_SPEED_MODE=2), 1=SR_BALANCED_MODE_FOR_GRAPHIC (was TEMPORAL_BALANCED_MODE=3). Recreates the handle."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarMagicSRLibrarySharpness(
	TEXT("r.MagicSR.LibrarySharpness"),
	0.2f,
	TEXT("Writes Tf.enable_sharpening and sharpness for viewport temporal Speed and Balanced. Range is [0,1]. Default is 0.2. 0 disables sharpening. Nonzero values in range enable it. Values outside [0,1] are rejected by the library (-105007)."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarMagicSRFrameGen(
	TEXT("r.MagicSR.FrameGen"),
	0,
	TEXT("0=Off, 1=GEN_BALANCED_MODE_FOR_GRAPHIC (FG-CoreV3), 2=GEN_SPEED_MODE_FOR_GRAPHIC (FG-Fast). Combined into the nscaler handle as SR_* | GEN_*."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarMagicSRFrameGenShowMid(
	TEXT("r.MagicSR.FrameGenShowMid"),
	0,
	TEXT("0=Normal real frame output, 1=Display generated midpoint frame t=0.5"),
	ECVF_RenderThreadSafe);

#if !MAGICSR_FG_BASELINE
TAutoConsoleVariable<int32> CVarMagicSRFrameGenLowRes(
	TEXT("r.MagicSR.FrameGenLowRes"),
	0,
	TEXT("0=FG at OutputExtent (default, zero extra work). ")
	TEXT("1=H3: FG at render res then Spatial SPEED 2x — blocked on Vulkan RDG (library submit_and_wait); falls back to 0. ")
	TEXT("2=B0 diagnostic: FG at render res then bilinear 2x. Fast + viewport temporal SP=50 only."),
	ECVF_RenderThreadSafe);
#endif

TRefCountPtr<IPooledRenderTarget> GMagicFgI0Color;
TRefCountPtr<IPooledRenderTarget> GMagicFgI0Depth;

int32 ViewportTemporalMode()
{
	return CVarMagicSRTemporalMode.GetValueOnRenderThread() != 0 ? 1 : 0;
}

int32 ViewportWantAlgMode()
{
#if MAGIC_SR_SUPPORTED
	const int32 Sr = ViewportTemporalMode() != 0
		? static_cast<int32>(SR_BALANCED_MODE_FOR_GRAPHIC)
		: static_cast<int32>(SR_SPEED_MODE_FOR_GRAPHIC);
	const int32 Fg = CVarMagicSRFrameGen.GetValueOnRenderThread();
	if (Fg == 2)
	{
		return Sr | static_cast<int32>(GEN_SPEED_MODE_FOR_GRAPHIC);
	}
	if (Fg == 1)
	{
		return Sr | static_cast<int32>(GEN_BALANCED_MODE_FOR_GRAPHIC);
	}
	return Sr;
#else
	return ViewportTemporalMode();
#endif
}
}

#if MAGIC_SR_SUPPORTED
static FString MagicSRFgWeightPath();
#endif

FMagicSRViewportBridge& FMagicSRViewportBridge::Get()
{
	static FMagicSRViewportBridge GInstance;
	return GInstance;
}

void FMagicSRViewportBridge::RequestHistoryReset()
{
	FScopeLock Lock(&Mutex);
	PendingHistoryReset = 1;
}

bool FMagicSRViewportBridge::ConsumeHistoryReset()
{
	FScopeLock Lock(&Mutex);
	if (PendingHistoryReset == 0)
	{
		return false;
	}
	PendingHistoryReset = 0;
	FrameCounter = 0;
	return true;
}

void FMagicSRViewportBridge::SetEnabled(bool bInEnabled)
{
	FScopeLock Lock(&Mutex);
	if (bEnabled == bInEnabled)
	{
		return;
	}
	bEnabled = bInEnabled;
	bCreateFailed = false;
	if (!bEnabled)
	{
		// Keep the handle alive across preset toggles; reset history on the next enable.
		PendingHistoryReset = 1;
	}
}

bool FMagicSRViewportBridge::QueryCumulativeGpuSeconds(double& OutSeconds)
{
	OutSeconds = 0.0;
#if !MAGIC_SR_SUPPORTED
	return false;
#else
	bool bOk = false;
	double Seconds = 0.0;
	ENQUEUE_RENDER_COMMAND(MagicSRQueryGpuSeconds)(
		[&bOk, &Seconds](FRHICommandListImmediate&)
		{
			FMagicSRViewportBridge& Self = FMagicSRViewportBridge::Get();
			if (Self.Handle == nullptr)
			{
				return;
			}
			output_status_params_t Status;
			FMemory::Memzero(&Status, sizeof(Status));
			void* HandlePtr = Self.Handle;
			if (mc_nscaler_control(&HandlePtr, MC_NSCALER_CMD_QUERY_STATUS, nullptr, &Status) != 0)
			{
				Self.Handle = HandlePtr;
				return;
			}
			Self.Handle = HandlePtr;
			Seconds = Status.gpu_time;
			bOk = true;
		});
	FlushRenderingCommands();
	OutSeconds = Seconds;
	return bOk;
#endif
}

void FMagicSRViewportBridge::DestroyHandle_RenderThread()
{
#if MAGIC_SR_SUPPORTED
	if (Handle)
	{
		FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		if (GDynamicRHI)
		{
			GDynamicRHI->RHIBlockUntilGPUIdle();
		}
		mc_nscaler_disable(Handle);
		Handle = nullptr;
	}
#endif
	HandleInputW = 0;
	HandleInputH = 0;
	HandleScale = 0.0f;
	HandleAlgMode = -1;
	FrameCounter = 0;
	bHaveCamPose = false;
	LastCamPoseFrame = 0xffffffffu;
	bHavePrimaryRenderViewRect = false;
	FramesSinceCameraStill = 0;
	bCreateFailed = false;
	GMagicSRZeroMask.SafeRelease();
	GMagicSRReactiveMask.SafeRelease();
	DestroyFgHandle_RenderThread();
	GMagicFgI0Color.SafeRelease();
	GMagicFgI0Depth.SafeRelease();
	ReleasePresentResources_RenderThread();
}

float FMagicSRViewportBridge::NoteCameraStill_RenderThread(const FVector& Loc, const FRotator& Rot, bool bReset)
{
	if (LastCamPoseFrame == GFrameNumberRenderThread)
	{
		return static_cast<float>(FramesSinceCameraStill);
	}
	LastCamPoseFrame = GFrameNumberRenderThread;
	const float LocEps = 0.01f;
	const float RotEps = 0.001f;
	const bool bMoved = !bHaveCamPose || !Loc.Equals(PrevCamLoc, LocEps) ||
		FMath::Abs(FRotator::NormalizeAxis(Rot.Pitch - PrevCamRot.Pitch)) > RotEps ||
		FMath::Abs(FRotator::NormalizeAxis(Rot.Yaw - PrevCamRot.Yaw)) > RotEps ||
		FMath::Abs(FRotator::NormalizeAxis(Rot.Roll - PrevCamRot.Roll)) > RotEps;
	PrevCamLoc = Loc;
	PrevCamRot = Rot;
	bHaveCamPose = true;
	if (bReset || bMoved)
	{
		FramesSinceCameraStill = 0;
	}
	else if (FramesSinceCameraStill < 1000000)
	{
		++FramesSinceCameraStill;
	}
	return static_cast<float>(FramesSinceCameraStill);
}

bool FMagicSRViewportBridge::EnsureHandle_RenderThread(int32 InputW, int32 InputH, float Scale)
{
	const int32 WantMode = ViewportWantAlgMode();
	if (InputW <= 0 || InputH <= 0 || Scale <= 1.0f)
	{
		return false;
	}
	if (Handle && HandleInputW == InputW && HandleInputH == InputH &&
		FMath::IsNearlyEqual(HandleScale, Scale) && HandleAlgMode == WantMode)
	{
		return true;
	}
	DestroyHandle_RenderThread();
	if (bCreateFailed)
	{
		return false;
	}
	return CreateHandle_RenderThread(InputW, InputH, Scale);
}

bool FMagicSRViewportBridge::CreateHandle_RenderThread(int32 InputW, int32 InputH, float Scale)
{
#if !MAGIC_SR_SUPPORTED
	bCreateFailed = true;
	return false;
#else
	if (GDynamicRHI == nullptr || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::Vulkan)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Viewport Temporal requires Vulkan RHI"));
		bCreateFailed = true;
		return false;
	}

	ctrl_param_t Ctrl;
	FMemory::Memzero(&Ctrl, sizeof(Ctrl));
	Ctrl.input_type = INPUT_TEXTURE_RGB8Unorm; // per-resource formats override at enable
	Ctrl.width = static_cast<unsigned int>(InputW);
	Ctrl.height = static_cast<unsigned int>(InputH);
	Ctrl.scaler_factor = Scale;
	const int32 WantMode = ViewportWantAlgMode();
	Ctrl.alg_mode = static_cast<alg_mode_e>(WantMode);
	Ctrl.backend = MAGIC_BACKEND_VULKAN;
	// WARNING so the one-shot Balanced "sharpness=" line is not filtered.
	// Speed RCAS already logs at ERROR. Neither line is per-frame.
	Ctrl.log_level = MAGIC_LOG_WARNING;
	// CopyDepth stores 1-deviceZ so 0=near. Convert dilates with min() and ignores the reversed flag.
	Ctrl.depth_reversed = MC_DEPTH_REVERSED_CONVENTIONAL;
	Ctrl.depth_infinite = MC_DEPTH_INFINITE_YES;
	Ctrl.hdr_color = MC_HDR_COLOR_HDR; // UE SceneColor is HDR (R11G11B10 / RGBA16F)

#if PLATFORM_WINDOWS || PLATFORM_LINUX
	IVulkanDynamicRHI* VulkanRHI = GetIVulkanDynamicRHI();
	Ctrl.gpu_context.physical_device = VulkanRHI->RHIGetVkPhysicalDevice();
	Ctrl.gpu_context.device = VulkanRHI->RHIGetVkDevice();
	Ctrl.gpu_context.native_context = VulkanRHI->RHIGetVkInstance();
	void* Gipa = VulkanRHI->RHIGetVkInstanceGlobalProcAddr("vkGetInstanceProcAddr");
	if (Gipa == nullptr)
	{
		Gipa = VulkanRHI->RHIGetVkInstanceProcAddr("vkGetInstanceProcAddr");
	}
	Ctrl.gpu_context.device_context = Gipa;
	Ctrl.gpu_context.get_device_proc_addr = VulkanRHI->RHIGetVkDeviceProcAddr("vkGetDeviceProcAddr");
#endif

	FString ModelPath = UMagicSRBlueprintLibrary::GetBundledModelPath(true);
	if ((WantMode & 0xC) == static_cast<int32>(GEN_BALANCED_MODE_FOR_GRAPHIC))
	{
		ModelPath = MagicSRFgWeightPath();
	}
	{
		FTCHARToUTF8 Utf8(*ModelPath);
		FCStringAnsi::Strncpy(Ctrl.model_path, Utf8.Get(), sizeof(Ctrl.model_path) - 1);
	}

	void* NewHandle = nullptr;
	const int Rc = mc_nscaler_control(&NewHandle, MC_NSCALER_CMD_SET_PARAM, &Ctrl, nullptr);
	if (Rc != 0 || NewHandle == nullptr)
	{
		UE_LOG(LogTemp, Error,
			   TEXT("[MagicSR] Viewport CreateHandle rc=%d in=%dx%d scale=%.3f model=%s"),
			   Rc, InputW, InputH, Scale, *ModelPath);
		bCreateFailed = true;
		return false;
	}

	Handle = NewHandle;
	HandleInputW = InputW;
	HandleInputH = InputH;
	HandleScale = Scale;
	HandleAlgMode = WantMode;
	FrameCounter = 0;
	UE_LOG(LogTemp, Display,
		   TEXT("[MagicSR] Viewport handle ready in=%dx%d scale=%.3f alg_mode=%d (%d|%d=%d) ver=%s"),
		   InputW, InputH, Scale, WantMode, WantMode & 0x3, WantMode & 0xC, WantMode,
		   UTF8_TO_TCHAR(mc_nscaler_version()));
	return true;
#endif
}

FRDGTextureRef FMagicSRViewportBridge::AcquireZeroMask_RenderThread(
	FRDGBuilder& GraphBuilder, const FIntPoint& Extent)
{
	if (GMagicSRZeroMask.IsValid() && GMagicSRZeroMask->GetDesc().Extent == Extent)
	{
		return GraphBuilder.RegisterExternalTexture(GMagicSRZeroMask, TEXT("MagicSR.ZeroMask"));
	}

	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		Extent,
		PF_R16F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
	FRDGTextureRef Tex = GraphBuilder.CreateTexture(Desc, TEXT("MagicSR.ZeroMask"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Tex), FVector4(0, 0, 0, 0));
	GraphBuilder.QueueTextureExtraction(Tex, &GMagicSRZeroMask);
	return Tex;
}

FRDGTextureRef FMagicSRViewportBridge::AcquireReactiveMask_RenderThread(
	FRDGBuilder& GraphBuilder, const FIntPoint& Extent)
{
	FRDGTextureRef Tex = nullptr;
	if (GMagicSRReactiveMask.IsValid() && GMagicSRReactiveMask->GetDesc().Extent == Extent)
	{
		Tex = GraphBuilder.RegisterExternalTexture(GMagicSRReactiveMask, TEXT("MagicSR.ReactiveMask"));
	}
	else
	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
			Extent,
			PF_R16F,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
		Tex = GraphBuilder.CreateTexture(Desc, TEXT("MagicSR.ReactiveMask"));
	}
	return Tex;
}

void FMagicSRViewportBridge::QueueReactiveMaskExtraction_RenderThread(
	FRDGBuilder& GraphBuilder, FRDGTextureRef Tex)
{
	if (Tex)
	{
		GraphBuilder.QueueTextureExtraction(Tex, &GMagicSRReactiveMask);
	}
}

int32 FMagicSRViewportBridge::ProcessTemporal_RenderThread(
	FRHICommandList& RHICmdList,
	FRHITexture* Color,
	FRHITexture* Depth,
	FRHITexture* Motion,
	FRHITexture* Output,
	FRHITexture* Reactive,
	FRHITexture* Transparency,
	int32 InputW,
	int32 InputH,
	FVector2f JitterPixels,
	float PreExposure,
	float CameraNear,
	float CameraFar,
	float CameraFovYRad,
	bool bResetHistory,
	uint32 FrameIndex,
	float InFramesSinceStill)
{
#if !MAGIC_SR_SUPPORTED
	return -3999;
#else
	LastJitter = JitterPixels;
	LastInputSize = FIntPoint(InputW, InputH);
	if (Output)
	{
		LastOutputSize = FIntPoint(Output->GetSizeX(), Output->GetSizeY());
	}

	if (!Color || !Depth || !Motion || !Output || !Reactive || !Transparency)
	{
		LastProcessRc = -3008;
		return LastProcessRc;
	}

	FMagicSRNativeResource ColorN, DepthN, MotionN, OutN, ReactiveN, TransparencyN;
	if (!MagicSRRHI::ResolveTexture(Color, ColorN) || !MagicSRRHI::ResolveTexture(Depth, DepthN) ||
		!MagicSRRHI::ResolveTexture(Motion, MotionN) || !MagicSRRHI::ResolveTexture(Output, OutN) ||
		!MagicSRRHI::ResolveTexture(Reactive, ReactiveN) ||
		!MagicSRRHI::ResolveTexture(Transparency, TransparencyN))
	{
		LastProcessRc = -3008;
		return LastProcessRc;
	}

	temporal_frame_t Tf;
	FMemory::Memzero(&Tf, sizeof(Tf));
	Tf.struct_size = sizeof(Tf);
	Tf.jitter_offset_x = JitterPixels.X;
	Tf.jitter_offset_y = JitterPixels.Y;
	// Decoded UV motion; (0,0) → library defaults to (W,H).
	Tf.motion_vector_scale_x = 0.0f;
	Tf.motion_vector_scale_y = 0.0f;
	Tf.frame_time_delta_ms = 16.666f;
	// Consecutive per-handle index (View.Family FrameNumber can skip / multi-view).
	Tf.frame_index = FrameCounter;
	Tf.reset_history = bResetHistory ? 1 : 0;
	Tf.camera_near = CameraNear;
	Tf.camera_far = CameraFar;
	Tf.camera_fov_y = CameraFovYRad;
	Tf.view_to_meters = 0.01f; // UE cm → meters
	// Viewport path freezes exposure at 1.0 (caller); keep guard for non-finite.
	Tf.pre_exposure = (PreExposure > 0.0f && FMath::IsFinite(PreExposure)) ? PreExposure : 1.0f;
	{
		const float LibSharp = CVarMagicSRLibrarySharpness.GetValueOnRenderThread();
		if (LibSharp != 0.0f)
		{
			Tf.enable_sharpening = 1u;
			Tf.sharpness = LibSharp;
		}
		else
		{
			Tf.enable_sharpening = 0u;
			Tf.sharpness = 0.0f;
		}
	}
	Tf.mv_jitter = MC_MV_JITTER_EXCLUDED; // UE velocity has jitter cancelled
	Tf.frames_since_camera_still = (InFramesSinceStill > 0.0f && FMath::IsFinite(InFramesSinceStill))
		? InFramesSinceStill
		: 0.0f;
	MagicSRRHI::FillColorResource(DepthN, Tf.depth);
	MagicSRRHI::FillColorResource(MotionN, Tf.motion);
	MagicSRRHI::FillColorResource(ReactiveN, Tf.reactive);
	MagicSRRHI::FillColorResource(TransparencyN, Tf.transparency);
	// Depth is conventional R32F (1-deviceZ). Masks are explicit zeros so auto photo-residual stays off.
	Tf.depth.layout = 5u;  // SHADER_READ_ONLY_OPTIMAL
	Tf.motion.layout = 5u;
	Tf.reactive.layout = 5u;
	Tf.transparency.layout = 5u;

	mc_nscaler_input_frame_t In;
	mc_nscaler_output_frame_t Out;
	FMemory::Memzero(&In, sizeof(In));
	FMemory::Memzero(&Out, sizeof(Out));
	MagicSRRHI::FillDataHandle(ColorN, In.handle);
	MagicSRRHI::FillDataHandle(OutN, Out.handle);
	In.format = ColorN.Format;
	Out.format = OutN.Format;
	In.layout = 5u; // SHADER_READ_ONLY_OPTIMAL
	Out.layout = 1u; // GENERAL for UAV write
	In.mip_count = 1;
	Out.mip_count = 1;
	In.width = static_cast<unsigned int>(InputW);
	In.height = static_cast<unsigned int>(InputH);
	Out.width = static_cast<unsigned int>(Output->GetSizeX());
	Out.height = static_cast<unsigned int>(Output->GetSizeY());
	In.frame = &Tf;

#if PLATFORM_WINDOWS || PLATFORM_LINUX
	if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan)
	{
		// Transitions record on the RDG list; Vk cmd buffer needs a live graphics context
		// (parallel translate has none) — get it inside EnqueueLambda like ProcessTemporal RT path.
		RHICmdList.Transition(FRHITransitionInfo(Color, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(Depth, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(Motion, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(Reactive, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(Transparency, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		const bool bOutUAV = EnumHasAnyFlags(Output->GetDesc().Flags, ETextureCreateFlags::UAV);
		if (bOutUAV)
		{
			RHICmdList.Transition(FRHITransitionInfo(Output, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		}

		void** HandlePtr = &Handle;
		int32* RcPtr = &LastProcessRc;
		const uint32 LogEvery = FrameCounter;
		const FIntPoint LogOut = LastOutputSize;
		RHICmdList.EnqueueLambda(
			[HandlePtr, RcPtr, In, Out, Tf, InputW, InputH, JitterPixels, LogEvery,
			 LogOut](FRHICommandListBase& ExecutingCmdList) mutable
			{
				if (HandlePtr == nullptr || *HandlePtr == nullptr)
				{
					*RcPtr = -3001;
					return;
				}
				In.frame = &Tf;
				IVulkanDynamicRHI* VulkanRHI = GetIVulkanDynamicRHI();
				In.command_buffer =
					reinterpret_cast<void*>(VulkanRHI->RHIGetVkCommandBuffer(ExecutingCmdList));
				mc_nscaler_input_frame_t* Frames[2] = { &In, nullptr };
				*RcPtr = mc_nscaler_enable(HandlePtr, Frames, &Out);
				if ((LogEvery % 120u) == 0u)
				{
					UE_LOG(LogTemp, Display,
						   TEXT("[MagicSR] Viewport process rc=%d in=%dx%d out=%dx%d jitter=(%.3f,%.3f) frame=%u cut=%d sharp=%.3f en=%u still=%.0f"),
						   *RcPtr, InputW, InputH, LogOut.X, LogOut.Y, JitterPixels.X, JitterPixels.Y,
						   Tf.frame_index, Tf.reset_history, Tf.sharpness, Tf.enable_sharpening,
						   Tf.frames_since_camera_still);
				}
			});

		RHICmdList.Transition(FRHITransitionInfo(
			Output, bOutUAV ? ERHIAccess::UAVCompute : ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	}
	else
#endif
	{
		mc_nscaler_input_frame_t* Frames[2] = { &In, nullptr };
		LastProcessRc = mc_nscaler_enable(&Handle, Frames, &Out);
		if ((FrameCounter % 120u) == 0u)
		{
			UE_LOG(LogTemp, Display,
				   TEXT("[MagicSR] Viewport process rc=%d in=%dx%d out=%dx%d jitter=(%.3f,%.3f) frame=%u"),
				   LastProcessRc, InputW, InputH, LastOutputSize.X, LastOutputSize.Y, JitterPixels.X,
				   JitterPixels.Y, FrameIndex);
		}
	}

	++FrameCounter;
	return LastProcessRc;
#endif
}

int32 FMagicSRViewportBridge::GetFrameGenMode()
{
	return IsInRenderingThread() ? CVarMagicSRFrameGen.GetValueOnRenderThread()
								 : CVarMagicSRFrameGen.GetValueOnGameThread();
}

bool FMagicSRViewportBridge::GetFrameGenShowMid()
{
	const int32 V = IsInRenderingThread() ? CVarMagicSRFrameGenShowMid.GetValueOnRenderThread()
										  : CVarMagicSRFrameGenShowMid.GetValueOnGameThread();
	return V != 0;
}

static FString MagicSRFgWeightPath()
{
	TArray<FString> Candidates;
	Candidates.Add(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("MagicSRModels"), TEXT("magic_fg_core.bin")));
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MagicSR")))
	{
		Candidates.Add(FPaths::Combine(Plugin->GetBaseDir(), TEXT("ThirdParty/MagicSR/models"), TEXT("magic_fg_core.bin")));
	}
	Candidates.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/MagicSR/ThirdParty/MagicSR/models"), TEXT("magic_fg_core.bin")));
	for (const FString& Path : Candidates)
	{
		if (FPaths::FileExists(Path))
		{
			return Path;
		}
	}
	return Candidates.Num() > 0 ? Candidates[0] : FString();
}

void FMagicSRViewportBridge::DestroyFgHandle_RenderThread()
{
	// 3.0.0: FG lives inside the nscaler handle. No separate FG destroy.
	FgHandle = nullptr;
	FgMode = 0;
	FgMaxW = 0;
	FgMaxH = 0;
	FgInputW = 0;
	FgInputH = 0;
	bFgCreateFailed = false;
	LastFgStatus = 0;
	LastFgRc = 0;
	FgPrevVpValid = 0;
}

bool FMagicSRViewportBridge::EnsureFgHandle_RenderThread(int32 Width, int32 Height, int32 Mode, bool bExactExtent)
{
#if !MAGIC_SR_SUPPORTED
	return false;
#else
	(void)bExactExtent;
	if (Width <= 0 || Height <= 0 || Mode <= 0)
	{
		return false;
	}
	if (!Handle)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MagicFG] GEN needs the viewport nscaler handle (SR+GEN). mode=%d"), Mode);
		LastFgStatus = 5;
		return false;
	}
	const int32 Want = ViewportWantAlgMode();
	if ((Want & 0xC) == 0)
	{
		return false;
	}
	if (HandleAlgMode != Want)
	{
		if (!EnsureHandle_RenderThread(HandleInputW, HandleInputH, HandleScale))
		{
			bFgCreateFailed = true;
			return false;
		}
	}
	if ((HandleAlgMode & 0xC) == 0)
	{
		bFgCreateFailed = true;
		return false;
	}
	if (FgMode != Mode || FgInputW != Width || FgInputH != Height)
	{
		UE_LOG(LogTemp, Display,
			   TEXT("[MagicFG] nscaler GEN ready mode=%d alg_mode=%d (%d|%d=%d) render=%dx%d color_out=%dx%d"),
			   Mode, HandleAlgMode, HandleAlgMode & 0x3, HandleAlgMode & 0xC, HandleAlgMode,
			   HandleInputW, HandleInputH, Width, Height);
	}
	FgMode = Mode;
	FgMaxW = Width;
	FgMaxH = Height;
	FgInputW = Width;
	FgInputH = Height;
	return true;
#endif
}

int32 FMagicSRViewportBridge::ProcessFrameGen_RenderThread(
	FRHICommandList& RHICmdList,
	FRHITexture* I0Color,
	FRHITexture* I0Depth,
	FRHITexture* I1Color,
	FRHITexture* I1Depth,
	FRHITexture* I1Velocity,
	FRHITexture* Output,
	int32 Width,
	int32 Height,
	bool bCameraStill,
	bool bFast,
	const float* Vp0,
	const float* Vp1,
	uint32 HaveVp,
	float CameraNear,
	float CameraFar)
{
#if !MAGIC_SR_SUPPORTED
	return -3999;
#else
	(void)bFast;
	LastFgRc = 0;
	if (!I0Color || !I0Depth || !I1Color || !I1Depth || !I1Velocity || !Output || !Handle)
	{
		LastFgRc = -3008;
		LastFgStatus = 5;
		return LastFgRc;
	}

	FMagicSRNativeResource N0c, N0d, N1c, N1d, Nv, Nout;
	if (!MagicSRRHI::ResolveTexture(I0Color, N0c) || !MagicSRRHI::ResolveTexture(I0Depth, N0d) ||
		!MagicSRRHI::ResolveTexture(I1Color, N1c) || !MagicSRRHI::ResolveTexture(I1Depth, N1d) ||
		!MagicSRRHI::ResolveTexture(I1Velocity, Nv) || !MagicSRRHI::ResolveTexture(Output, Nout))
	{
		LastFgRc = -3008;
		LastFgStatus = 5;
		return LastFgRc;
	}

	temporal_frame_t Tf0;
	temporal_frame_t Tf1;
	FMemory::Memzero(&Tf0, sizeof(Tf0));
	FMemory::Memzero(&Tf1, sizeof(Tf1));
	Tf0.struct_size = sizeof(Tf0);
	Tf1.struct_size = sizeof(Tf1);
	Tf0.motion_vector_scale_x = 0.0f;
	Tf0.motion_vector_scale_y = 0.0f;
	Tf1.motion_vector_scale_x = 0.0f;
	Tf1.motion_vector_scale_y = 0.0f;
	Tf1.frames_since_camera_still = bCameraStill ? 2.0f : 0.0f;
	Tf0.camera_near = CameraNear;
	Tf1.camera_near = CameraNear;
	Tf0.camera_far = CameraFar;
	Tf1.camera_far = CameraFar;
	if (HaveVp && Vp0 && Vp1)
	{
		FMemory::Memcpy(Tf0.vp0, Vp0, sizeof(Tf0.vp0));
		FMemory::Memcpy(Tf0.vp1, Vp1, sizeof(Tf0.vp1));
		FMemory::Memcpy(Tf1.vp0, Vp0, sizeof(Tf1.vp0));
		FMemory::Memcpy(Tf1.vp1, Vp1, sizeof(Tf1.vp1));
		Tf0.have_view_projection = 1u;
		Tf1.have_view_projection = 1u;
	}
	MagicSRRHI::FillColorResource(N0d, Tf0.depth);
	MagicSRRHI::FillColorResource(N1d, Tf1.depth);
	MagicSRRHI::FillColorResource(Nv, Tf1.motion);
	Tf0.depth.layout = 5u;
	Tf1.depth.layout = 5u;
	Tf1.motion.layout = 5u;

	mc_nscaler_input_frame_t In0;
	mc_nscaler_input_frame_t In1;
	mc_nscaler_output_frame_t Out;
	FMemory::Memzero(&In0, sizeof(In0));
	FMemory::Memzero(&In1, sizeof(In1));
	FMemory::Memzero(&Out, sizeof(Out));
	MagicSRRHI::FillDataHandle(N0c, In0.handle);
	MagicSRRHI::FillDataHandle(N1c, In1.handle);
	MagicSRRHI::FillDataHandle(Nout, Out.handle);
	In0.format = N0c.Format;
	In1.format = N1c.Format;
	Out.format = Nout.Format;
	In0.layout = 5u;
	In1.layout = 5u;
	Out.layout = 1u;
	In0.mip_count = 1;
	In1.mip_count = 1;
	Out.mip_count = 1;
	const unsigned InW = static_cast<unsigned>(HandleInputW > 0 ? HandleInputW : I0Depth->GetSizeX());
	const unsigned InH = static_cast<unsigned>(HandleInputH > 0 ? HandleInputH : I0Depth->GetSizeY());
	In0.width = InW;
	In0.height = InH;
	In1.width = InW;
	In1.height = InH;
	Out.width = static_cast<unsigned>(Width);
	Out.height = static_cast<unsigned>(Height);
	In0.frame = &Tf0;
	In1.frame = &Tf1;

#if PLATFORM_WINDOWS || PLATFORM_LINUX
	if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan)
	{
		RHICmdList.Transition(FRHITransitionInfo(I0Color, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(I0Depth, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(I1Color, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(I1Depth, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(I1Velocity, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(Output, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

		void** HandlePtr = &Handle;
		int32* RcPtr = &LastFgRc;
		int32* StatusPtr = &LastFgStatus;
		const uint32 LogEvery = FrameCounter;
		RHICmdList.EnqueueLambda(
			[HandlePtr, RcPtr, StatusPtr, In0, In1, Out, Tf0, Tf1, InW, InH, Width, Height, LogEvery](
				FRHICommandListBase& ExecutingCmdList) mutable
			{
				if (HandlePtr == nullptr || *HandlePtr == nullptr)
				{
					*RcPtr = -3001;
					*StatusPtr = 5;
					return;
				}
				In0.frame = &Tf0;
				In1.frame = &Tf1;
				IVulkanDynamicRHI* VulkanRHI = GetIVulkanDynamicRHI();
				In0.command_buffer =
					reinterpret_cast<void*>(VulkanRHI->RHIGetVkCommandBuffer(ExecutingCmdList));
				mc_nscaler_input_frame_t* Frames[2] = { &In0, &In1 };
				*RcPtr = mc_nscaler_enable(HandlePtr, Frames, &Out);
				*StatusPtr = (*RcPtr == 0) ? 1 : 5;
				if ((LogEvery % 120u) == 0u)
				{
					UE_LOG(LogTemp, Display,
						   TEXT("[MagicFG] GEN rc=%d in=%ux%u out=%dx%d have_vp=%u"),
						   *RcPtr, InW, InH, Width, Height, Tf1.have_view_projection);
				}
			});
		RHICmdList.Transition(FRHITransitionInfo(Output, ERHIAccess::UAVCompute, ERHIAccess::SRVGraphics));
	}
	else
#endif
	{
		LastFgRc = -3998;
		LastFgStatus = 0;
	}
	return LastFgRc;
#endif
}

namespace
{
BEGIN_SHADER_PARAMETER_STRUCT(FMagicSRFgPassParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, I0Color)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, I0Depth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, I1Color)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, I1Depth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, I1Velocity)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, Output)
END_SHADER_PARAMETER_STRUCT()

FRDGTextureRef MagicSRMakeExtent(
	FRDGBuilder& GraphBuilder, const FViewInfo& View, FRDGTextureRef Src,
	const FIntPoint& Extent, EPixelFormat Format, const TCHAR* Name)
{
	if (Src && Src->Desc.Extent == Extent && Src->Desc.Format == Format)
	{
		return Src;
	}
	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		Extent, Format, FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
	FRDGTextureRef Dst = GraphBuilder.CreateTexture(Desc, Name);
	if (Src)
	{
		AddDrawTexturePass(GraphBuilder, View, Src, Dst, FIntPoint::ZeroValue, Src->Desc.Extent,
						   FIntPoint::ZeroValue, Extent);
	}
	else
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Dst), FLinearColor::Black);
	}
	return Dst;
}

struct FMagicFgVpFrame
{
	float Vp0[16];
	float Vp1[16];
	uint32 Valid = 0;
	float Near = 10.f;
	float Far = 0.f;
};

void MagicSRFillUnjitteredVp(const FSceneView& View, float Out[16])
{
	const FMatrix UeVp =
		View.ViewMatrices.GetWorldToView() * View.ViewMatrices.GetViewToClipNoAA();
	/* CopyDepth stores 1-DeviceZ (0=near). Fast pre.comp unprojects with
	 * OpenGL NDC z = dHw*2-1. Map UE reversed-Z Vulkan clip z onto that. */
	const FMatrix Conv(
		FPlane(1.f, 0.f, 0.f, 0.f),
		FPlane(0.f, 1.f, 0.f, 0.f),
		FPlane(0.f, 0.f, -2.f, 0.f),
		FPlane(0.f, 0.f, 1.f, 1.f));
	const FMatrix Vp = UeVp * Conv;
	FMemory::Memcpy(Out, &Vp.M[0][0], 16 * sizeof(float));
}
} // namespace

FRDGTextureRef FMagicSRViewportBridge::AddFrameGenPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef I1ColorIn,
	FRDGTextureRef SceneDepth,
	FRDGTextureRef EncodedOrDecodedVelocity,
	const FIntRect& DepthVelViewRect,
	const FIntPoint& OutputExtent,
	bool bVelocityAlreadyDecoded,
	bool bResetHistory)
{
	const int32 Mode = CVarMagicSRFrameGen.GetValueOnRenderThread();
	if (Mode <= 0 || OutputExtent.X <= 0 || OutputExtent.Y <= 0 || !I1ColorIn)
	{
		NoteGeneratedFrame_RenderThread(GraphBuilder, nullptr, nullptr);
		return nullptr;
	}

#if MAGICSR_FG_BASELINE
	const int32 WantLow = 0;
#else
	const int32 WantLow = CVarMagicSRFrameGenLowRes.GetValueOnRenderThread();
#endif
	int32 LowResMode = 0;
	if (WantLow > 0)
	{
		const FIntPoint RenderExtent = DepthVelViewRect.Size();
		const bool bFast = Mode == 2;
		const bool bSp50 = RenderExtent.X > 0 && RenderExtent.Y > 0 &&
						   FMath::Abs(OutputExtent.X - RenderExtent.X * 2) <= 4 &&
						   FMath::Abs(OutputExtent.Y - RenderExtent.Y * 2) <= 4;
		if (!(bFast && bSp50))
		{
			static int32 GLoggedLowResFallback = 0;
			if (GLoggedLowResFallback == 0)
			{
				GLoggedLowResFallback = 1;
				UE_LOG(LogTemp, Warning,
					   TEXT("[MagicFG] FrameGenLowRes=%d fallback to OutputExtent (need Fast + viewport temporal SP50) in=%dx%d out=%dx%d mode=%d"),
					   WantLow, RenderExtent.X, RenderExtent.Y, OutputExtent.X, OutputExtent.Y, Mode);
			}
		}
		else if (WantLow == 1)
		{
			static int32 GLoggedSpatialBlock = 0;
			if (GLoggedSpatialBlock == 0)
			{
				GLoggedSpatialBlock = 1;
				UE_LOG(LogTemp, Error,
					   TEXT("[MagicFG] FrameGenLowRes=1 blocked: Spatial SPEED_MODE Vulkan (vulkan_speed_sr_process) uses library CB + submit_and_wait; cannot record into UE RDG. Falling back to FG at OutputExtent. Use FrameGenLowRes=2 for B0 bilinear. SpatialSharpenLevel default 0 (unused)."));
			}
		}
		else if (WantLow == 2)
		{
			LowResMode = 2;
		}
		LastFgLowResWanted = WantLow;
		LastFgLowResMode = LowResMode;
		LastFgExtent = LowResMode > 0 ? RenderExtent : OutputExtent;
	}

	const FIntPoint FgExtent = LowResMode > 0 ? DepthVelViewRect.Size() : OutputExtent;
	if (!EnsureFgHandle_RenderThread(FgExtent.X, FgExtent.Y, Mode, LowResMode != 0))
	{
		LastFgStatus = 5;
		NoteGeneratedFrame_RenderThread(GraphBuilder, nullptr, nullptr);
		return nullptr;
	}

	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	FRDGTextureRef DepthTight = SceneDepth
		? AddMagicSRCopyDepthPass(GraphBuilder, ViewInfo, SceneDepth, DepthVelViewRect, DepthVelViewRect.Size())
		: nullptr;
	FRDGTextureRef VelTight = EncodedOrDecodedVelocity;
	if (!bVelocityAlreadyDecoded && EncodedOrDecodedVelocity && SceneDepth)
	{
		VelTight = AddMagicSRDecodeVelocityPass(
			GraphBuilder, ViewInfo, EncodedOrDecodedVelocity, SceneDepth, DepthVelViewRect,
			DepthVelViewRect.Size(), nullptr, 0.0f, 0.40f);
	}

	const FIntPoint RenderExtent = DepthVelViewRect.Size();
	FRDGTextureRef I1Color = nullptr;
	FRDGTextureRef I1Depth = nullptr;
	FRDGTextureRef I1Vel = nullptr;
	if (LowResMode == 2)
	{
		if (GMagicFgI0Color.IsValid() && GMagicFgI0Color->GetDesc().Extent != FgExtent)
		{
			GMagicFgI0Color.SafeRelease();
			GMagicFgI0Depth.SafeRelease();
		}
		RDG_EVENT_SCOPE(GraphBuilder, "MagicFG.LowResDownsample %dx%d", FgExtent.X, FgExtent.Y);
		I1Color = MagicSRMakeExtent(
			GraphBuilder, ViewInfo, I1ColorIn, FgExtent, PF_FloatRGBA, TEXT("MagicFG.I1Color"));
		I1Depth = MagicSRMakeExtent(
			GraphBuilder, ViewInfo, DepthTight, FgExtent, PF_R32_FLOAT, TEXT("MagicFG.I1Depth"));
		I1Vel = MagicSRMakeExtent(
			GraphBuilder, ViewInfo, VelTight, FgExtent, PF_G16R16F, TEXT("MagicFG.I1Velocity"));
	}
	else
	{
		// 3.0.0 GEN: color is post-SR output res; depth/MV stay at render res.
		I1Color = MagicSRMakeExtent(
			GraphBuilder, ViewInfo, I1ColorIn, OutputExtent, PF_FloatRGBA, TEXT("MagicFG.I1Color"));
		I1Depth = DepthTight;
		if (!I1Depth || I1Depth->Desc.Extent != RenderExtent || I1Depth->Desc.Format != PF_R32_FLOAT)
		{
			I1Depth = MagicSRMakeExtent(
				GraphBuilder, ViewInfo, DepthTight, RenderExtent, PF_R32_FLOAT, TEXT("MagicFG.I1Depth"));
		}
		I1Vel = VelTight;
		if (!I1Vel || I1Vel->Desc.Extent != RenderExtent || I1Vel->Desc.Format != PF_G16R16F)
		{
			I1Vel = MagicSRMakeExtent(
				GraphBuilder, ViewInfo, VelTight, RenderExtent, PF_G16R16F, TEXT("MagicFG.I1Velocity"));
		}
	}

	FRDGTextureRef FgOut = nullptr;
	const FIntPoint ColorExtent = LowResMode > 0 ? FgExtent : OutputExtent;
	const FIntPoint DepthExtent = LowResMode > 0 ? FgExtent : RenderExtent;
	const FIntPoint HistExtent = ColorExtent;
	const bool bHaveI0 = !bResetHistory && GMagicFgI0Color.IsValid() && GMagicFgI0Depth.IsValid() &&
						 GMagicFgI0Color->GetDesc().Extent == ColorExtent &&
						 GMagicFgI0Depth->GetDesc().Extent == DepthExtent;
	if (bHaveI0)
	{
		FRDGTextureRef I0Color =
			GraphBuilder.RegisterExternalTexture(GMagicFgI0Color, TEXT("MagicFG.I0Color"));
		FRDGTextureRef I0Depth =
			GraphBuilder.RegisterExternalTexture(GMagicFgI0Depth, TEXT("MagicFG.I0Depth"));

		FRDGTextureDesc OutDesc = FRDGTextureDesc::Create2D(
			HistExtent, PF_FloatRGBA, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
		FgOut = GraphBuilder.CreateTexture(OutDesc, TEXT("MagicFG.Output"));

		FMagicSRFgPassParameters* PassParameters = GraphBuilder.AllocParameters<FMagicSRFgPassParameters>();
		PassParameters->I0Color = I0Color;
		PassParameters->I0Depth = I0Depth;
		PassParameters->I1Color = I1Color;
		PassParameters->I1Depth = I1Depth;
		PassParameters->I1Velocity = I1Vel;
		PassParameters->Output = GraphBuilder.CreateUAV(FgOut);

		const bool bStill = FramesSinceCameraStill > 1;
		const bool bFast = Mode == 2;
		const int32 W = HistExtent.X;
		const int32 H = HistExtent.Y;
		FMagicFgVpFrame VpFrame;
		FMemory::Memzero(&VpFrame, sizeof(VpFrame));
		VpFrame.Near = View.NearClippingDistance > 0.f ? View.NearClippingDistance : 10.f;
		VpFrame.Far = 0.f;
		float CurVp[16];
		MagicSRFillUnjitteredVp(View, CurVp);
		if (FgPrevVpValid)
		{
			FMemory::Memcpy(VpFrame.Vp0, FgPrevVp, sizeof(VpFrame.Vp0));
			FMemory::Memcpy(VpFrame.Vp1, CurVp, sizeof(VpFrame.Vp1));
			VpFrame.Valid = 1;
		}
		FMemory::Memcpy(FgPrevVp, CurVp, sizeof(FgPrevVp));
		FgPrevVpValid = 1;
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("MagicFG %s %dx%d", bFast ? TEXT("Fast") : TEXT("CoreV3"), W, H),
			PassParameters,
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			[PassParameters, W, H, bStill, bFast, VpFrame](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FMagicSRViewportBridge::Get().ProcessFrameGen_RenderThread(
					RHICmdList,
					PassParameters->I0Color->GetRHI(),
					PassParameters->I0Depth->GetRHI(),
					PassParameters->I1Color->GetRHI(),
					PassParameters->I1Depth->GetRHI(),
					PassParameters->I1Velocity->GetRHI(),
					PassParameters->Output ? PassParameters->Output->GetParent()->GetRHI() : nullptr,
					W, H, bStill, bFast, VpFrame.Vp0, VpFrame.Vp1, VpFrame.Valid, VpFrame.Near,
					VpFrame.Far);
			});
	}
	else
	{
		LastFgStatus = 2; // HISTORY_NOT_READY
		float CurVp[16];
		MagicSRFillUnjitteredVp(View, CurVp);
		FMemory::Memcpy(FgPrevVp, CurVp, sizeof(FgPrevVp));
		FgPrevVpValid = bResetHistory ? 0 : 1;
	}

	FRDGTextureRef FgPresent = FgOut;
	if (LowResMode == 2 && FgOut)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "MagicFG.LowResUpsampleBilinear %dx%d", OutputExtent.X, OutputExtent.Y);
		FgPresent = MagicSRMakeExtent(
			GraphBuilder, ViewInfo, FgOut, OutputExtent, PF_FloatRGBA, TEXT("MagicFG.UpsampleBilinear"));
	}

	GraphBuilder.QueueTextureExtraction(I1Color, &GMagicFgI0Color);
	GraphBuilder.QueueTextureExtraction(I1Depth, &GMagicFgI0Depth);
	if (FgPresent && GetFrameGenPresent() && !GetFrameGenShowMid())
	{
		NoteGeneratedFrame_RenderThread(GraphBuilder, FgPresent, I1ColorIn);
	}
	else
	{
		NoteGeneratedFrame_RenderThread(GraphBuilder, nullptr, nullptr);
	}
	return FgPresent;
}
