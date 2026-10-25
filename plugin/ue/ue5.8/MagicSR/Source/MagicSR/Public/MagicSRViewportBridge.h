#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "HAL/CriticalSection.h"
#include "RenderGraphFwd.h"

class FSceneView;
class SWindow;
class ISlateViewportProvider;

/**
 * Render-thread MagicSR Temporal session for the player viewport path.
 * Owns Vulkan-imported mc_nscaler handle and processes one frame from RHI textures.
 */
class MAGICSR_API FMagicSRViewportBridge
{
public:
	static FMagicSRViewportBridge& Get();

	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	/** One-shot. The next viewport process sends reset_history. */
	void RequestHistoryReset();
	/** Render thread. True once after RequestHistoryReset. Also restarts the per-handle frame index. */
	bool ConsumeHistoryReset();

	/**
	 * Render thread. 0 when the camera moved this frame or history reset.
	 * Otherwise the number of frames since that event. Location epsilon 0.01 cm,
	 * rotation epsilon 0.001 deg (the yaw tour steps 0.025 deg).
	 */
	float NoteCameraStill_RenderThread(const FVector& Loc, const FRotator& Rot, bool bReset);

	/** Primary (pre-upscale) ViewRect, captured in PreRenderView. */
	void SetPrimaryRenderViewRect(const FIntRect& Rect)
	{
		PrimaryRenderViewRect = Rect;
		bHavePrimaryRenderViewRect = Rect.Width() > 0 && Rect.Height() > 0;
	}
	FIntRect GetPrimaryRenderViewRect() const { return PrimaryRenderViewRect; }
	bool HasPrimaryRenderViewRect() const { return bHavePrimaryRenderViewRect; }

	/** Last process result / jitter for HUD / logs (game or render thread; atomic-ish). */
	int32 GetLastProcessRc() const { return LastProcessRc; }
	FVector2f GetLastJitter() const { return LastJitter; }
	FIntPoint GetLastInputSize() const { return LastInputSize; }
	FIntPoint GetLastOutputSize() const { return LastOutputSize; }

	/**
	 * Ensure handle matches current I/O sizes. Safe to call on render thread.
	 * Returns false if create failed (latched until resolution change / disable).
	 */
	bool EnsureHandle_RenderThread(int32 InputW, int32 InputH, float Scale);

	/**
	 * Explicit all-zero mask (input resolution), used for transparency.
	 * Omitting transparency makes some paths derive a motion-color approximation.
	 * Cleared once per size; later frames reuse the extracted texture.
	 */
	FRDGTextureRef AcquireZeroMask_RenderThread(FRDGBuilder& GraphBuilder, const FIntPoint& Extent);

	/**
	 * Pooled R16F reactive target. The decode pass overwrites every pixel;
	 * this only allocates / reimports. Not cleared (the writer covers the rect).
	 */
	FRDGTextureRef AcquireReactiveMask_RenderThread(FRDGBuilder& GraphBuilder, const FIntPoint& Extent);
	/** Call after the pass that writes the mask. Extraction before that pass is not produced yet. */
	void QueueReactiveMaskExtraction_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef Tex);

	/**
	 * Run Temporal MagicSR into Output. All textures must be valid RHI resources.
	 * Jitter in input pixels (+Y down). Motion scale (0,0) → library (W,H) UV default.
	 * Reactive and Transparency must be explicit (zero is valid) so auto-mask stays off.
	 */
	int32 ProcessTemporal_RenderThread(
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
		float InFramesSinceStill);

	void DestroyHandle_RenderThread();

	/** 0=Off, 1=CoreV3, 2=Fast Analytic. Render or game thread. */
	static int32 GetFrameGenMode();
	static bool GetFrameGenShowMid();
	/** 0=ShowMid evaluation. 1=deprecated nested present. 2=plugin overlay presenter. */
	static int32 GetFrameGenPresentMode();
	/** true if GetFrameGenPresentMode() != 0 */
	static bool GetFrameGenPresent();
	void NotifyPresenterPresent(int32 Kind);

	float GetLastFgGpuMs() const { return LastFgGpuMs; }
	int32 GetLastFgStatus() const { return LastFgStatus; }
	int32 GetLastFgRc() const { return LastFgRc; }
	/** Requested r.MagicSR.FrameGenLowRes this frame (0/1/2). */
	int32 GetLastFgLowResWanted() const { return LastFgLowResWanted; }
	/** Actual FG working extent mode: 0=OutputExtent, 2=render-res + bilinear 2x. 1 never activates. */
	int32 GetLastFgLowResMode() const { return LastFgLowResMode; }
	FIntPoint GetLastFgExtent() const { return LastFgExtent; }

	struct FPresentSnapshot
	{
		int32 EngineFrames = 0;
		int32 PresentCount = 0;
		double WindowSeconds = 0.0;
		float RenderFps = 0.f;
		float PresentFps = 0.f;
		float FgGpuMs = 0.f;
		float IntervalMeanMs = 0.f;
		float IntervalStdMs = 0.f;
		float IntervalP95Ms = 0.f;
		float IntervalMaxMs = 0.f;
		float RealIntervalEmaMs = 0.f;
		float ExtraLatencyMs = 0.f;
		float GenPhaseMean = 0.f;
		float GenToRealMeanMs = 0.f;
		float GenToRealStdMs = 0.f;
		float RealToGenMeanMs = 0.f;
		float RealToGenStdMs = 0.f;
		uint32 DxgiLastPresentCount = 0;
		int32 DxgiPresentDelta = -1;
		int32 DxgiStatsHr = 1;
		uint32 DxgiStatsPresentCount = 0;
		float PresenterCopyMs = 0.f;
		float PresenterHoldMs = 0.f;
		float PresenterThreadCpuPct = 0.f;
		int32 PresenterCopySamples = 0;
		float TickToPresentMs = 0.f;
		int32 TickToPresentSamples = 0;
		float TickToGpuCompleteMs = 0.f;
		int32 TickToGpuCompleteSamples = 0;
		float WriteToGpuCompleteMs = 0.f;
		float TickToReadyMs = 0.f;
		int32 TickToReadySamples = 0;
		float ReadyToPresentMs = 0.f;
		int32 ReadyToPresentSamples = 0;
		float BlitWaitMs = 0.f;
		float D3dCopyMs = 0.f;
		float DxgiPresentMs = 0.f;
		int32 JobsDropped = 0;
		float QueueDepthMean = 0.f;
		float QueueDepthP95 = 0.f;
		float QueueDepthMax = 0.f;
		float GpuInflightMean = 0.f;
		float GpuInflightP95 = 0.f;
		float GpuInflightMax = 0.f;
		float PresenterBacklogMean = 0.f;
		float PresenterBacklogP95 = 0.f;
		float PresenterBacklogMax = 0.f;
	};

	void EnsurePresentHook();
	void ShutdownPresentHook();
	void HandleBackBufferReady(SWindow& Window, ISlateViewportProvider& ViewportProvider);
	void NoteGeneratedFrame_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef FgOut, FRDGTextureRef RealScene);
	void ReleasePresentResources_RenderThread();
	void ResetPresentCounters();
	FPresentSnapshot GetPresentSnapshot() const;
	/** Measurement only. Default off: extra present stats / DXGI / copy timers stay idle. */
	static void SetPerfCollect(bool bOn);
	static bool IsPerfCollect();
	/** Game-thread timestamp for tick→present. No-op unless IsPerfCollect. */
	static void NoteGameTickTime();
	/** Measurement only. Arm a 2-frame white flash on the next backbuffers. Default CVar off. */
	static void TriggerLatencyFlash();
	void NoteOffGpuComplete(double TickToGpuMsSum, double WriteToGpuMsSum, int32 Count);
	void BeginPerfWindow();
	void DumpPresenterTraceCsv(const TCHAR* Path) const;
	int32 GetPresenterTraceCount() const;
	void OnCustomPresent(FRHIViewport* Viewport);
	FRHITexture* GetSavedPresentRHI() const;
	FRHITexture* ReplaceSavedPresent(FRHITexture* NewTex);
	float GetHudRenderFps() const { return HudRenderFps; }
	float GetHudPresentFps() const { return HudPresentFps; }
	float GetHudJitterStdMs() const { return HudJitterStdMs; }
	float GetHudRealIntervalEmaMs() const { return HudRealIntervalEmaMs; }

	/**
	 * RDG Frame Generation between previous real frame (I0) and current (I1).
	 * Returns generated midpoint (t=0.5) or nullptr when history is not ready.
	 */
	FRDGTextureRef AddFrameGenPass_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		FRDGTextureRef I1Color,
		FRDGTextureRef SceneDepth,
		FRDGTextureRef EncodedOrDecodedVelocity,
		const FIntRect& DepthVelViewRect,
		const FIntPoint& OutputExtent,
		bool bVelocityAlreadyDecoded,
		bool bResetHistory);

	/**
	 * One-shot status read of the library's cumulative gpu_time seconds.
	 * Not called from the frame path. Returns false when the viewport handle
	 * is absent (native modes) or the query fails.
	 */
	bool QueryCumulativeGpuSeconds(double& OutSeconds);

	/**
	 * One-shot MV and reactive readback. No-op unless MAGICSR_PROBE_MV=1,
	 * which is read once. Does not run on the library hot path.
	 */
	static void RequestMvProbe(const TCHAR* Tag);

private:
	FMagicSRViewportBridge() = default;

	bool CreateHandle_RenderThread(int32 InputW, int32 InputH, float Scale);

	bool bEnabled = false;
	bool bCreateFailed = false;
	int32 PendingHistoryReset = 0;
	bool bHaveCamPose = false;
	uint32 LastCamPoseFrame = 0xffffffffu;
	FIntRect PrimaryRenderViewRect;
	bool bHavePrimaryRenderViewRect = false;
	FVector PrevCamLoc = FVector::ZeroVector;
	FRotator PrevCamRot = FRotator::ZeroRotator;
	int32 FramesSinceCameraStill = 0;
	void* Handle = nullptr;
	int32 HandleInputW = 0;
	int32 HandleInputH = 0;
	float HandleScale = 0.0f;
	int32 HandleAlgMode = -1;
	uint32 FrameCounter = 0;

	int32 LastProcessRc = 0;
	FVector2f LastJitter = FVector2f::ZeroVector;
	FIntPoint LastInputSize = FIntPoint::ZeroValue;
	FIntPoint LastOutputSize = FIntPoint::ZeroValue;

	void* FgHandle = nullptr;
	int32 FgMode = 0;
	int32 FgMaxW = 0;
	int32 FgMaxH = 0;
	int32 FgInputW = 0;
	int32 FgInputH = 0;
	bool bFgCreateFailed = false;
	int32 LastFgStatus = 0;
	int32 LastFgRc = 0;
	float LastFgGpuMs = 0.f;
	int32 LastFgLowResWanted = 0;
	int32 LastFgLowResMode = 0;
	FIntPoint LastFgExtent = FIntPoint::ZeroValue;
	float FgPrevVp[16] = {};
	uint32 FgPrevVpValid = 0;

	bool EnsureFgHandle_RenderThread(int32 Width, int32 Height, int32 Mode, bool bExactExtent);
	void DestroyFgHandle_RenderThread();
	int32 ProcessFrameGen_RenderThread(
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
		float CameraFar);

	FCriticalSection Mutex;

	FDelegateHandle PresentHookHandle;
	bool bPresentHookBound = false;
	TRefCountPtr<FRHITexture> FgSavedPresent;
	TRefCountPtr<FRHIViewport> CachedPresentViewport;
	int32 FgGeneratedReady = 0;
	int32 FgPresentKindWrite = 0;
	int32 FgPresentKindRead = 0;
	int32 FgPresentKinds[8] = {};
	int32 PresentEngineFrames = 0;
	int32 PresentCount = 0;
	int32 WindowEngineFrames = 0;
	int32 WindowPresentCount = 0;
	double PresentWindowStart = 0.0;
	double LastPresentTime = 0.0;
	double LastRealPresentTime = 0.0;
	double LastGenPresentTime = 0.0;
	double RealIntervalEma = 0.0;
	double FgGpuMsSum = 0.0;
	int32 FgGpuMsCount = 0;
	double GenPhaseSum = 0.0;
	int32 GenPhaseCount = 0;
	static constexpr int32 PresentIntervalCap = 512;
	double PresentIntervalsMs[PresentIntervalCap] = {};
	int32 PresentIntervalKinds[PresentIntervalCap] = {};
	int32 PresentIntervalCount = 0;
	int32 PresentIntervalWrite = 0;
	float HudRenderFps = 0.f;
	float HudPresentFps = 0.f;
	float HudJitterStdMs = 0.f;
	float HudRealIntervalEmaMs = 16.f;
	int32 HudPresentWindowCount = 0;
	double HudPresentWindowStart = 0.0;
	int32 HudEngineWindowCount = 0;
	double OffTickToPresentMsSum = 0.0;
	int32 OffTickToPresentSamples = 0;
	double OffTickToGpuMsSum = 0.0;
	double OffWriteToGpuMsSum = 0.0;
	int32 OffTickToGpuSamples = 0;

	void PushPresentKind(int32 Kind);
	int32 PopPresentKind();
	void NotePresentTime(int32 Kind);
};
