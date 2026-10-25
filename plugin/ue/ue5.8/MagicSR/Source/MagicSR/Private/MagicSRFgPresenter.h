#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "HAL/Runnable.h"
#include "Containers/Ticker.h"

class FRHICommandListImmediate;
class FRHITexture;

/**
 * Plugin-owned overlay presenter + present thread (FrameGenPresent=2).
 * Arch P: same VkDevice as UE, exclusive graphics-family queue index 1 for blit,
 * D3D11 DXGI_SWAP_EFFECT_DISCARD on the overlay HWND so DWM/GDI see every frame.
 */
class FMagicSRFgPresenter final : public FRunnable
{
public:
	static FMagicSRFgPresenter& Get();

	void EnsureTicker();
	void StopAndRelease();
	void RemoveTicker();

	void PushFrame_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		FRHITexture* Src,
		int32 Kind,
		bool bSkipPace);

	/** Claim a free ring slot (state=filling). Caller records GPU work, then FinishSlot. */
	int32 AcquireSlot_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* SizeSrc);
	FRHITexture* GetSlotRHI(int32 SlotIndex) const;
	void FinishSlot_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		int32 SlotIndex,
		int32 Kind,
		bool bSkipPace);

	void ReleaseRing_RenderThread();

	virtual uint32 Run() override;
	virtual void Stop() override;

	/** Measurement only. Default off: present thread skips extra timers / DXGI queries. */
	void BeginPerfWindow();
	struct FPresenterPerfSnap
	{
		uint32 DxgiLastPresentCount = 0;
		int32 DxgiPresentDelta = -1;
		int32 DxgiStatsHr = 1;
		uint32 DxgiStatsPresentCount = 0;
		float CopySubmitMsMean = 0.f;
		float RealHoldMsMean = 0.f;
		float ThreadCpuPct = 0.f;
		int32 CopySamples = 0;
		int32 HoldSamples = 0;
		float TickToPresentMsMean = 0.f;
		int32 TickToPresentSamples = 0;
		float TickToReadyMsMean = 0.f;
		int32 TickToReadySamples = 0;
		float ReadyToPresentMsMean = 0.f;
		int32 ReadyToPresentSamples = 0;
		float BlitWaitMsMean = 0.f;
		float D3dCopyMsMean = 0.f;
		float DxgiPresentMsMean = 0.f;
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
	FPresenterPerfSnap GetPerfSnap() const;

	/** Game-thread frame start. Measurement only (IsPerfCollect). */
	static void NoteGameTickTime();
	static double GetGameTickTimeForRenderFrame();

	/** Measurement only. Dump the RAM timeline ring to CSV. No-op if tracing was off. */
	void DumpTraceCsv(const TCHAR* Path) const;
	int32 GetTraceCount() const;

private:
	bool TickGameThread(float DeltaTime);
	void EnsureOverlayWindow();
	void SyncOverlayWindow();
	void HideAndDestroyWindow();
	bool EnsureVulkan(void* Hwnd);
	void DestroyVulkan();
	bool RecreateSwapchain(void* Hwnd, int32 Width, int32 Height);
	void DestroySwapchain();
	bool PresentSlot(int32 SlotIndex);
	void PresentColor(float R, float G, float B, int32 Width, int32 Height);
	bool AcquireAndBlit(
		uint64 SrcImage,
		int32 SrcW,
		int32 SrcH,
		bool bSolidColor,
		float Cr,
		float Cg,
		float Cb);
	void SubmitOnQueue(TFunction<void(void*)>&& Fn);
	int32 AllocSlot_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* Src);
	void FreeSlot(int32 SlotIndex);
	void EnqueueJob(int32 Slot, int32 Kind, bool bSkipPace);
	void NoteEnginePushInterval();
	int32 PeekPendingSlot_NoLock(int32& OutKind) const;
	int32 JobCount_NoLock() const;
	void CountQueueSplit_NoLock(int32& OutInflight, int32& OutBacklog) const;
	void DropOldestGens_NoLock(int32 MaxKeep);
	void NoteQueueDepth_NoLock();
	void NoteQueueSplit(int32 Inflight, int32 Backlog);
	void NoteRealReadyInterval(double ReadyTime);
	double EstimateDtSec() const;
	void PreciseWaitUntil(double Target) const;
	void PreciseWaitUntil(double Target, double MaxWaitSec) const;

	FTSTicker::FDelegateHandle TickerHandle;
	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;
	FCriticalSection JobCs;
	FCriticalSection VkCs;

	volatile int32 bWantRun = 0;
	volatile int32 bThreadRun = 0;
	volatile int32 bAcceptFrames = 0;
	volatile int32 bHasPresented = 0;
	volatile int32 RecreateFlag = 0;
	volatile int32 DesiredW = 0;
	volatile int32 DesiredH = 0;
	volatile int32 OverlayW = 0;
	volatile int32 OverlayH = 0;

	void* OwnerHwnd = nullptr;
	void* OverlayHwnd = nullptr;
	void* WaitableTimer = nullptr;

	// 8 slots: queued jobs + in-flight + 2 RT fills. 6 exhausted every other gen
	// (P/R ≈ 1.5, jobs_dropped=0). Job cap 4 then dropped those gens (~1/frame);
	// 8 leaves 7 FIFO slots so gen+real pairs are not shed under pacing.
	static constexpr int32 kSlotCount = 8;
	static constexpr int32 kJobCap = 8;

	struct FSlot
	{
		TRefCountPtr<FRHITexture> Tex;
		FGPUFenceRHIRef Fence;
		uint64 VkImage = 0;
		int32 Width = 0;
		int32 Height = 0;
		int32 Kind = 0;
		int32 Seq = 0;
		int32 SkipPace = 0;
		double PushTime = 0.0;
		double GameTickTime = 0.0;
		volatile int32 State = 0;
	};
	FSlot Slots[kSlotCount];

	struct FJob
	{
		int32 Slot = -1;
		int32 Kind = 0;
		int32 SkipPace = 0;
	};
	FJob Jobs[kJobCap];
	int32 JobRead = 0;
	int32 JobWrite = 0;
	int32 PendingGenSlot = -1;
	int32 PendingRealSlot = -1;
	int32 InFlightSlot = -1;
	int32 NextSeq = 1;

	double LastRealPresentTime = 0.0;
	double LastGenPresentTime = 0.0;
	double LastRealTarget = 0.0;
	volatile double EngineIntervalEma = 0.0;
	double LastEnginePushTime = 0.0;
	int32 DebugPresents = 0;

	static constexpr int32 kDtHist = 16;
	double RealReadyDt[kDtHist] = {};
	int32 RealReadyN = 0;
	int32 RealReadyW = 0;
	double LastRealReadyTime = 0.0;

	static constexpr int32 kTraceCap = 2048;
	struct FTraceRec
	{
		uint32 Seq = 0;
		int32 Kind = 0;
		int32 QueueDepth = 0;
		int32 GpuInflight = 0;
		int32 PresenterBacklog = 0;
		int32 Resync = 0;
		int32 PaceMode = 0;
		int32 DroppedSoFar = 0;
		float DtEstMs = 0.f;
		float BehindMs = 0.f;
		float WaitMs = 0.f;
		double EnqueueSec = 0.0;
		double ReadySec = 0.0;
		double TargetSec = 0.0;
		double PresentSec = 0.0;
		double GameTickSec = 0.0;
		double LastRealPresentSec = 0.0;
	};
	FTraceRec TraceRing[kTraceCap];
	int32 TraceWrite = 0;
	int32 TraceCount = 0;
	mutable FCriticalSection TraceCs;

	static constexpr int32 kQHist = 512;
	uint8 QueueDepthHist[kQHist] = {};
	int32 QueueDepthHistN = 0;
	int32 QueueDepthHistW = 0;
	int32 QueueDepthMaxI = 0;
	uint8 GpuInflightHist[kQHist] = {};
	int32 GpuInflightHistN = 0;
	int32 GpuInflightHistW = 0;
	int32 GpuInflightMaxI = 0;
	uint8 PresenterBacklogHist[kQHist] = {};
	int32 PresenterBacklogHistN = 0;
	int32 PresenterBacklogHistW = 0;
	int32 PresenterBacklogMaxI = 0;

	mutable FCriticalSection PerfCs;
	uint32 PerfDxgiCount0 = 0;
	uint32 PerfDxgiCountNow = 0;
	int32 PerfDxgiStatsHr = 1;
	uint32 PerfDxgiStatsPresentCount = 0;
	double PerfCopyMsSum = 0.0;
	int32 PerfCopySamples = 0;
	double PerfHoldMsSum = 0.0;
	int32 PerfHoldSamples = 0;
	double PerfTickToPresentMsSum = 0.0;
	int32 PerfTickToPresentSamples = 0;
	double PerfTickToReadyMsSum = 0.0;
	int32 PerfTickToReadySamples = 0;
	double PerfReadyToPresentMsSum = 0.0;
	int32 PerfReadyToPresentSamples = 0;
	double PerfGpuInflightSum = 0.0;
	int32 PerfGpuInflightSamples = 0;
	double PerfPresenterBacklogSum = 0.0;
	int32 PerfPresenterBacklogSamples = 0;
	double PerfBlitWaitMsSum = 0.0;
	double PerfD3dCopyMsSum = 0.0;
	double PerfDxgiPresentMsSum = 0.0;
	int32 PerfJobsDropped = 0;
	double PerfQueueDepthSum = 0.0;
	int32 PerfQueueDepthSamples = 0;
	double PerfThreadCpu0Sec = 0.0;
	double PerfThreadWall0 = 0.0;
	void* PerfThreadHandle = nullptr;

	bool bVulkanReady = false;
	bool bExclusiveQueue = false;
	uint32 QueueFamily = 0;
	uint32 QueueIndex = 0;
	int32 SwapW = 0;
	int32 SwapH = 0;
	int32 SwapImageCount = 0;
	int32 SwapPresentMode = 0;

	void* VkInstanceHandle = nullptr;
	void* VkPhysHandle = nullptr;
	void* VkDeviceHandle = nullptr;
	void* VkQueueHandle = nullptr;
	void* VkSurfaceHandle = nullptr;
	void* VkSwapchainHandle = nullptr;
	void* VkCommandPoolHandle = nullptr;
	void* Api = nullptr;
};
