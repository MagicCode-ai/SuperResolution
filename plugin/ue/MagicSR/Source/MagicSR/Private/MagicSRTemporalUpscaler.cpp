#include "MagicSRTemporalUpscaler.h"

#include "MagicSRReactiveMarker.h"
#include "MagicSRShaders.h"
#include "MagicSRViewportBridge.h"

#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/PlatformMisc.h"
#include "Math/Float16.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"

namespace
{
static TAutoConsoleVariable<int32> CVarMagicSRJitterYSign(
	TEXT("r.MagicSR.JitterYSign"),
	1,
	TEXT("Multiply UE TemporalJitterPixels.Y. UE SampleY is already +Y-down (projection uses -2Y/H); 1 matches MagicSR."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMagicSRZeroMotion(
	TEXT("r.MagicSR.ZeroMotion"),
	0,
	TEXT("1 = replace velocity with zeros (diagnostic). 0 = decoded UE velocity."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMagicSRReactiveFromMv(
	TEXT("r.MagicSR.ReactiveFromMv"),
	1,
	TEXT("1 = reactive mask from decoded |MV| in the velocity pass (static≈0, motion→1). 0 = explicit zero reactive."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarMagicSRReactiveStartPx(
	TEXT("r.MagicSR.ReactiveStartPx"),
	0.05f,
	TEXT("Input-pixel |MV| where reactive leaves 0. Sits under a ~0.1px/frame slow yaw and above a still camera."),
	ECVF_RenderThreadSafe);

int32 MagicSRProbeEnv()
{
	static int32 Env = -1;
	if (Env < 0)
	{
		const FString Value = FPlatformMisc::GetEnvironmentVariable(TEXT("MAGICSR_PROBE_MV"));
		Env = (Value == TEXT("1")) ? 1 : 0;
	}
	return Env;
}

struct FMvProbeMeta
{
	float LibJitterX = 0.f;
	float LibJitterY = 0.f;
	float UeJitterX = 0.f;
	float UeJitterY = 0.f;
	float ProjJitterX = 0.f;
	float ProjJitterY = 0.f;
	int32 JitterIndex = 0;
	int32 JitterLength = 0;
	uint32 FrameNumber = 0;
	float FovY = 0.f;
	int32 ViewMinX = 0;
	int32 ViewMinY = 0;
	int32 ViewMaxX = 0;
	int32 ViewMaxY = 0;
};

struct FMvProbeSlot
{
	FRHIGPUTextureReadback* Motion = nullptr;
	FRHIGPUTextureReadback* Color = nullptr;
	int32 W = 0;
	int32 H = 0;
	FMvProbeMeta Meta;
};

struct FMvProbeState
{
	FCriticalSection Mutex;
	TCHAR Tag[64] = {};
	volatile int32 Requested = 0;
	volatile int32 Finished = 0;
	int32 StartedReq = 0;
	TCHAR StartedTag[64] = {};
	FMvProbeSlot Slots[2];
	int32 Enqueued = 0;
	uint32 LastFrame = 0xffffffffu;
};

FMvProbeState GMvProbe;

float MagicSRHalf(uint16 Encoded)
{
	FFloat16 Half;
	Half.Encoded = Encoded;
	return static_cast<float>(Half);
}

FString MagicSRProbeDir();

void MagicSRWriteProbeBuffer(const TCHAR* Tag, const TCHAR* Kind, int32 Channels, const void* Data,
							 int32 Width, int32 Height, int32 PitchPixels, float& OutMax, int32& OutNonZero)
{
	OutMax = 0.f;
	OutNonZero = 0;
	if (!Data || Width <= 0 || Height <= 0 || PitchPixels <= 0)
	{
		return;
	}
	const int32 BytesPerPixel = Channels * static_cast<int32>(sizeof(uint16));
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const uint16* Row = reinterpret_cast<const uint16*>(
			static_cast<const uint8*>(Data) + static_cast<int64>(Y) * PitchPixels * BytesPerPixel);
		for (int32 X = 0; X < Width; ++X)
		{
			float Acc = 0.f;
			for (int32 C = 0; C < Channels; ++C)
			{
				const uint16 Enc = Row[X * Channels + C];
				if (Enc != 0)
				{
					++OutNonZero;
				}
				const float V = MagicSRHalf(Enc);
				Acc += V * V;
			}
			OutMax = FMath::Max(OutMax, FMath::Sqrt(Acc));
		}
	}
	const FString Path = FPaths::Combine(MagicSRProbeDir(),
										 FString::Printf(TEXT("probe_%s_%s.bin"), Tag, Kind));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	TArray<uint8> Bytes;
	const int32 Header[4] = {Width, Height, PitchPixels, Channels};
	Bytes.Append(reinterpret_cast<const uint8*>(Header), sizeof(Header));
	const int32 RowBytes = PitchPixels * BytesPerPixel;
	Bytes.Append(static_cast<const uint8*>(Data), RowBytes * Height);
	FFileHelper::SaveArrayToFile(Bytes, *Path);
}

FString MagicSRProbeDir()
{
	const FString Temp = FPlatformMisc::GetEnvironmentVariable(TEXT("TEMP"));
	const FString Root = Temp.IsEmpty() ? FString(TEXT("D:\\magicsr_beat_tsr\\tmp")) : Temp;
	return FPaths::Combine(Root, TEXT("mvprobe"));
}

void MagicSRWriteProbeSlot(int32 SlotIndex)
{
	FMvProbeSlot& Slot = GMvProbe.Slots[SlotIndex];
	if (!Slot.Motion || !Slot.Color || Slot.W <= 0 || Slot.H <= 0)
	{
		return;
	}
	int32 MotionPitch = 0;
	int32 MotionH = 0;
	void* MotionData = Slot.Motion->Lock(MotionPitch, &MotionH);
	float MotionMax = 0.f;
	int32 MotionNz = 0;
	const FString KindM = FString::Printf(TEXT("f%d_motion"), SlotIndex);
	MagicSRWriteProbeBuffer(GMvProbe.StartedTag, *KindM, 2, MotionData, Slot.W, Slot.H, MotionPitch,
							MotionMax, MotionNz);
	float CenterMx = 0.f;
	float CenterMy = 0.f;
	if (MotionData && MotionPitch > 0)
	{
		const int32 Cx = Slot.W / 2;
		const int32 Cy = Slot.H / 2;
		const uint16* Row = reinterpret_cast<const uint16*>(
			static_cast<const uint8*>(MotionData) + static_cast<int64>(Cy) * MotionPitch * 4);
		CenterMx = MagicSRHalf(Row[Cx * 2]);
		CenterMy = MagicSRHalf(Row[Cx * 2 + 1]);
	}
	Slot.Motion->Unlock();

	int32 ColorPitch = 0;
	int32 ColorH = 0;
	void* ColorData = Slot.Color->Lock(ColorPitch, &ColorH);
	float ColorMax = 0.f;
	int32 ColorNz = 0;
	const FString KindC = FString::Printf(TEXT("f%d_color"), SlotIndex);
	MagicSRWriteProbeBuffer(GMvProbe.StartedTag, *KindC, 4, ColorData, Slot.W, Slot.H, ColorPitch,
							ColorMax, ColorNz);
	Slot.Color->Unlock();

	const FMvProbeMeta& M = Slot.Meta;
	const float MotionPx = MotionMax * FMath::Max(static_cast<float>(Slot.W), static_cast<float>(Slot.H));
	const FString Line = FString::Printf(
		TEXT("slot=%d frame=%u wh=%dx%d view=%d,%d-%d,%d lib_jitter=(%.5f,%.5f) ue_jitter=(%.5f,%.5f) proj_jitter=(%.6f,%.6f) jidx=%d jlen=%d fovy=%.6f motion_max_uv=%.8g motion_max_px=%.8g center_uv=(%.8g,%.8g) motion_nonzero=%d"),
		SlotIndex, M.FrameNumber, Slot.W, Slot.H, M.ViewMinX, M.ViewMinY, M.ViewMaxX, M.ViewMaxY,
		M.LibJitterX, M.LibJitterY, M.UeJitterX, M.UeJitterY, M.ProjJitterX, M.ProjJitterY, M.JitterIndex,
		M.JitterLength, M.FovY, MotionMax, MotionPx, CenterMx, CenterMy, MotionNz);
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] PROBE %s"), *Line);
	const FString Path = FPaths::Combine(MagicSRProbeDir(),
										 FString::Printf(TEXT("probe_%s_meta.txt"), GMvProbe.StartedTag));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	FFileHelper::SaveStringToFile(Line + TEXT("\n"), *Path, FFileHelper::EEncodingOptions::AutoDetect,
								  &IFileManager::Get(), FILEWRITE_Append);
}

void MagicSRFinishProbe()
{
	if (GMvProbe.Enqueued < 2)
	{
		return;
	}
	for (int32 I = 0; I < 2; ++I)
	{
		FMvProbeSlot& Slot = GMvProbe.Slots[I];
		if (!Slot.Motion || !Slot.Color || !Slot.Motion->IsReady() || !Slot.Color->IsReady())
		{
			return;
		}
	}
	for (int32 I = 0; I < 2; ++I)
	{
		MagicSRWriteProbeSlot(I);
		delete GMvProbe.Slots[I].Motion;
		delete GMvProbe.Slots[I].Color;
		GMvProbe.Slots[I].Motion = nullptr;
		GMvProbe.Slots[I].Color = nullptr;
	}
	GMvProbe.Enqueued = 0;
	FPlatformAtomics::InterlockedExchange(&GMvProbe.Finished, GMvProbe.StartedReq);
}

void MagicSR_TickMvProbe(FRDGBuilder& GraphBuilder, FRDGTextureRef Motion, FRDGTextureRef Color, int32 W,
						 int32 H, const FMvProbeMeta& Meta)
{
	if (MagicSRProbeEnv() != 1 || !Motion || !Color)
	{
		return;
	}
	MagicSRFinishProbe();
	const int32 Req = FPlatformAtomics::AtomicRead(&GMvProbe.Requested);
	if (Req == GMvProbe.Finished || GMvProbe.Enqueued >= 2)
	{
		return;
	}
	if (W != 960 || H != 540 || Meta.FrameNumber == GMvProbe.LastFrame)
	{
		return;
	}
	FScopeLock Lock(&GMvProbe.Mutex);
	if (GMvProbe.Enqueued == 0)
	{
		FCString::Strncpy(GMvProbe.StartedTag, GMvProbe.Tag, 63);
		GMvProbe.StartedReq = Req;
	}
	FMvProbeSlot& Slot = GMvProbe.Slots[GMvProbe.Enqueued];
	Slot.W = W;
	Slot.H = H;
	Slot.Meta = Meta;
	Slot.Motion = new FRHIGPUTextureReadback(TEXT("MagicSRProbeMV"));
	Slot.Color = new FRHIGPUTextureReadback(TEXT("MagicSRProbeColor"));
	AddEnqueueCopyPass(GraphBuilder, Slot.Motion, Motion);
	AddEnqueueCopyPass(GraphBuilder, Slot.Color, Color);
	GMvProbe.LastFrame = Meta.FrameNumber;
	++GMvProbe.Enqueued;
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] PROBE_COPY slot=%d frame=%u jitter=(%.4f,%.4f) jidx=%d"),
		   GMvProbe.Enqueued - 1, Meta.FrameNumber, Meta.LibJitterX, Meta.LibJitterY, Meta.JitterIndex);
}

void MagicSRArmProbe(const TCHAR* Tag)
{
	if (MagicSRProbeEnv() != 1 || Tag == nullptr)
	{
		return;
	}
	FScopeLock Lock(&GMvProbe.Mutex);
	if (GMvProbe.Requested != GMvProbe.Finished)
	{
		UE_LOG(LogTemp, Display, TEXT("[MagicSR] PROBE_BUSY %s"), Tag);
		return;
	}
	FCString::Strncpy(GMvProbe.Tag, Tag, UE_ARRAY_COUNT(GMvProbe.Tag) - 1);
	FPlatformAtomics::InterlockedIncrement(&GMvProbe.Requested);
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] PROBE_ARM %s"), Tag);
}

static TAutoConsoleVariable<float> CVarMagicSRReactiveSatPx(
	TEXT("r.MagicSR.ReactiveSatPx"),
	0.40f,
	TEXT("Input-pixel |MV| where reactive reaches 1. Slow yaw stays on the lower ramp."),
	ECVF_RenderThreadSafe);

BEGIN_SHADER_PARAMETER_STRUCT(FMagicSRTemporalPassParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneVelocity)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Reactive)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Transparency)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, Output)
END_SHADER_PARAMETER_STRUCT()
} // namespace

void FMagicSRViewportBridge::RequestMvProbe(const TCHAR* Tag)
{
	MagicSRArmProbe(Tag);
}

UE::Renderer::Private::ITemporalUpscaler::FOutputs FMagicSRTemporalUpscaler::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FInputs& Inputs) const
{
	FOutputs Outputs;
	Outputs.NewHistory = MakeRefCount<FHistory>();

	if (View.bIsSceneCapture || View.bIsReflectionCapture || View.bIsPlanarReflection ||
		View.bIsSceneCaptureCube)
	{
		Outputs.FullRes = Inputs.SceneColor;
		return Outputs;
	}

	const FIntRect InputRect = Inputs.SceneColor.ViewRect;
	const FIntRect OutputRect = Inputs.OutputViewRect;
	if (!Inputs.SceneColor.IsValid() || !Inputs.SceneDepth.IsValid() || !Inputs.SceneVelocity.IsValid() ||
		InputRect.Width() <= 0 || InputRect.Height() <= 0 || OutputRect.Width() <= 0 ||
		OutputRect.Height() <= 0)
	{
		Outputs.FullRes = Inputs.SceneColor;
		return Outputs;
	}

	const int32 InW = InputRect.Width();
	const int32 InH = InputRect.Height();
	const int32 OutW = OutputRect.Width();
	const int32 OutH = OutputRect.Height();
	// Match UE secondary size exactly (not forced 2× — avoids Out size contract mismatch).
	const float Scale = static_cast<float>(OutW) / static_cast<float>(InW);

	const EPixelFormat SceneFormat = Inputs.SceneColor.Texture->Desc.Format;
	const FIntPoint TightIn(InW, InH);
	const FIntPoint TightOut(OutW, OutH);
	// Final texture must cover OutputViewRect.Max (Min is 0 for third-party upscaler).
	const FIntPoint FinalExtent(FMath::Max(OutputRect.Max.X, 1), FMath::Max(OutputRect.Max.Y, 1));

	auto MakeFallback = [&]() -> FScreenPassTexture
	{
		FRDGTextureDesc FallbackDesc = FRDGTextureDesc::Create2D(
			FinalExtent,
			SceneFormat,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		FRDGTextureRef Fallback = GraphBuilder.CreateTexture(FallbackDesc, TEXT("MagicSR.Fallback"));
		AddDrawTexturePass(GraphBuilder, View, Inputs.SceneColor.Texture, Fallback, InputRect.Min,
						   InputRect.Size(), OutputRect.Min, OutputRect.Size());
		return FScreenPassTexture(Fallback, OutputRect);
	};

	FMagicSRViewportBridge& Bridge = FMagicSRViewportBridge::Get();
	if (!Bridge.IsEnabled() || !Bridge.EnsureHandle_RenderThread(InW, InH, Scale))
	{
		Outputs.FullRes = MakeFallback();
		return Outputs;
	}

	// Library Vulkan temporal requires R16G16B16A16_SFLOAT output; tight OutW×OutH at origin.
	FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
		TightOut,
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("MagicSR.TemporalOutput"));

	// Tight-pack color / depth / MV to (0,0)-(InW,InH) — library reads top-left WxH.
	// Explicit zero transparency stays pooled. Reactive is either that same zero
	// or an MRT written by DecodeVelocity from |MV| (no second fullscreen pass).
	FRDGTextureRef ZeroMask = Bridge.AcquireZeroMask_RenderThread(GraphBuilder, TightIn);
	const bool bReactiveFromMv = CVarMagicSRReactiveFromMv.GetValueOnRenderThread() != 0 &&
								 CVarMagicSRZeroMotion.GetValueOnRenderThread() == 0;
	FRDGTextureRef ReactiveMask = bReactiveFromMv
		? Bridge.AcquireReactiveMask_RenderThread(GraphBuilder, TightIn)
		: ZeroMask;
	const float ReactiveStartPx = CVarMagicSRReactiveStartPx.GetValueOnRenderThread();
	const float ReactiveSatPx = CVarMagicSRReactiveSatPx.GetValueOnRenderThread();

	FRDGTextureRef MotionTexture;
	if (CVarMagicSRZeroMotion.GetValueOnRenderThread() != 0)
	{
		FRDGTextureDesc ZeroMvDesc = FRDGTextureDesc::Create2D(
			TightIn, PF_G16R16F, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
		MotionTexture = GraphBuilder.CreateTexture(ZeroMvDesc, TEXT("MagicSR.ZeroMotion"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(MotionTexture), FVector4(0, 0, 0, 0));
	}
	else
	{
		// Optional CustomDepth-stencil marking (TV / video wall). Merged into the reactive MRT:
		// no extra fullscreen pass; the stencil is only fetched when a stencil texture exists.
		FMagicSRDecodeVelocityExtras VelExtras;
		if (bReactiveFromMv)
		{
			uint32 StencilValue = 0;
			float StencilStrength = 0.f;
			if (MagicSRReactive::GetRenderThreadParams(StencilValue, StencilStrength))
			{
				const FSceneTextures* SceneTextures = static_cast<const FViewInfo&>(View).GetSceneTexturesChecked();
				if (SceneTextures && SceneTextures->CustomDepth.IsValid() && SceneTextures->CustomDepth.Stencil)
				{
					VelExtras.CustomStencil = SceneTextures->CustomDepth.Stencil;
					VelExtras.StencilValue = StencilValue;
					VelExtras.StencilStrength = StencilStrength;
				}
			}
		}
		MotionTexture = AddMagicSRDecodeVelocityPass(
			GraphBuilder, View, Inputs.SceneVelocity.Texture, Inputs.SceneDepth.Texture, InputRect,
			TightIn, bReactiveFromMv ? ReactiveMask : nullptr, ReactiveStartPx, ReactiveSatPx, VelExtras);
		if (bReactiveFromMv)
		{
			Bridge.QueueReactiveMaskExtraction_RenderThread(GraphBuilder, ReactiveMask);
		}
	}
	FRDGTextureRef DepthTexture =
		AddMagicSRCopyDepthPass(GraphBuilder, View, Inputs.SceneDepth.Texture, InputRect, TightIn);

	FRDGTextureDesc ColorDesc = FRDGTextureDesc::Create2D(
		TightIn,
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable);
	FRDGTextureRef ColorTexture = GraphBuilder.CreateTexture(ColorDesc, TEXT("MagicSR.ColorTightRGBA16F"));
	AddDrawTexturePass(GraphBuilder, View, Inputs.SceneColor.Texture, ColorTexture, InputRect.Min,
					   FIntPoint::ZeroValue, InputRect.Size());

	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	const FVector2f UeJitter(static_cast<float>(ViewInfo.TemporalJitterPixels.X),
							 static_cast<float>(ViewInfo.TemporalJitterPixels.Y));
	FVector2f Jitter = UeJitter;
	Jitter.Y *= static_cast<float>(CVarMagicSRJitterYSign.GetValueOnRenderThread());
	const FMatrix ClipToView = View.ViewMatrices.GetClipToView();
	const FVector Corner = ClipToView.TransformFVector4(FVector4(1.0f, 1.0f, 0.0f, 1.0f));
	const float CameraFovY =
		2.0f * FMath::Atan(FMath::Abs(Corner.Y / FMath::Max(FMath::Abs(Corner.Z), 0.0001f)));
	const FVector2D ProjJitter = View.ViewMatrices.GetTemporalAAJitter();
	FMvProbeMeta ProbeMeta;
	ProbeMeta.LibJitterX = Jitter.X;
	ProbeMeta.LibJitterY = Jitter.Y;
	ProbeMeta.UeJitterX = UeJitter.X;
	ProbeMeta.UeJitterY = UeJitter.Y;
	ProbeMeta.ProjJitterX = static_cast<float>(ProjJitter.X);
	ProbeMeta.ProjJitterY = static_cast<float>(ProjJitter.Y);
	ProbeMeta.JitterIndex = ViewInfo.TemporalJitterIndex;
	ProbeMeta.JitterLength = ViewInfo.TemporalJitterSequenceLength;
	ProbeMeta.FrameNumber = View.Family ? View.Family->FrameNumber : 0u;
	ProbeMeta.FovY = CameraFovY;
	ProbeMeta.ViewMinX = ViewInfo.ViewRect.Min.X;
	ProbeMeta.ViewMinY = ViewInfo.ViewRect.Min.Y;
	ProbeMeta.ViewMaxX = ViewInfo.ViewRect.Max.X;
	ProbeMeta.ViewMaxY = ViewInfo.ViewRect.Max.Y;
	MagicSR_TickMvProbe(GraphBuilder, MotionTexture, ColorTexture, InW, InH, ProbeMeta);

	// Transparency stays the explicit zero. Reactive is zero or the MV mask above.
	// Omitting either makes the library derive a photo residual and reject history.
	FMagicSRTemporalPassParameters* PassParameters =
		GraphBuilder.AllocParameters<FMagicSRTemporalPassParameters>();
	PassParameters->SceneColor = ColorTexture;
	PassParameters->SceneDepth = DepthTexture;
	PassParameters->SceneVelocity = MotionTexture;
	PassParameters->Reactive = ReactiveMask;
	PassParameters->Transparency = ZeroMask;
	PassParameters->Output = GraphBuilder.CreateUAV(OutputTexture);

	// Freeze pre-exposure: eye-adaptation deltas kill history when static.
	const float PreExposure = 1.0f;
	const float CameraNear = View.NearClippingDistance > 0.0f ? View.NearClippingDistance : 10.0f;
	const float CameraFar = 0.0f;
	const bool bResetHistory = View.bCameraCut;
	const FVector CamLoc = View.ViewLocation;
	const FRotator CamRot = View.ViewRotation;
	const bool bTrackCam = View.bIsGameView && !View.bIsSceneCapture;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MagicSR Temporal %dx%d -> %dx%d", InW, InH, OutW, OutH),
		PassParameters,
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		[PassParameters, Jitter, PreExposure, CameraNear, CameraFar, CameraFovY, bResetHistory, InW,
		 InH, CamLoc, CamRot, bTrackCam](FRDGAsyncTask, FRHICommandList& RHICmdList)
		{
			FRHITexture* ColorRHI = PassParameters->SceneColor->GetRHI();
			FRHITexture* DepthRHI = PassParameters->SceneDepth->GetRHI();
			FRHITexture* MotionRHI = PassParameters->SceneVelocity->GetRHI();
			FRHITexture* ReactiveRHI = PassParameters->Reactive->GetRHI();
			FRHITexture* TransparencyRHI = PassParameters->Transparency->GetRHI();
			FRHITexture* OutputRHI =
				PassParameters->Output ? PassParameters->Output->GetParent()->GetRHI() : nullptr;

			const bool bCut =
				bResetHistory || FMagicSRViewportBridge::Get().ConsumeHistoryReset();
			const float StillFrames = bTrackCam
				? FMagicSRViewportBridge::Get().NoteCameraStill_RenderThread(CamLoc, CamRot, bCut)
				: 0.0f;
			FMagicSRViewportBridge::Get().ProcessTemporal_RenderThread(
				RHICmdList, ColorRHI, DepthRHI, MotionRHI, OutputRHI, ReactiveRHI, TransparencyRHI,
				InW, InH, Jitter, PreExposure, CameraNear, CameraFar, CameraFovY, bCut,
				/*FrameIndex=*/0, StillFrames);
		});

	FRDGTextureRef DisplayColor = OutputTexture;

	// Blit tight FloatRGBA → SceneColor format covering OutputViewRect.
	if (SceneFormat != PF_FloatRGBA || FinalExtent != TightOut || OutputRect.Min != FIntPoint::ZeroValue ||
		DisplayColor != OutputTexture)
	{
		FRDGTextureDesc FinalDesc = FRDGTextureDesc::Create2D(
			FinalExtent,
			SceneFormat,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		FRDGTextureRef FinalTexture =
			GraphBuilder.CreateTexture(FinalDesc, TEXT("MagicSR.TemporalOutputSceneFmt"));
		AddDrawTexturePass(GraphBuilder, View, DisplayColor, FinalTexture, FIntPoint::ZeroValue,
						   TightOut, OutputRect.Min, OutputRect.Size());
		Outputs.FullRes = FScreenPassTexture(FinalTexture, OutputRect);
	}
	else
	{
		Outputs.FullRes = FScreenPassTexture(DisplayColor, OutputRect);
	}
	return Outputs;
}

UE::Renderer::Private::ITemporalUpscaler* FMagicSRTemporalUpscaler::Fork_GameThread(
	const FSceneViewFamily& ViewFamily) const
{
	return new FMagicSRTemporalUpscaler();
}
