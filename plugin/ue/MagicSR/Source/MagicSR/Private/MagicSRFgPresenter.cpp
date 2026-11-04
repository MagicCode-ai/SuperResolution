#include "MagicSRFgPresenter.h"

#include "MagicSRViewportBridge.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/Event.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderCore.h"
#include "RenderingThread.h"
#include "Widgets/SWindow.h"

#if PLATFORM_WINDOWS
#include "IVulkanDynamicRHI.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan_win32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

namespace
{
TAutoConsoleVariable<int32> CVarMagicSRFgPresenterDebug(
	TEXT("r.MagicSR.FgPresenterDebug"),
	0,
	TEXT("Presenter debug. 0=production. 1=alternating test pattern (gate 1). 2=real frames only (gate 2)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterQueueIndex(
	TEXT("r.MagicSR.FgPresenterQueueIndex"),
	1,
	TEXT("Graphics-family queue index for presenter submit/present. 1=exclusive unused UE queue (Arch P). 0=RHIRunOnQueue on UE graphics queue."),
	ECVF_ReadOnly);

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterSkipPace(
	TEXT("r.MagicSR.FgPresenterSkipPace"),
	0,
	TEXT("Measurement only. 1=skip present-thread pacing wait. Default 0."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterLatestWins(
	TEXT("r.MagicSR.FgPresenterLatestWins"),
	0,
	TEXT("1=drop queued stale same-kind jobs (experimental). 0=FIFO, cap 8 (default)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterPace(
	TEXT("r.MagicSR.FgPresenterPace"),
	1,
	TEXT("0=legacy LastRealTarget FIFO. 1=controller: re-align to frame ready time, resync if late, drop old gens above soft cap. Default 1."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterQueueSoftCap(
	TEXT("r.MagicSR.FgPresenterQueueSoftCap"),
	6,
	TEXT("Controller: drop oldest gens only above this overflow cap (default 6). Drain backlog by skipping waits when depth>DrainDepth, not by shedding gens. Hard cap remains 8."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterDrainDepth(
	TEXT("r.MagicSR.FgPresenterDrainDepth"),
	3,
	TEXT("Controller: skip pacing wait when mixed queue depth at ready is above this (default 3). Mixed depth includes GPU inflight; presenter backlog of fence-ready jobs is typically ~1. Skip overflow so gen/real can still sit on 0.5*dt."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterTrace(
	TEXT("r.MagicSR.FgPresenterTrace"),
	0,
	TEXT("Measurement only. 1=record present timeline to a RAM ring (no file I/O on the present thread). Default 0."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMagicSRFgPresenterAblation(
	TEXT("r.MagicSR.FgPresenterAblation"),
	0,
	TEXT("Measurement only. 0=full. 1=skip DXGI Present. 2=skip D3D11 CopyResource+Present. 3=skip vk blit (present last). 4=real enqueue only (handled at push)."),
	ECVF_Default);

bool PresenterTraceOn()
{
	return CVarMagicSRFgPresenterTrace.GetValueOnAnyThread() != 0;
}
}

#if PLATFORM_WINDOWS

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
struct FMagicVkApi
{
	PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
	PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
	PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
	PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
	PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
	PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
	PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
	PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
	PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
	PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
	PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
	PFN_vkCreateCommandPool CreateCommandPool = nullptr;
	PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
	PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
	PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
	PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
	PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
	PFN_vkCmdBlitImage CmdBlitImage = nullptr;
	PFN_vkCmdCopyImage CmdCopyImage = nullptr;
	PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
	PFN_vkCreateFence CreateFence = nullptr;
	PFN_vkDestroyFence DestroyFence = nullptr;
	PFN_vkWaitForFences WaitForFences = nullptr;
	PFN_vkResetFences ResetFences = nullptr;
	PFN_vkCreateSemaphore CreateSemaphore = nullptr;
	PFN_vkDestroySemaphore DestroySemaphore = nullptr;
	PFN_vkQueueSubmit QueueSubmit = nullptr;
	PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
	PFN_vkCreateImageView CreateImageView = nullptr;
	PFN_vkDestroyImageView DestroyImageView = nullptr;
	PFN_vkCreateImage CreateImage = nullptr;
	PFN_vkDestroyImage DestroyImage = nullptr;
	PFN_vkAllocateMemory AllocateMemory = nullptr;
	PFN_vkFreeMemory FreeMemory = nullptr;
	PFN_vkBindImageMemory BindImageMemory = nullptr;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;

	template <typename T>
	static T Load(IVulkanDynamicRHI* RHI, const char* Name)
	{
		T Fn = reinterpret_cast<T>(RHI->RHIGetVkDeviceProcAddr(Name));
		if (!Fn)
		{
			Fn = reinterpret_cast<T>(RHI->RHIGetVkInstanceProcAddr(Name));
		}
		return Fn;
	}

	bool LoadAll(IVulkanDynamicRHI* RHI)
	{
#define MAGICSR_VK_LOAD(Field, Name) \
	Field = Load<PFN_##Name>(RHI, #Name); \
	if (!Field) \
	{ \
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter missing PFN %s"), TEXT(#Name)); \
		return false; \
	}
		MAGICSR_VK_LOAD(GetPhysicalDeviceQueueFamilyProperties, vkGetPhysicalDeviceQueueFamilyProperties);
		MAGICSR_VK_LOAD(GetDeviceQueue, vkGetDeviceQueue);
		MAGICSR_VK_LOAD(CreateWin32SurfaceKHR, vkCreateWin32SurfaceKHR);
		MAGICSR_VK_LOAD(DestroySurfaceKHR, vkDestroySurfaceKHR);
		MAGICSR_VK_LOAD(GetPhysicalDeviceSurfaceSupportKHR, vkGetPhysicalDeviceSurfaceSupportKHR);
		MAGICSR_VK_LOAD(GetPhysicalDeviceSurfaceCapabilitiesKHR, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
		MAGICSR_VK_LOAD(GetPhysicalDeviceSurfaceFormatsKHR, vkGetPhysicalDeviceSurfaceFormatsKHR);
		MAGICSR_VK_LOAD(GetPhysicalDeviceSurfacePresentModesKHR, vkGetPhysicalDeviceSurfacePresentModesKHR);
		MAGICSR_VK_LOAD(CreateSwapchainKHR, vkCreateSwapchainKHR);
		MAGICSR_VK_LOAD(DestroySwapchainKHR, vkDestroySwapchainKHR);
		MAGICSR_VK_LOAD(GetSwapchainImagesKHR, vkGetSwapchainImagesKHR);
		MAGICSR_VK_LOAD(AcquireNextImageKHR, vkAcquireNextImageKHR);
		MAGICSR_VK_LOAD(QueuePresentKHR, vkQueuePresentKHR);
		MAGICSR_VK_LOAD(CreateCommandPool, vkCreateCommandPool);
		MAGICSR_VK_LOAD(DestroyCommandPool, vkDestroyCommandPool);
		MAGICSR_VK_LOAD(AllocateCommandBuffers, vkAllocateCommandBuffers);
		MAGICSR_VK_LOAD(FreeCommandBuffers, vkFreeCommandBuffers);
		MAGICSR_VK_LOAD(ResetCommandBuffer, vkResetCommandBuffer);
		MAGICSR_VK_LOAD(BeginCommandBuffer, vkBeginCommandBuffer);
		MAGICSR_VK_LOAD(EndCommandBuffer, vkEndCommandBuffer);
		MAGICSR_VK_LOAD(CmdPipelineBarrier, vkCmdPipelineBarrier);
		MAGICSR_VK_LOAD(CmdBlitImage, vkCmdBlitImage);
		MAGICSR_VK_LOAD(CmdCopyImage, vkCmdCopyImage);
		MAGICSR_VK_LOAD(CmdClearColorImage, vkCmdClearColorImage);
		MAGICSR_VK_LOAD(CreateFence, vkCreateFence);
		MAGICSR_VK_LOAD(DestroyFence, vkDestroyFence);
		MAGICSR_VK_LOAD(WaitForFences, vkWaitForFences);
		MAGICSR_VK_LOAD(ResetFences, vkResetFences);
		MAGICSR_VK_LOAD(CreateSemaphore, vkCreateSemaphore);
		MAGICSR_VK_LOAD(DestroySemaphore, vkDestroySemaphore);
		MAGICSR_VK_LOAD(QueueSubmit, vkQueueSubmit);
		MAGICSR_VK_LOAD(QueueWaitIdle, vkQueueWaitIdle);
		MAGICSR_VK_LOAD(CreateImageView, vkCreateImageView);
		MAGICSR_VK_LOAD(DestroyImageView, vkDestroyImageView);
		MAGICSR_VK_LOAD(CreateImage, vkCreateImage);
		MAGICSR_VK_LOAD(DestroyImage, vkDestroyImage);
		MAGICSR_VK_LOAD(AllocateMemory, vkAllocateMemory);
		MAGICSR_VK_LOAD(FreeMemory, vkFreeMemory);
		MAGICSR_VK_LOAD(BindImageMemory, vkBindImageMemory);
		MAGICSR_VK_LOAD(GetImageMemoryRequirements, vkGetImageMemoryRequirements);
		MAGICSR_VK_LOAD(GetPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceMemoryProperties);
#undef MAGICSR_VK_LOAD
		return true;
	}
};

static const wchar_t* GPresenterClassName = L"MagicSRFgPresenterWnd";
static volatile int32 GPresenterClassRegistered = 0;

LRESULT CALLBACK MagicSRPresenterWndProc(HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
	if (Msg == WM_NCHITTEST)
	{
		return HTTRANSPARENT;
	}
	if (Msg == WM_MOUSEACTIVATE)
	{
		return MA_NOACTIVATE;
	}
	if (Msg == WM_ERASEBKGND)
	{
		return 1;
	}
	if (Msg == WM_PAINT)
	{
		ValidateRect(Hwnd, nullptr);
		return 0;
	}
	if (Msg == WM_SETCURSOR)
	{
		return TRUE;
	}
	return DefWindowProcW(Hwnd, Msg, WParam, LParam);
}

void RegisterPresenterClass()
{
	if (FPlatformAtomics::InterlockedCompareExchange(&GPresenterClassRegistered, 1, 0) != 0)
	{
		return;
	}
	WNDCLASSEXW Wc = {};
	Wc.cbSize = sizeof(Wc);
	Wc.style = CS_HREDRAW | CS_VREDRAW;
	Wc.lpfnWndProc = MagicSRPresenterWndProc;
	Wc.hInstance = reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
	Wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	Wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
	Wc.lpszClassName = GPresenterClassName;
	RegisterClassExW(&Wc);
}

HWND FindGameHwnd()
{
	if (GEngine && GEngine->GameViewport)
	{
		const TSharedPtr<SWindow> Win = GEngine->GameViewport->GetWindow();
		if (Win.IsValid() && Win->GetNativeWindow().IsValid())
		{
			return static_cast<HWND>(Win->GetNativeWindow()->GetOSWindowHandle());
		}
	}
	if (FSlateApplication::IsInitialized())
	{
		const TSharedPtr<SWindow> Win = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (Win.IsValid() && Win->GetNativeWindow().IsValid())
		{
			return static_cast<HWND>(Win->GetNativeWindow()->GetOSWindowHandle());
		}
	}
	return nullptr;
}

const TCHAR* PresentModeName(VkPresentModeKHR Mode)
{
	switch (Mode)
	{
	case VK_PRESENT_MODE_IMMEDIATE_KHR:
		return TEXT("IMMEDIATE");
	case VK_PRESENT_MODE_MAILBOX_KHR:
		return TEXT("MAILBOX");
	case VK_PRESENT_MODE_FIFO_KHR:
		return TEXT("FIFO");
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
		return TEXT("FIFO_RELAXED");
	default:
		return TEXT("OTHER");
	}
}

template <typename T>
void SafeReleaseCom(T*& Ptr)
{
	if (Ptr)
	{
		Ptr->Release();
		Ptr = nullptr;
	}
}

uint32 FindMemoryTypeIndex(FMagicVkApi& Api, VkPhysicalDevice Phys, uint32 TypeBits, VkMemoryPropertyFlags Flags)
{
	VkPhysicalDeviceMemoryProperties Props = {};
	Api.GetPhysicalDeviceMemoryProperties(Phys, &Props);
	for (uint32 i = 0; i < Props.memoryTypeCount; ++i)
	{
		if ((TypeBits & (1u << i)) && (Props.memoryTypes[i].propertyFlags & Flags) == Flags)
		{
			return i;
		}
	}
	for (uint32 i = 0; i < Props.memoryTypeCount; ++i)
	{
		if (TypeBits & (1u << i))
		{
			return i;
		}
	}
	return 0;
}

IDXGIAdapter* FindDxgiAdapterByLuid(uint64 AdapterLuid)
{
	IDXGIFactory1* Factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&Factory))) || !Factory)
	{
		return nullptr;
	}
	IDXGIAdapter* Found = nullptr;
	for (UINT i = 0;; ++i)
	{
		IDXGIAdapter* Adapter = nullptr;
		if (Factory->EnumAdapters(i, &Adapter) == DXGI_ERROR_NOT_FOUND)
		{
			break;
		}
		DXGI_ADAPTER_DESC Desc = {};
		Adapter->GetDesc(&Desc);
		uint64 Luid = 0;
		FMemory::Memcpy(&Luid, &Desc.AdapterLuid, sizeof(Luid));
		if (Luid == AdapterLuid)
		{
			Found = Adapter;
			break;
		}
		Adapter->Release();
	}
	Factory->Release();
	return Found;
}
} // namespace

struct FMagicSRPresenterVk
{
	FMagicVkApi Api;
	VkInstance Instance = VK_NULL_HANDLE;
	VkPhysicalDevice Phys = VK_NULL_HANDLE;
	VkDevice Device = VK_NULL_HANDLE;
	VkQueue Queue = VK_NULL_HANDLE;
	VkSurfaceKHR Surface = VK_NULL_HANDLE;
	VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
	VkCommandPool CmdPool = VK_NULL_HANDLE;
	VkFormat SwapFormat = VK_FORMAT_B8G8R8A8_UNORM;
	VkPresentModeKHR PresentMode = VK_PRESENT_MODE_FIFO_KHR;
	TArray<VkImage> Images;
	TArray<VkImageView> Views;
	TArray<VkCommandBuffer> Cmds;
	TArray<VkFence> InFlight;
	TArray<VkSemaphore> Acquired;
	TArray<VkSemaphore> Rendered;
	int32 Frame = 0;

	ID3D11Device* D3D = nullptr;
	ID3D11DeviceContext* Ctx = nullptr;
	IDXGISwapChain* DxgiSwap = nullptr;
	ID3D11Texture2D* DxgiBB = nullptr;
	ID3D11Texture2D* SharedTex = nullptr;
	HANDLE SharedHandle = nullptr;
	VkImage SharedImage = VK_NULL_HANDLE;
	VkDeviceMemory SharedMemory = VK_NULL_HANDLE;
	VkExternalMemoryHandleTypeFlagBits SharedHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	bool bD3D11 = false;
};

#endif // PLATFORM_WINDOWS

FMagicSRFgPresenter& FMagicSRFgPresenter::Get()
{
	static FMagicSRFgPresenter G;
	return G;
}

#if PLATFORM_WINDOWS
static double MagicSRFileTimeToSec(const FILETIME& Ft)
{
	ULARGE_INTEGER U;
	U.LowPart = Ft.dwLowDateTime;
	U.HighPart = Ft.dwHighDateTime;
	return double(U.QuadPart) * 1.0e-7;
}
#endif

namespace
{
double GMagicFgTickSec[32] = {};
uint64 GMagicFgTickFrame[32] = {};
}

void FMagicSRFgPresenter::NoteGameTickTime()
{
	if (!FMagicSRViewportBridge::IsPerfCollect())
	{
		return;
	}
	const uint64 F = GFrameCounter;
	const int32 i = static_cast<int32>(F & 31ull);
	GMagicFgTickSec[i] = FPlatformTime::Seconds();
	GMagicFgTickFrame[i] = F;
}

double FMagicSRFgPresenter::GetGameTickTimeForRenderFrame()
{
	const uint64 F = GFrameCounterRenderThread;
	const int32 i = static_cast<int32>(F & 31ull);
	if (GMagicFgTickFrame[i] == F)
	{
		return GMagicFgTickSec[i];
	}
	const uint64 F1 = F - 1ull;
	const int32 j = static_cast<int32>(F1 & 31ull);
	if (GMagicFgTickFrame[j] == F1)
	{
		return GMagicFgTickSec[j];
	}
	return GMagicFgTickSec[i];
}

void FMagicSRFgPresenter::BeginPerfWindow()
{
	FScopeLock Lock(&PerfCs);
	PerfCopyMsSum = 0.0;
	PerfCopySamples = 0;
	PerfHoldMsSum = 0.0;
	PerfHoldSamples = 0;
	PerfTickToPresentMsSum = 0.0;
	PerfTickToPresentSamples = 0;
	PerfTickToReadyMsSum = 0.0;
	PerfTickToReadySamples = 0;
	PerfReadyToPresentMsSum = 0.0;
	PerfReadyToPresentSamples = 0;
	PerfBlitWaitMsSum = 0.0;
	PerfD3dCopyMsSum = 0.0;
	PerfDxgiPresentMsSum = 0.0;
	PerfJobsDropped = 0;
	PerfQueueDepthSum = 0.0;
	PerfQueueDepthSamples = 0;
	PerfGpuInflightSum = 0.0;
	PerfGpuInflightSamples = 0;
	PerfPresenterBacklogSum = 0.0;
	PerfPresenterBacklogSamples = 0;
	QueueDepthHistN = 0;
	QueueDepthHistW = 0;
	QueueDepthMaxI = 0;
	FMemory::Memzero(QueueDepthHist, sizeof(QueueDepthHist));
	GpuInflightHistN = 0;
	GpuInflightHistW = 0;
	GpuInflightMaxI = 0;
	FMemory::Memzero(GpuInflightHist, sizeof(GpuInflightHist));
	PresenterBacklogHistN = 0;
	PresenterBacklogHistW = 0;
	PresenterBacklogMaxI = 0;
	FMemory::Memzero(PresenterBacklogHist, sizeof(PresenterBacklogHist));
	{
		FScopeLock TraceLock(&TraceCs);
		TraceWrite = 0;
		TraceCount = 0;
	}
	PerfDxgiCount0 = 0;
	PerfDxgiCountNow = 0;
	PerfDxgiStatsHr = 1;
	PerfDxgiStatsPresentCount = 0;
	PerfThreadCpu0Sec = 0.0;
	PerfThreadWall0 = FPlatformTime::Seconds();
#if PLATFORM_WINDOWS
	if (PerfThreadHandle)
	{
		CloseHandle(static_cast<HANDLE>(PerfThreadHandle));
		PerfThreadHandle = nullptr;
	}
	if (Thread)
	{
		PerfThreadHandle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, Thread->GetThreadID());
	}
	if (PerfThreadHandle)
	{
		FILETIME C, E, K, U;
		if (GetThreadTimes(static_cast<HANDLE>(PerfThreadHandle), &C, &E, &K, &U))
		{
			PerfThreadCpu0Sec = MagicSRFileTimeToSec(K) + MagicSRFileTimeToSec(U);
		}
	}
	FMagicSRPresenterVk* Vk = reinterpret_cast<FMagicSRPresenterVk*>(Api);
	if (Vk && Vk->DxgiSwap)
	{
		UINT Count = 0;
		if (SUCCEEDED(Vk->DxgiSwap->GetLastPresentCount(&Count)))
		{
			PerfDxgiCount0 = Count;
			PerfDxgiCountNow = Count;
		}
		DXGI_FRAME_STATISTICS Stats;
		FMemory::Memzero(&Stats, sizeof(Stats));
		const HRESULT Hr = Vk->DxgiSwap->GetFrameStatistics(&Stats);
		PerfDxgiStatsHr = static_cast<int32>(Hr);
		if (SUCCEEDED(Hr))
		{
			PerfDxgiStatsPresentCount = Stats.PresentCount;
		}
	}
#endif
}

FMagicSRFgPresenter::FPresenterPerfSnap FMagicSRFgPresenter::GetPerfSnap() const
{
	FPresenterPerfSnap Out;
	FScopeLock Lock(&PerfCs);
	Out.DxgiLastPresentCount = PerfDxgiCountNow;
	if (PerfDxgiCountNow >= PerfDxgiCount0)
	{
		Out.DxgiPresentDelta = static_cast<int32>(PerfDxgiCountNow - PerfDxgiCount0);
	}
	Out.DxgiStatsHr = PerfDxgiStatsHr;
	Out.DxgiStatsPresentCount = PerfDxgiStatsPresentCount;
	Out.CopySamples = PerfCopySamples;
	Out.HoldSamples = PerfHoldSamples;
	if (PerfCopySamples > 0)
	{
		Out.CopySubmitMsMean = static_cast<float>(PerfCopyMsSum / double(PerfCopySamples));
	}
	if (PerfHoldSamples > 0)
	{
		Out.RealHoldMsMean = static_cast<float>(PerfHoldMsSum / double(PerfHoldSamples));
	}
	Out.TickToPresentSamples = PerfTickToPresentSamples;
	if (PerfTickToPresentSamples > 0)
	{
		Out.TickToPresentMsMean = static_cast<float>(PerfTickToPresentMsSum / double(PerfTickToPresentSamples));
	}
	Out.TickToReadySamples = PerfTickToReadySamples;
	if (PerfTickToReadySamples > 0)
	{
		Out.TickToReadyMsMean = static_cast<float>(PerfTickToReadyMsSum / double(PerfTickToReadySamples));
	}
	Out.ReadyToPresentSamples = PerfReadyToPresentSamples;
	if (PerfReadyToPresentSamples > 0)
	{
		Out.ReadyToPresentMsMean = static_cast<float>(PerfReadyToPresentMsSum / double(PerfReadyToPresentSamples));
	}
	if (PerfCopySamples > 0)
	{
		Out.BlitWaitMsMean = static_cast<float>(PerfBlitWaitMsSum / double(PerfCopySamples));
		Out.D3dCopyMsMean = static_cast<float>(PerfD3dCopyMsSum / double(PerfCopySamples));
		Out.DxgiPresentMsMean = static_cast<float>(PerfDxgiPresentMsSum / double(PerfCopySamples));
	}
	Out.JobsDropped = PerfJobsDropped;
	if (PerfQueueDepthSamples > 0)
	{
		Out.QueueDepthMean = static_cast<float>(PerfQueueDepthSum / double(PerfQueueDepthSamples));
	}
	Out.QueueDepthMax = static_cast<float>(QueueDepthMaxI);
	if (QueueDepthHistN > 0)
	{
		uint8 Sorted[kQHist];
		FMemory::Memcpy(Sorted, QueueDepthHist, QueueDepthHistN);
		for (int32 i = 1; i < QueueDepthHistN; ++i)
		{
			const uint8 V = Sorted[i];
			int32 j = i;
			while (j > 0 && Sorted[j - 1] > V)
			{
				Sorted[j] = Sorted[j - 1];
				--j;
			}
			Sorted[j] = V;
		}
		const int32 P95 = FMath::Clamp(int32(double(QueueDepthHistN - 1) * 0.95), 0, QueueDepthHistN - 1);
		Out.QueueDepthP95 = static_cast<float>(Sorted[P95]);
	}
	if (PerfGpuInflightSamples > 0)
	{
		Out.GpuInflightMean = static_cast<float>(PerfGpuInflightSum / double(PerfGpuInflightSamples));
	}
	Out.GpuInflightMax = static_cast<float>(GpuInflightMaxI);
	if (GpuInflightHistN > 0)
	{
		uint8 Sorted[kQHist];
		FMemory::Memcpy(Sorted, GpuInflightHist, GpuInflightHistN);
		for (int32 i = 1; i < GpuInflightHistN; ++i)
		{
			const uint8 V = Sorted[i];
			int32 j = i;
			while (j > 0 && Sorted[j - 1] > V)
			{
				Sorted[j] = Sorted[j - 1];
				--j;
			}
			Sorted[j] = V;
		}
		const int32 P95 = FMath::Clamp(int32(double(GpuInflightHistN - 1) * 0.95), 0, GpuInflightHistN - 1);
		Out.GpuInflightP95 = static_cast<float>(Sorted[P95]);
	}
	if (PerfPresenterBacklogSamples > 0)
	{
		Out.PresenterBacklogMean = static_cast<float>(PerfPresenterBacklogSum / double(PerfPresenterBacklogSamples));
	}
	Out.PresenterBacklogMax = static_cast<float>(PresenterBacklogMaxI);
	if (PresenterBacklogHistN > 0)
	{
		uint8 Sorted[kQHist];
		FMemory::Memcpy(Sorted, PresenterBacklogHist, PresenterBacklogHistN);
		for (int32 i = 1; i < PresenterBacklogHistN; ++i)
		{
			const uint8 V = Sorted[i];
			int32 j = i;
			while (j > 0 && Sorted[j - 1] > V)
			{
				Sorted[j] = Sorted[j - 1];
				--j;
			}
			Sorted[j] = V;
		}
		const int32 P95 = FMath::Clamp(int32(double(PresenterBacklogHistN - 1) * 0.95), 0, PresenterBacklogHistN - 1);
		Out.PresenterBacklogP95 = static_cast<float>(Sorted[P95]);
	}
#if PLATFORM_WINDOWS
	if (PerfThreadHandle && PerfThreadWall0 > 0.0)
	{
		FILETIME C, E, K, U;
		if (GetThreadTimes(static_cast<HANDLE>(PerfThreadHandle), &C, &E, &K, &U))
		{
			const double Cpu = MagicSRFileTimeToSec(K) + MagicSRFileTimeToSec(U);
			const double Wall = FPlatformTime::Seconds() - PerfThreadWall0;
			if (Wall > 1.0e-4)
			{
				Out.ThreadCpuPct = static_cast<float>(((Cpu - PerfThreadCpu0Sec) / Wall) * 100.0);
			}
		}
	}
	FMagicSRPresenterVk* Vk = reinterpret_cast<FMagicSRPresenterVk*>(Api);
	if (Vk && Vk->DxgiSwap)
	{
		DXGI_FRAME_STATISTICS Stats;
		FMemory::Memzero(&Stats, sizeof(Stats));
		const HRESULT Hr = Vk->DxgiSwap->GetFrameStatistics(&Stats);
		Out.DxgiStatsHr = static_cast<int32>(Hr);
		if (SUCCEEDED(Hr))
		{
			Out.DxgiStatsPresentCount = Stats.PresentCount;
		}
	}
#endif
	return Out;
}

void FMagicSRFgPresenter::EnsureTicker()
{
	if (TickerHandle.IsValid())
	{
		return;
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FMagicSRFgPresenter::TickGameThread), 0.f);
}

void FMagicSRFgPresenter::RemoveTicker()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	StopAndRelease();
}

void FMagicSRFgPresenter::StopAndRelease()
{
	FPlatformAtomics::InterlockedExchange(&bWantRun, 0);
	FPlatformAtomics::InterlockedExchange(&bAcceptFrames, 0);
	FPlatformAtomics::InterlockedExchange(&bThreadRun, 0);
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
#if PLATFORM_WINDOWS
	DestroyVulkan();
	HideAndDestroyWindow();
#endif
	if (IsInRenderingThread())
	{
		ReleaseRing_RenderThread();
	}
	else if (IsInGameThread())
	{
		ENQUEUE_RENDER_COMMAND(MagicSRPresenterReleaseRing)(
			[](FRHICommandListImmediate&)
			{
				FMagicSRFgPresenter::Get().ReleaseRing_RenderThread();
			});
	}
	FPlatformAtomics::InterlockedExchange(&bHasPresented, 0);
	LastRealPresentTime = 0.0;
	LastGenPresentTime = 0.0;
	LastRealTarget = 0.0;
	EngineIntervalEma = 0.0;
	LastEnginePushTime = 0.0;
	LastRealReadyTime = 0.0;
	RealReadyN = 0;
	RealReadyW = 0;
#if PLATFORM_WINDOWS
	if (PerfThreadHandle)
	{
		CloseHandle(static_cast<HANDLE>(PerfThreadHandle));
		PerfThreadHandle = nullptr;
	}
#endif
}

bool FMagicSRFgPresenter::TickGameThread(float DeltaTime)
{
	(void)DeltaTime;
	NoteGameTickTime();
	const int32 PresentMode = FMagicSRViewportBridge::GetFrameGenPresentMode();
	const int32 FgMode = FMagicSRViewportBridge::GetFrameGenMode();
	const bool bOn = PresentMode == 2 && FgMode > 0 && !FMagicSRViewportBridge::GetFrameGenShowMid();
	if (!bOn)
	{
		if (Thread || OverlayHwnd)
		{
			StopAndRelease();
		}
		return true;
	}

	FPlatformAtomics::InterlockedExchange(&bWantRun, 1);
#if PLATFORM_WINDOWS
	EnsureOverlayWindow();
	SyncOverlayWindow();
	if (OverlayHwnd && OverlayW > 0 && OverlayH > 0)
	{
		if (!bVulkanReady)
		{
			EnsureVulkan(OverlayHwnd);
		}
		if (bVulkanReady &&
			(SwapW != OverlayW || SwapH != OverlayH || RecreateFlag))
		{
			FScopeLock Lock(&VkCs);
			RecreateSwapchain(OverlayHwnd, OverlayW, OverlayH);
			FPlatformAtomics::InterlockedExchange(&RecreateFlag, 0);
		}
	}
#endif
	if (bVulkanReady && !Thread)
	{
		FPlatformAtomics::InterlockedExchange(&bThreadRun, 1);
		FPlatformAtomics::InterlockedExchange(&bAcceptFrames, 1);
		if (!WakeEvent)
		{
			WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
		}
		Thread = FRunnableThread::Create(this, TEXT("MagicSRPresent"), 0, TPri_TimeCritical);
		UE_LOG(LogTemp, Display, TEXT("[MagicSR] Presenter thread started (Arch P D3D11 bitblt)"));
	}
	return true;
}

void FMagicSRFgPresenter::NoteEnginePushInterval()
{
	const double PushNow = FPlatformTime::Seconds();
	if (LastEnginePushTime > 0.0)
	{
		const double Dt = PushNow - LastEnginePushTime;
		if (Dt > 0.004 && Dt < 0.100)
		{
			EngineIntervalEma = (EngineIntervalEma < 0.004) ? Dt : (EngineIntervalEma * 0.7 + Dt * 0.3);
		}
	}
	LastEnginePushTime = PushNow;
}

int32 FMagicSRFgPresenter::JobCount_NoLock() const
{
	return (JobWrite - JobRead + kJobCap) % kJobCap;
}

void FMagicSRFgPresenter::NoteQueueDepth_NoLock()
{
	if (!FMagicSRViewportBridge::IsPerfCollect())
	{
		return;
	}
	const int32 Depth = JobCount_NoLock();
	FScopeLock PerfLock(&PerfCs);
	PerfQueueDepthSum += double(Depth);
	++PerfQueueDepthSamples;
	if (Depth > QueueDepthMaxI)
	{
		QueueDepthMaxI = Depth;
	}
	QueueDepthHist[QueueDepthHistW] = static_cast<uint8>(FMath::Clamp(Depth, 0, 255));
	QueueDepthHistW = (QueueDepthHistW + 1) % kQHist;
	if (QueueDepthHistN < kQHist)
	{
		++QueueDepthHistN;
	}
}

void FMagicSRFgPresenter::CountQueueSplit_NoLock(int32& OutInflight, int32& OutBacklog) const
{
	OutInflight = 0;
	OutBacklog = 0;
	int32 i = JobRead;
	while (i != JobWrite)
	{
		const int32 S = Jobs[i].Slot;
		const bool bReady = S >= 0 && S < kSlotCount && Slots[S].Fence.IsValid() && Slots[S].Fence->Poll();
		if (bReady)
		{
			++OutBacklog;
		}
		else
		{
			++OutInflight;
		}
		i = (i + 1) % kJobCap;
	}
}

void FMagicSRFgPresenter::NoteQueueSplit(int32 Inflight, int32 Backlog)
{
	if (!FMagicSRViewportBridge::IsPerfCollect())
	{
		return;
	}
	FScopeLock PerfLock(&PerfCs);
	PerfGpuInflightSum += double(Inflight);
	++PerfGpuInflightSamples;
	if (Inflight > GpuInflightMaxI)
	{
		GpuInflightMaxI = Inflight;
	}
	GpuInflightHist[GpuInflightHistW] = static_cast<uint8>(FMath::Clamp(Inflight, 0, 255));
	GpuInflightHistW = (GpuInflightHistW + 1) % kQHist;
	if (GpuInflightHistN < kQHist)
	{
		++GpuInflightHistN;
	}
	PerfPresenterBacklogSum += double(Backlog);
	++PerfPresenterBacklogSamples;
	if (Backlog > PresenterBacklogMaxI)
	{
		PresenterBacklogMaxI = Backlog;
	}
	PresenterBacklogHist[PresenterBacklogHistW] = static_cast<uint8>(FMath::Clamp(Backlog, 0, 255));
	PresenterBacklogHistW = (PresenterBacklogHistW + 1) % kQHist;
	if (PresenterBacklogHistN < kQHist)
	{
		++PresenterBacklogHistN;
	}
}

void FMagicSRFgPresenter::DropOldestGens_NoLock(int32 MaxKeep)
{
	MaxKeep = FMath::Clamp(MaxKeep, 0, kJobCap - 1);
	FJob Kept[kJobCap];
	int32 N = 0;
	int32 i = JobRead;
	while (i != JobWrite)
	{
		Kept[N++] = Jobs[i];
		i = (i + 1) % kJobCap;
	}
	while (N > MaxKeep)
	{
		int32 DropIdx = -1;
		for (int32 k = 0; k < N; ++k)
		{
			if (Kept[k].Kind == 1 && Kept[k].Slot != InFlightSlot)
			{
				DropIdx = k;
				break;
			}
		}
		if (DropIdx < 0)
		{
			break;
		}
		const int32 OldSlot = Kept[DropIdx].Slot;
		if (OldSlot >= 0 && OldSlot < kSlotCount && OldSlot != InFlightSlot)
		{
			FPlatformAtomics::InterlockedExchange(&Slots[OldSlot].State, 0);
		}
		if (FMagicSRViewportBridge::IsPerfCollect())
		{
			FScopeLock PerfLock(&PerfCs);
			++PerfJobsDropped;
		}
		for (int32 k = DropIdx; k < N - 1; ++k)
		{
			Kept[k] = Kept[k + 1];
		}
		--N;
	}
	JobRead = 0;
	JobWrite = N;
	for (int32 k = 0; k < N; ++k)
	{
		Jobs[k] = Kept[k];
	}
}

void FMagicSRFgPresenter::NoteRealReadyInterval(double ReadyTime)
{
	if (LastRealReadyTime > 0.0)
	{
		const double D = ReadyTime - LastRealReadyTime;
		if (D > 0.004 && D < 0.080)
		{
			RealReadyDt[RealReadyW] = D;
			RealReadyW = (RealReadyW + 1) % kDtHist;
			if (RealReadyN < kDtHist)
			{
				++RealReadyN;
			}
		}
	}
	LastRealReadyTime = ReadyTime;
}

double FMagicSRFgPresenter::EstimateDtSec() const
{
	const double E = EngineIntervalEma;
	const bool bEngineOk = (E >= 0.004 && E <= 0.100);
	const int32 N = RealReadyN;
	if (N >= 3)
	{
		double Tmp[kDtHist];
		for (int32 i = 0; i < N; ++i)
		{
			Tmp[i] = RealReadyDt[i];
		}
		for (int32 i = 1; i < N; ++i)
		{
			const double V = Tmp[i];
			int32 j = i;
			while (j > 0 && Tmp[j - 1] > V)
			{
				Tmp[j] = Tmp[j - 1];
				--j;
			}
			Tmp[j] = V;
		}
		const double Med = Tmp[N / 2];
		// Ready-to-ready of reals includes present-thread waits when the queue
		// is already signaled, which inflates dt_est (~28 ms) and keeps FIFO
		// from draining. Trust median only when it agrees with engine push dt.
		if (!bEngineOk)
		{
			return Med;
		}
		if (Med >= E * 0.70 && Med <= E * 1.30)
		{
			return Med;
		}
	}
	if (bEngineOk)
	{
		return E;
	}
	return 0.016;
}

int32 FMagicSRFgPresenter::GetTraceCount() const
{
	FScopeLock Lock(&TraceCs);
	return TraceCount;
}

void FMagicSRFgPresenter::DumpTraceCsv(const TCHAR* Path) const
{
	if (!Path || !Path[0])
	{
		return;
	}
	TArray<FTraceRec> Copy;
	{
		FScopeLock Lock(&TraceCs);
		Copy.Reserve(TraceCount);
		const int32 Oldest = (TraceCount == kTraceCap) ? (TraceWrite % kTraceCap) : 0;
		for (int32 i = 0; i < TraceCount; ++i)
		{
			Copy.Add(TraceRing[(Oldest + i) % kTraceCap]);
		}
	}
	FString Out;
	Out.Reserve(Copy.Num() * 120 + 160);
	Out += TEXT("seq,kind,enqueue_s,ready_s,target_s,present_s,tick_s,last_real_s,qdepth,gpu_a,present_b,dt_est_ms,behind_ms,wait_ms,resync,dropped,pace\n");
	for (const FTraceRec& R : Copy)
	{
		Out += FString::Printf(
			TEXT("%u,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%.3f,%.3f,%.3f,%d,%d,%d\n"),
			R.Seq, R.Kind, R.EnqueueSec, R.ReadySec, R.TargetSec, R.PresentSec, R.GameTickSec,
			R.LastRealPresentSec, R.QueueDepth, R.GpuInflight, R.PresenterBacklog, R.DtEstMs, R.BehindMs, R.WaitMs,
			R.Resync, R.DroppedSoFar, R.PaceMode);
	}
	FFileHelper::SaveStringToFile(Out, Path);
}

int32 FMagicSRFgPresenter::PeekPendingSlot_NoLock(int32& OutKind) const
{
	auto Ready = [this](int32 S) -> bool
	{
		return S >= 0 && (!Slots[S].Fence.IsValid() || Slots[S].Fence->Poll());
	};
	OutKind = 0;
	if (LastRealPresentTime > 0.0 && Ready(PendingGenSlot))
	{
		OutKind = 1;
		return PendingGenSlot;
	}
	if (Ready(PendingRealSlot))
	{
		OutKind = 2;
		return PendingRealSlot;
	}
	if (LastRealPresentTime > 0.0 && PendingGenSlot >= 0)
	{
		OutKind = 1;
		return PendingGenSlot;
	}
	if (PendingRealSlot >= 0)
	{
		OutKind = 2;
		return PendingRealSlot;
	}
	if (PendingGenSlot >= 0)
	{
		OutKind = 1;
		return PendingGenSlot;
	}
	return -1;
}

void FMagicSRFgPresenter::EnqueueJob(int32 Slot, int32 Kind, bool bSkipPace)
{
	{
		FScopeLock Lock(&JobCs);
		if (CVarMagicSRFgPresenterLatestWins.GetValueOnAnyThread() != 0)
		{
			FJob Kept[kJobCap];
			int32 N = 0;
			int32 i = JobRead;
			while (i != JobWrite)
			{
				const FJob J = Jobs[i];
				i = (i + 1) % kJobCap;
				if (J.Kind == Kind && J.Slot != Slot && J.Slot != InFlightSlot)
				{
					if (J.Slot >= 0 && J.Slot < kSlotCount)
					{
						FPlatformAtomics::InterlockedExchange(&Slots[J.Slot].State, 0);
					}
					if (FMagicSRViewportBridge::IsPerfCollect())
					{
						FScopeLock PerfLock(&PerfCs);
						++PerfJobsDropped;
					}
					continue;
				}
				Kept[N++] = J;
			}
			JobRead = 0;
			JobWrite = N;
			for (int32 k = 0; k < N; ++k)
			{
				Jobs[k] = Kept[k];
			}
		}
		if (CVarMagicSRFgPresenterPace.GetValueOnAnyThread() != 0 &&
			CVarMagicSRFgPresenterLatestWins.GetValueOnAnyThread() == 0)
		{
			const int32 SoftCap = FMath::Clamp(
				CVarMagicSRFgPresenterQueueSoftCap.GetValueOnAnyThread(), 2, kJobCap - 1);
			if (JobCount_NoLock() >= SoftCap)
			{
				DropOldestGens_NoLock(SoftCap - 1);
			}
		}
		int32 Next = (JobWrite + 1) % kJobCap;
		if (Next == JobRead)
		{
			const int32 OldSlot = Jobs[JobRead].Slot;
			if (OldSlot != InFlightSlot)
			{
				JobRead = (JobRead + 1) % kJobCap;
				if (OldSlot >= 0 && OldSlot < kSlotCount && OldSlot != Slot)
				{
					FPlatformAtomics::InterlockedExchange(&Slots[OldSlot].State, 0);
				}
				if (FMagicSRViewportBridge::IsPerfCollect())
				{
					FScopeLock PerfLock(&PerfCs);
					++PerfJobsDropped;
				}
			}
			Next = (JobWrite + 1) % kJobCap;
		}
		if (Next != JobRead)
		{
			Jobs[JobWrite].Slot = Slot;
			Jobs[JobWrite].Kind = Kind;
			Jobs[JobWrite].SkipPace = bSkipPace ? 1 : 0;
			JobWrite = Next;
			NoteQueueDepth_NoLock();
		}
		else
		{
			FPlatformAtomics::InterlockedExchange(&Slots[Slot].State, 0);
			if (FMagicSRViewportBridge::IsPerfCollect())
			{
				FScopeLock PerfLock(&PerfCs);
				++PerfJobsDropped;
			}
		}
	}
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

int32 FMagicSRFgPresenter::AcquireSlot_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* SizeSrc)
{
	if (!SizeSrc || FPlatformAtomics::AtomicRead(&bAcceptFrames) == 0)
	{
		return -1;
	}
	const FIntPoint Size = SizeSrc->GetSizeXY();
	FPlatformAtomics::InterlockedExchange(&DesiredW, Size.X);
	FPlatformAtomics::InterlockedExchange(&DesiredH, Size.Y);
	return AllocSlot_RenderThread(RHICmdList, SizeSrc);
}

FRHITexture* FMagicSRFgPresenter::GetSlotRHI(int32 SlotIndex) const
{
	if (SlotIndex < 0 || SlotIndex >= kSlotCount)
	{
		return nullptr;
	}
	return Slots[SlotIndex].Tex.GetReference();
}

void FMagicSRFgPresenter::FinishSlot_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	int32 SlotIndex,
	int32 Kind,
	bool bSkipPace)
{
	if (SlotIndex < 0 || SlotIndex >= kSlotCount)
	{
		return;
	}
	if (Slots[SlotIndex].Fence.IsValid())
	{
		Slots[SlotIndex].Fence->Clear();
		RHICmdList.WriteGPUFence(Slots[SlotIndex].Fence);
	}
	Slots[SlotIndex].Kind = Kind;
	Slots[SlotIndex].Seq = NextSeq;
	Slots[SlotIndex].SkipPace = bSkipPace ? 1 : 0;
	const bool bStamp = FMagicSRViewportBridge::IsPerfCollect() || PresenterTraceOn();
	Slots[SlotIndex].PushTime = bStamp ? FPlatformTime::Seconds() : 0.0;
	Slots[SlotIndex].GameTickTime = bStamp ? GetGameTickTimeForRenderFrame() : 0.0;
	if (Kind == 2)
	{
		++NextSeq;
		NoteEnginePushInterval();
	}
	FPlatformMisc::MemoryBarrier();
	FPlatformAtomics::InterlockedExchange(&Slots[SlotIndex].State, 1);
	EnqueueJob(SlotIndex, Kind, bSkipPace);
}

void FMagicSRFgPresenter::PushFrame_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FRHITexture* Src,
	int32 Kind,
	bool bSkipPace)
{
	const int32 Slot = AcquireSlot_RenderThread(RHICmdList, Src);
	if (Slot < 0 || !Src)
	{
		return;
	}
	if (Slots[Slot].Tex.IsValid() && Src->GetSizeXY() != Slots[Slot].Tex->GetSizeXY())
	{
		FreeSlot(Slot);
		return;
	}

	FRHICopyTextureInfo Info;
	const FIntPoint Ss = Src->GetSizeXY();
	Info.Size = FIntVector(Ss.X, Ss.Y, 1);
	RHICmdList.Transition(FRHITransitionInfo(Src, ERHIAccess::Unknown, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(Slots[Slot].Tex, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(Src, Slots[Slot].Tex, Info);
	RHICmdList.Transition(FRHITransitionInfo(Src, ERHIAccess::CopySrc, ERHIAccess::SRVGraphics));
	RHICmdList.Transition(FRHITransitionInfo(Slots[Slot].Tex, ERHIAccess::CopyDest, ERHIAccess::CopySrc));
	FinishSlot_RenderThread(RHICmdList, Slot, Kind, bSkipPace);
}

void FMagicSRFgPresenter::ReleaseRing_RenderThread()
{
	for (int32 i = 0; i < kSlotCount; ++i)
	{
		Slots[i].Tex.SafeRelease();
		Slots[i].Fence.SafeRelease();
		Slots[i].VkImage = 0;
		Slots[i].State = 0;
	}
	JobRead = 0;
	JobWrite = 0;
	PendingGenSlot = -1;
	PendingRealSlot = -1;
	InFlightSlot = -1;
}

int32 FMagicSRFgPresenter::AllocSlot_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* Src)
{
	const FIntPoint Size = Src->GetSizeXY();
	const EPixelFormat Fmt = Src->GetFormat();
	for (int32 i = 0; i < kSlotCount; ++i)
	{
		if (FPlatformAtomics::InterlockedCompareExchange(&Slots[i].State, 3, 0) != 0)
		{
			continue;
		}
		if (!Slots[i].Tex.IsValid() || Slots[i].Tex->GetSizeXY() != Size || Slots[i].Tex->GetFormat() != Fmt)
		{
			const FRHITextureCreateDesc Desc =
				FRHITextureCreateDesc::Create2D(*FString::Printf(TEXT("MagicFG.PresentRing%d"), i), Size.X, Size.Y, Fmt)
					.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable)
					.SetInitialState(ERHIAccess::SRVMask);
			Slots[i].Tex = RHICmdList.CreateTexture(Desc);
			Slots[i].Fence = RHICreateGPUFence(*FString::Printf(TEXT("MagicFG.PresentFence%d"), i));
#if PLATFORM_WINDOWS
			if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan && Slots[i].Tex.IsValid())
			{
				IVulkanDynamicRHI* VulkanRHI = GetIVulkanDynamicRHI();
				Slots[i].VkImage = reinterpret_cast<uint64>(VulkanRHI->RHIGetVkImage(Slots[i].Tex));
			}
#endif
			Slots[i].Width = Size.X;
			Slots[i].Height = Size.Y;
		}
		return i;
	}
	return -1;
}

void FMagicSRFgPresenter::FreeSlot(int32 SlotIndex)
{
	if (SlotIndex >= 0 && SlotIndex < kSlotCount)
	{
		FPlatformAtomics::InterlockedExchange(&Slots[SlotIndex].State, 0);
	}
}

void FMagicSRFgPresenter::Stop()
{
	FPlatformAtomics::InterlockedExchange(&bThreadRun, 0);
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

uint32 FMagicSRFgPresenter::Run()
{
#if PLATFORM_WINDOWS
	if (!WaitableTimer)
	{
		WaitableTimer = CreateWaitableTimerExW(
			nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (!WaitableTimer)
		{
			WaitableTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
		}
	}

	double ColorT = FPlatformTime::Seconds();
	while (FPlatformAtomics::AtomicRead(&bThreadRun) != 0)
	{
		const int32 Debug = CVarMagicSRFgPresenterDebug.GetValueOnAnyThread();
		if (Debug == 1)
		{
			const double Now = FPlatformTime::Seconds();
			if (Now - ColorT >= 0.008)
			{
				ColorT = Now;
				const bool bA = (DebugPresents & 1) == 0;
				PresentColor(bA ? 0.15f : 0.85f, bA ? 0.55f : 0.25f, bA ? 0.95f : 0.20f, OverlayW, OverlayH);
				++DebugPresents;
			}
			if (WakeEvent)
			{
				WakeEvent->Wait(1);
			}
			else
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			continue;
		}

		FJob Job;
		Job.Slot = -1;
		{
			FScopeLock Lock(&JobCs);
			if (JobRead != JobWrite)
			{
				Job = Jobs[JobRead];
				InFlightSlot = Job.Slot;
			}
		}
		if (Job.Slot < 0)
		{
			if (WakeEvent)
			{
				WakeEvent->Wait(2);
			}
			else
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			continue;
		}

		FSlot& Slot = Slots[Job.Slot];
		int32 Spin = 0;
		while (FPlatformAtomics::AtomicRead(&bThreadRun) != 0 && Slot.Fence.IsValid() && !Slot.Fence->Poll())
		{
			++Spin;
			if (Spin < 64)
			{
				FPlatformProcess::SleepNoStats(0.0f);
			}
			else
			{
				FPlatformProcess::SleepNoStats(0.0002f);
			}
		}
		if (FPlatformAtomics::AtomicRead(&bThreadRun) == 0)
		{
			break;
		}
		if (Slot.Fence.IsValid() && !Slot.Fence->Poll())
		{
			{
				FScopeLock Lock(&JobCs);
				InFlightSlot = -1;
			}
			if (WakeEvent)
			{
				WakeEvent->Wait(1);
			}
			continue;
		}
		const double ReadyNow = FPlatformTime::Seconds();
		int32 QueueDepthAtReady = 0;
		int32 GpuInflightAtReady = 0;
		int32 PresenterBacklogAtReady = 0;
		{
			FScopeLock Lock(&JobCs);
			if (CVarMagicSRFgPresenterPace.GetValueOnAnyThread() != 0 &&
				CVarMagicSRFgPresenterLatestWins.GetValueOnAnyThread() == 0)
			{
				const int32 SoftCap = FMath::Clamp(
					CVarMagicSRFgPresenterQueueSoftCap.GetValueOnAnyThread(), 2, kJobCap - 1);
				if (JobCount_NoLock() > SoftCap)
				{
					DropOldestGens_NoLock(SoftCap);
				}
			}
			if (JobRead != JobWrite && Jobs[JobRead].Slot == Job.Slot)
			{
				QueueDepthAtReady = JobCount_NoLock();
				JobRead = (JobRead + 1) % kJobCap;
				NoteQueueDepth_NoLock();
				CountQueueSplit_NoLock(GpuInflightAtReady, PresenterBacklogAtReady);
			}
			else
			{
				InFlightSlot = -1;
				continue;
			}
		}
		// Current job is fence-ready and not yet presented → presenter backlog (b).
		++PresenterBacklogAtReady;
		NoteQueueSplit(GpuInflightAtReady, PresenterBacklogAtReady);
		if (FMagicSRViewportBridge::IsPerfCollect() && Slot.GameTickTime > 0.0)
		{
			FScopeLock Lock(&PerfCs);
			PerfTickToReadyMsSum += (ReadyNow - Slot.GameTickTime) * 1000.0;
			++PerfTickToReadySamples;
		}

		if (Job.Kind == 2 && QueueDepthAtReady <= 3)
		{
			NoteRealReadyInterval(ReadyNow);
		}

		const bool bSkip = Job.SkipPace != 0 ||
			(FMagicSRViewportBridge::IsPerfCollect() &&
			 CVarMagicSRFgPresenterSkipPace.GetValueOnAnyThread() != 0);
		const int32 PaceMode = CVarMagicSRFgPresenterPace.GetValueOnAnyThread();
		const double PaceEma = EngineIntervalEma;
		const double DtEst = EstimateDtSec();
		double Target = ReadyNow;
		int32 Resync = 0;
		if (!bSkip)
		{
			if (PaceMode == 0)
			{
				if (Job.Kind == 1 && PaceEma >= 0.004 && PaceEma <= 0.100 && LastRealTarget > 0.0)
				{
					Target = LastRealTarget + PaceEma * 0.5;
					PreciseWaitUntil(Target);
				}
				else if (Job.Kind == 2 && LastRealTarget > 0.0 && PaceEma >= 0.004 && PaceEma <= 0.100)
				{
					Target = LastRealTarget + PaceEma;
					PreciseWaitUntil(Target);
				}
			}
			else
			{
				const double Half = DtEst * 0.5;
				constexpr double Slack = 0.002;
				// PresentSlot (blit+DXGI) runs after the wait and adds ~0.8 ms to
				// gen→real. Aim the wait so present-to-present lands on Half.
				constexpr double PresentFudge = 0.0008;
				const int32 DrainDepth = FMath::Clamp(
					CVarMagicSRFgPresenterDrainDepth.GetValueOnAnyThread(), 0, kJobCap - 1);
				if (Job.Kind == 1)
				{
					Target = (LastRealPresentTime > 0.0)
						? FMath::Max(ReadyNow, LastRealPresentTime + Half - PresentFudge)
						: ReadyNow;
				}
				else
				{
					if (LastGenPresentTime > 0.0)
					{
						Target = LastGenPresentTime + Half - PresentFudge;
					}
					else if (LastRealPresentTime > 0.0)
					{
						Target = LastRealPresentTime + DtEst - PresentFudge;
					}
					else
					{
						Target = ReadyNow;
					}
				}
				if (QueueDepthAtReady > DrainDepth)
				{
					Target = ReadyNow;
					Resync = 2;
				}
				else if (Target < ReadyNow - Slack)
				{
					Target = ReadyNow;
					Resync = 1;
				}
				if (QueueDepthAtReady <= DrainDepth)
				{
					PreciseWaitUntil(Target, FMath::Clamp(DtEst * 0.85, 0.008, 0.050));
				}
			}
		}
		const double WaitDone = FPlatformTime::Seconds();

		const bool bOk = PresentSlot(Job.Slot);
		if (bOk)
		{
			const double Now = FPlatformTime::Seconds();
			const double PrevRealPresent = LastRealPresentTime;
			if (FMagicSRViewportBridge::IsPerfCollect() && Job.Kind == 2)
			{
				FScopeLock Lock(&PerfCs);
				if (Slot.PushTime > 0.0)
				{
					PerfHoldMsSum += (Now - Slot.PushTime) * 1000.0;
					++PerfHoldSamples;
				}
				if (Slot.GameTickTime > 0.0)
				{
					PerfTickToPresentMsSum += (Now - Slot.GameTickTime) * 1000.0;
					++PerfTickToPresentSamples;
				}
				PerfReadyToPresentMsSum += (Now - ReadyNow) * 1000.0;
				++PerfReadyToPresentSamples;
			}
			FMagicSRViewportBridge::Get().NotifyPresenterPresent(Job.Kind);
			if (Job.Kind == 2)
			{
				LastRealPresentTime = Now;
				if (PaceMode == 0)
				{
					if (LastRealTarget <= 0.0 || PaceEma < 0.004)
					{
						LastRealTarget = Now;
					}
					else
					{
						LastRealTarget += PaceEma;
						if (Now > LastRealTarget + 0.003)
						{
							LastRealTarget = Now;
						}
					}
				}
				else
				{
					LastRealTarget = Now;
				}
			}
			else if (Job.Kind == 1)
			{
				LastGenPresentTime = Now;
			}
			if (PresenterTraceOn())
			{
				int32 DroppedCopy = 0;
				if (FMagicSRViewportBridge::IsPerfCollect())
				{
					FScopeLock Lock(&PerfCs);
					DroppedCopy = PerfJobsDropped;
				}
				FTraceRec Rec;
				Rec.Seq = static_cast<uint32>(Slot.Seq);
				Rec.Kind = Job.Kind;
				Rec.QueueDepth = QueueDepthAtReady;
				Rec.GpuInflight = GpuInflightAtReady;
				Rec.PresenterBacklog = PresenterBacklogAtReady;
				Rec.Resync = Resync;
				Rec.PaceMode = PaceMode;
				Rec.DroppedSoFar = DroppedCopy;
				Rec.DtEstMs = static_cast<float>(DtEst * 1000.0);
				Rec.BehindMs = static_cast<float>((ReadyNow - Target) * 1000.0);
				Rec.WaitMs = static_cast<float>((WaitDone - ReadyNow) * 1000.0);
				Rec.EnqueueSec = Slot.PushTime;
				Rec.ReadySec = ReadyNow;
				Rec.TargetSec = Target;
				Rec.PresentSec = Now;
				Rec.GameTickSec = Slot.GameTickTime;
				Rec.LastRealPresentSec = PrevRealPresent;
				FScopeLock TraceLock(&TraceCs);
				TraceRing[TraceWrite % kTraceCap] = Rec;
				++TraceWrite;
				if (TraceCount < kTraceCap)
				{
					++TraceCount;
				}
			}
			FPlatformAtomics::InterlockedExchange(&bHasPresented, 1);
			++DebugPresents;
			if (CVarMagicSRFgPresenterDebug.GetValueOnAnyThread() != 0 &&
				(DebugPresents <= 8 || (DebugPresents % 120) == 0))
			{
				UE_LOG(LogTemp, Display, TEXT("[MagicSR] Presenter present n=%d kind=%d slot=%d src=0x%llx"),
					   DebugPresents, Job.Kind, Job.Slot, Slots[Job.Slot].VkImage);
			}
		}
		FreeSlot(Job.Slot);
		{
			FScopeLock Lock(&JobCs);
			InFlightSlot = -1;
		}
	}

	if (WaitableTimer)
	{
		CloseHandle(static_cast<HANDLE>(WaitableTimer));
		WaitableTimer = nullptr;
	}
#endif
	return 0;
}

void FMagicSRFgPresenter::PreciseWaitUntil(double Target) const
{
	PreciseWaitUntil(Target, 0.022);
}

void FMagicSRFgPresenter::PreciseWaitUntil(double Target, double MaxWaitSec) const
{
	const double Now0 = FPlatformTime::Seconds();
	if (Target <= Now0)
	{
		return;
	}
	const double CapWait = FMath::Clamp(MaxWaitSec, 0.002, 0.050);
	const double Cap = Now0 + CapWait;
	if (Target > Cap)
	{
		Target = Cap;
	}
#if PLATFORM_WINDOWS
	HANDLE Timer = static_cast<HANDLE>(WaitableTimer);
#endif
	for (;;)
	{
		const double Now = FPlatformTime::Seconds();
		const double Remain = Target - Now;
		if (Remain <= 0.0004)
		{
			break;
		}
#if PLATFORM_WINDOWS
		if (Timer && Remain > 0.0015)
		{
			LARGE_INTEGER Due;
			Due.QuadPart = -static_cast<LONGLONG>((Remain - 0.0004) * 10000000.0);
			if (SetWaitableTimer(Timer, &Due, 0, nullptr, nullptr, FALSE))
			{
				WaitForSingleObject(Timer, 50);
				continue;
			}
		}
#endif
		FPlatformProcess::SleepNoStats(static_cast<float>(Remain - 0.0004));
	}
	while (FPlatformTime::Seconds() < Target)
	{
	}
}

#if PLATFORM_WINDOWS

void FMagicSRFgPresenter::EnsureOverlayWindow()
{
	HWND Owner = FindGameHwnd();
	if (!Owner || !IsWindow(Owner))
	{
		return;
	}
	OwnerHwnd = Owner;
	if (OverlayHwnd && IsWindow(static_cast<HWND>(OverlayHwnd)))
	{
		return;
	}
	RegisterPresenterClass();
	RECT Rc;
	GetClientRect(Owner, &Rc);
	const int32 W = FMath::Max(2, static_cast<int32>(Rc.right - Rc.left));
	const int32 H = FMath::Max(2, static_cast<int32>(Rc.bottom - Rc.top));
	HWND Overlay = CreateWindowExW(
		WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
		GPresenterClassName,
		L"MagicSR FG Presenter",
		WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
		0, 0, W, H,
		Owner,
		nullptr,
		reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr)),
		nullptr);
	if (!Overlay)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter CreateWindowEx failed err=%u"), GetLastError());
		return;
	}
	OverlayHwnd = Overlay;
	OverlayW = W;
	OverlayH = H;
	SetWindowPos(Overlay, HWND_TOP, 0, 0, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] Presenter overlay HWND=%p size=%dx%d owner=%p"), Overlay, W, H, Owner);
}

void FMagicSRFgPresenter::SyncOverlayWindow()
{
	HWND Owner = static_cast<HWND>(OwnerHwnd);
	HWND Overlay = static_cast<HWND>(OverlayHwnd);
	if (!Owner || !Overlay || !IsWindow(Owner) || !IsWindow(Overlay))
	{
		return;
	}
	if (IsIconic(Owner) || !IsWindowVisible(Owner))
	{
		ShowWindow(Overlay, SW_HIDE);
		return;
	}
	RECT Rc;
	GetClientRect(Owner, &Rc);
	const int32 W = FMath::Max(2, static_cast<int32>(Rc.right - Rc.left));
	const int32 H = FMath::Max(2, static_cast<int32>(Rc.bottom - Rc.top));
	if (W != OverlayW || H != OverlayH)
	{
		SetWindowPos(Overlay, HWND_TOP, 0, 0, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		OverlayW = W;
		OverlayH = H;
		FPlatformAtomics::InterlockedExchange(&RecreateFlag, 1);
	}
}

void FMagicSRFgPresenter::HideAndDestroyWindow()
{
	HWND Overlay = static_cast<HWND>(OverlayHwnd);
	if (Overlay && IsWindow(Overlay))
	{
		ShowWindow(Overlay, SW_HIDE);
		DestroyWindow(Overlay);
	}
	OverlayHwnd = nullptr;
	OwnerHwnd = nullptr;
	OverlayW = 0;
	OverlayH = 0;
}

void FMagicSRFgPresenter::SubmitOnQueue(TFunction<void(void*)>&& Fn)
{
	if (bExclusiveQueue && VkQueueHandle)
	{
		Fn(VkQueueHandle);
		return;
	}
	IVulkanDynamicRHI* RHI = GetIVulkanDynamicRHI();
	RHI->RHIRunOnQueue(
		EVulkanRHIRunOnQueueType::Graphics,
		[Fn = MoveTemp(Fn)](VkQueue Q)
		{
			Fn(Q);
		},
		true);
}

bool FMagicSRFgPresenter::EnsureVulkan(void* Hwnd)
{
	if (bVulkanReady)
	{
		return true;
	}
	if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::Vulkan)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter requires Vulkan RHI"));
		return false;
	}
	IVulkanDynamicRHI* RHI = GetIVulkanDynamicRHI();
	FMagicSRPresenterVk* Vk = new FMagicSRPresenterVk();
	if (!Vk->Api.LoadAll(RHI))
	{
		delete Vk;
		return false;
	}
	Vk->Instance = RHI->RHIGetVkInstance();
	Vk->Phys = RHI->RHIGetVkPhysicalDevice();
	Vk->Device = RHI->RHIGetVkDevice();
	QueueFamily = RHI->RHIGetGraphicsQueueFamilyIndex();
	uint32 FamCount = 0;
	Vk->Api.GetPhysicalDeviceQueueFamilyProperties(Vk->Phys, &FamCount, nullptr);
	TArray<VkQueueFamilyProperties> Fams;
	Fams.SetNum(FamCount);
	Vk->Api.GetPhysicalDeviceQueueFamilyProperties(Vk->Phys, &FamCount, Fams.GetData());
	const uint32 FamilyQueues = (QueueFamily < FamCount) ? Fams[QueueFamily].queueCount : 1;
	const int32 WantIndex = CVarMagicSRFgPresenterQueueIndex.GetValueOnAnyThread();
	QueueIndex = 0;
	bExclusiveQueue = false;
	if (WantIndex > 0 && FamilyQueues > static_cast<uint32>(WantIndex))
	{
		VkQueue Q = VK_NULL_HANDLE;
		Vk->Api.GetDeviceQueue(Vk->Device, QueueFamily, static_cast<uint32>(WantIndex), &Q);
		if (Q)
		{
			Vk->Queue = Q;
			QueueIndex = static_cast<uint32>(WantIndex);
			bExclusiveQueue = true;
		}
	}
	if (!bExclusiveQueue)
	{
		Vk->Queue = RHI->RHIGetGraphicsVkQueue();
		QueueIndex = RHI->RHIGetGraphicsQueueIndex();
		UE_LOG(LogTemp, Warning,
			   TEXT("[MagicSR] Presenter using RHIRunOnQueue (family queues=%u). Exclusive index %d unavailable."),
			   FamilyQueues, WantIndex);
	}

	Api = Vk;
	VkInstanceHandle = Vk->Instance;
	VkPhysHandle = Vk->Phys;
	VkDeviceHandle = Vk->Device;
	VkQueueHandle = Vk->Queue;

	if (!RecreateSwapchain(Hwnd, OverlayW, OverlayH))
	{
		DestroyVulkan();
		return false;
	}
	bVulkanReady = true;
	UE_LOG(LogTemp, Display,
		   TEXT("[MagicSR] Presenter Arch P D3D11 device=%p exclusive=%d family=%u index=%u familyQueues=%u size=%dx%d"),
		   Vk->Device, bExclusiveQueue ? 1 : 0, QueueFamily, QueueIndex, FamilyQueues, SwapW, SwapH);
	return true;
}

void FMagicSRFgPresenter::DestroyVulkan()
{
	FScopeLock Lock(&VkCs);
	DestroySwapchain();
	FMagicSRPresenterVk* Vk = reinterpret_cast<FMagicSRPresenterVk*>(Api);
	if (Vk)
	{
		if (Vk->Surface && Vk->Api.DestroySurfaceKHR && Vk->Instance)
		{
			Vk->Api.DestroySurfaceKHR(Vk->Instance, Vk->Surface, nullptr);
		}
		SafeReleaseCom(Vk->Ctx);
		SafeReleaseCom(Vk->D3D);
		delete Vk;
	}
	Api = nullptr;
	VkSurfaceHandle = nullptr;
	VkInstanceHandle = nullptr;
	VkPhysHandle = nullptr;
	VkDeviceHandle = nullptr;
	VkQueueHandle = nullptr;
	bVulkanReady = false;
}

void FMagicSRFgPresenter::DestroySwapchain()
{
	FMagicSRPresenterVk* Vk = reinterpret_cast<FMagicSRPresenterVk*>(Api);
	if (!Vk)
	{
		return;
	}
	if (Vk->Queue && Vk->Api.QueueWaitIdle)
	{
		if (bExclusiveQueue)
		{
			Vk->Api.QueueWaitIdle(Vk->Queue);
		}
		else
		{
			SubmitOnQueue(
				[&](void* QPtr)
				{
					Vk->Api.QueueWaitIdle(static_cast<VkQueue>(QPtr));
				});
		}
	}

	if (Vk->SharedImage)
	{
		Vk->Api.DestroyImage(Vk->Device, Vk->SharedImage, nullptr);
		Vk->SharedImage = VK_NULL_HANDLE;
	}
	if (Vk->SharedMemory)
	{
		Vk->Api.FreeMemory(Vk->Device, Vk->SharedMemory, nullptr);
		Vk->SharedMemory = VK_NULL_HANDLE;
	}
	if (Vk->SharedHandle && Vk->SharedHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT)
	{
		CloseHandle(Vk->SharedHandle);
	}
	Vk->SharedHandle = nullptr;
	SafeReleaseCom(Vk->DxgiBB);
	SafeReleaseCom(Vk->SharedTex);
	SafeReleaseCom(Vk->DxgiSwap);

	for (VkSemaphore S : Vk->Acquired)
	{
		if (S)
		{
			Vk->Api.DestroySemaphore(Vk->Device, S, nullptr);
		}
	}
	for (VkSemaphore S : Vk->Rendered)
	{
		if (S)
		{
			Vk->Api.DestroySemaphore(Vk->Device, S, nullptr);
		}
	}
	for (VkFence F : Vk->InFlight)
	{
		if (F)
		{
			Vk->Api.DestroyFence(Vk->Device, F, nullptr);
		}
	}
	for (VkImageView V : Vk->Views)
	{
		if (V)
		{
			Vk->Api.DestroyImageView(Vk->Device, V, nullptr);
		}
	}
	if (Vk->Cmds.Num() && Vk->CmdPool)
	{
		Vk->Api.FreeCommandBuffers(Vk->Device, Vk->CmdPool, Vk->Cmds.Num(), Vk->Cmds.GetData());
	}
	if (Vk->CmdPool)
	{
		Vk->Api.DestroyCommandPool(Vk->Device, Vk->CmdPool, nullptr);
	}
	if (Vk->Swapchain)
	{
		Vk->Api.DestroySwapchainKHR(Vk->Device, Vk->Swapchain, nullptr);
	}
	Vk->Acquired.Reset();
	Vk->Rendered.Reset();
	Vk->InFlight.Reset();
	Vk->Views.Reset();
	Vk->Cmds.Reset();
	Vk->Images.Reset();
	Vk->CmdPool = VK_NULL_HANDLE;
	Vk->Swapchain = VK_NULL_HANDLE;
	VkSwapchainHandle = nullptr;
	VkCommandPoolHandle = nullptr;
	Vk->bD3D11 = false;
	SwapW = 0;
	SwapH = 0;
	SwapImageCount = 0;
}

bool FMagicSRFgPresenter::RecreateSwapchain(void* Hwnd, int32 Width, int32 Height)
{
	FMagicSRPresenterVk* Vk = reinterpret_cast<FMagicSRPresenterVk*>(Api);
	if (!Vk || !Hwnd || Width < 2 || Height < 2)
	{
		return false;
	}
	DestroySwapchain();

	if (!Vk->D3D)
	{
		IVulkanDynamicRHI* RHI = GetIVulkanDynamicRHI();
		const uint64 AdapterLuid = RHI->RHIGetGraphicsAdapterLUID(Vk->Phys);
		IDXGIAdapter* Adapter = FindDxgiAdapterByLuid(AdapterLuid);
		const D3D_FEATURE_LEVEL Levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL GotLevel = D3D_FEATURE_LEVEL_11_0;
		const HRESULT DevHr = D3D11CreateDevice(
			Adapter,
			Adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			Levels,
			UE_ARRAY_COUNT(Levels),
			D3D11_SDK_VERSION,
			&Vk->D3D,
			&GotLevel,
			&Vk->Ctx);
		if (Adapter)
		{
			Adapter->Release();
		}
		if (FAILED(DevHr) || !Vk->D3D || !Vk->Ctx)
		{
			UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter D3D11CreateDevice hr=0x%08x"), DevHr);
			return false;
		}
		UE_LOG(LogTemp, Display, TEXT("[MagicSR] Presenter D3D11 device featureLevel=0x%x luid=0x%llx"),
			   static_cast<int32>(GotLevel), AdapterLuid);
		IDXGIDevice1* DxgiDev1 = nullptr;
		if (SUCCEEDED(Vk->D3D->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&DxgiDev1))) && DxgiDev1)
		{
			DxgiDev1->SetMaximumFrameLatency(1);
			DxgiDev1->Release();
		}
	}

	DXGI_SWAP_CHAIN_DESC Sd = {};
	Sd.BufferCount = 2;
	Sd.BufferDesc.Width = static_cast<UINT>(Width);
	Sd.BufferDesc.Height = static_cast<UINT>(Height);
	Sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	Sd.BufferDesc.RefreshRate.Numerator = 0;
	Sd.BufferDesc.RefreshRate.Denominator = 1;
	Sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	Sd.OutputWindow = static_cast<HWND>(Hwnd);
	Sd.SampleDesc.Count = 1;
	Sd.Windowed = TRUE;
	Sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	Sd.Flags = 0;

	IDXGIDevice* DxgiDev = nullptr;
	Vk->D3D->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDev));
	IDXGIAdapter* DxgiAd = nullptr;
	IDXGIFactory* DxgiFac = nullptr;
	if (DxgiDev)
	{
		DxgiDev->GetAdapter(&DxgiAd);
	}
	if (DxgiAd)
	{
		DxgiAd->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&DxgiFac));
	}
	HRESULT SwapHr = E_FAIL;
	if (DxgiFac)
	{
		SwapHr = DxgiFac->CreateSwapChain(Vk->D3D, &Sd, &Vk->DxgiSwap);
		DxgiFac->MakeWindowAssociation(static_cast<HWND>(Hwnd), DXGI_MWA_NO_ALT_ENTER);
	}
	SafeReleaseCom(DxgiFac);
	SafeReleaseCom(DxgiAd);
	SafeReleaseCom(DxgiDev);
	if (FAILED(SwapHr) || !Vk->DxgiSwap)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter CreateSwapChain hr=0x%08x"), SwapHr);
		return false;
	}
	if (FAILED(Vk->DxgiSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&Vk->DxgiBB))) || !Vk->DxgiBB)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter GetBuffer failed"));
		return false;
	}

	D3D11_TEXTURE2D_DESC Td = {};
	Td.Width = static_cast<UINT>(Width);
	Td.Height = static_cast<UINT>(Height);
	Td.MipLevels = 1;
	Td.ArraySize = 1;
	Td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	Td.SampleDesc.Count = 1;
	Td.Usage = D3D11_USAGE_DEFAULT;
	Td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	Td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	HRESULT TexHr = Vk->D3D->CreateTexture2D(&Td, nullptr, &Vk->SharedTex);
	Vk->SharedHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	if (FAILED(TexHr))
	{
		Td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
		TexHr = Vk->D3D->CreateTexture2D(&Td, nullptr, &Vk->SharedTex);
		Vk->SharedHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
	}
	if (FAILED(TexHr) || !Vk->SharedTex)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter CreateTexture2D shared hr=0x%08x"), TexHr);
		return false;
	}
	if (Vk->SharedHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT)
	{
		IDXGIResource1* Res1 = nullptr;
		Vk->SharedTex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&Res1));
		if (!Res1 || FAILED(Res1->CreateSharedHandle(
						 nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &Vk->SharedHandle)))
		{
			UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter CreateSharedHandle failed"));
			SafeReleaseCom(Res1);
			return false;
		}
		SafeReleaseCom(Res1);
	}
	else
	{
		IDXGIResource* Res = nullptr;
		Vk->SharedTex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&Res));
		if (!Res || FAILED(Res->GetSharedHandle(&Vk->SharedHandle)))
		{
			UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter GetSharedHandle failed"));
			SafeReleaseCom(Res);
			return false;
		}
		SafeReleaseCom(Res);
	}

	VkExternalMemoryImageCreateInfo ExtImg = {};
	ExtImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	ExtImg.handleTypes = Vk->SharedHandleType;
	VkImageCreateInfo ImgCi = {};
	ImgCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ImgCi.pNext = &ExtImg;
	ImgCi.imageType = VK_IMAGE_TYPE_2D;
	ImgCi.format = VK_FORMAT_B8G8R8A8_UNORM;
	ImgCi.extent.width = static_cast<uint32>(Width);
	ImgCi.extent.height = static_cast<uint32>(Height);
	ImgCi.extent.depth = 1;
	ImgCi.mipLevels = 1;
	ImgCi.arrayLayers = 1;
	ImgCi.samples = VK_SAMPLE_COUNT_1_BIT;
	ImgCi.tiling = VK_IMAGE_TILING_OPTIMAL;
	ImgCi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ImgCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ImgCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (Vk->Api.CreateImage(Vk->Device, &ImgCi, nullptr, &Vk->SharedImage) != VK_SUCCESS)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter vkCreateImage(shared) failed"));
		return false;
	}
	VkMemoryRequirements MemReq = {};
	Vk->Api.GetImageMemoryRequirements(Vk->Device, Vk->SharedImage, &MemReq);
	VkImportMemoryWin32HandleInfoKHR Import = {};
	Import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	Import.handleType = Vk->SharedHandleType;
	Import.handle = Vk->SharedHandle;
	VkMemoryDedicatedAllocateInfo Ded = {};
	Ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	Ded.pNext = &Import;
	Ded.image = Vk->SharedImage;
	VkMemoryAllocateInfo Alloc = {};
	Alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	Alloc.pNext = &Ded;
	Alloc.allocationSize = MemReq.size;
	Alloc.memoryTypeIndex =
		FindMemoryTypeIndex(Vk->Api, Vk->Phys, MemReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (Vk->Api.AllocateMemory(Vk->Device, &Alloc, nullptr, &Vk->SharedMemory) != VK_SUCCESS)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter vkAllocateMemory(import D3D11) failed handleType=%u"),
			   static_cast<uint32>(Vk->SharedHandleType));
		return false;
	}
	if (Vk->Api.BindImageMemory(Vk->Device, Vk->SharedImage, Vk->SharedMemory, 0) != VK_SUCCESS)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter vkBindImageMemory(shared) failed"));
		return false;
	}

	VkCommandPoolCreateInfo Pci = {};
	Pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	Pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	Pci.queueFamilyIndex = QueueFamily;
	if (Vk->Api.CreateCommandPool(Vk->Device, &Pci, nullptr, &Vk->CmdPool) != VK_SUCCESS)
	{
		UE_LOG(LogTemp, Error, TEXT("[MagicSR] Presenter vkCreateCommandPool failed"));
		return false;
	}
	Vk->Cmds.SetNum(1);
	VkCommandBufferAllocateInfo Ai = {};
	Ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	Ai.commandPool = Vk->CmdPool;
	Ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	Ai.commandBufferCount = 1;
	Vk->Api.AllocateCommandBuffers(Vk->Device, &Ai, Vk->Cmds.GetData());
	Vk->InFlight.SetNum(1);
	VkFenceCreateInfo Fi = {};
	Fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	Fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	Vk->Api.CreateFence(Vk->Device, &Fi, nullptr, &Vk->InFlight[0]);

	VkCommandPoolHandle = Vk->CmdPool;
	SwapW = Width;
	SwapH = Height;
	SwapImageCount = 2;
	SwapPresentMode = -1;
	Vk->bD3D11 = true;
	Vk->Frame = 0;
	UE_LOG(LogTemp, Display,
		   TEXT("[MagicSR] Presenter D3D11 DISCARD swapchain %dx%d sharedHandleType=%u (GDI-visible bitblt)"), SwapW,
		   SwapH, static_cast<uint32>(Vk->SharedHandleType));
	return true;
}

bool FMagicSRFgPresenter::AcquireAndBlit(
	uint64 SrcImage,
	int32 SrcW,
	int32 SrcH,
	bool bSolidColor,
	float Cr,
	float Cg,
	float Cb)
{
	FMagicSRPresenterVk* Vk = reinterpret_cast<FMagicSRPresenterVk*>(Api);
	if (!Vk || !Vk->bD3D11 || !Vk->DxgiSwap || !Vk->SharedImage || !Vk->Ctx || Vk->Cmds.Num() == 0)
	{
		return false;
	}
	FScopeLock Lock(&VkCs);
	const int32 Ablation = CVarMagicSRFgPresenterAblation.GetValueOnAnyThread();
	const bool bPerf = FMagicSRViewportBridge::IsPerfCollect();
	const double T0 = bPerf ? FPlatformTime::Seconds() : 0.0;
	if (Ablation == 3)
	{
		const HRESULT PresHr = Vk->DxgiSwap->Present(0, 0);
		if (FAILED(PresHr))
		{
			FPlatformAtomics::InterlockedExchange(&RecreateFlag, 1);
			return false;
		}
		if (bPerf)
		{
			UINT Count = 0;
			const bool bCount = SUCCEEDED(Vk->DxgiSwap->GetLastPresentCount(&Count));
			FScopeLock PerfLock(&PerfCs);
			PerfCopyMsSum += (FPlatformTime::Seconds() - T0) * 1000.0;
			++PerfCopySamples;
			if (bCount)
			{
				PerfDxgiCountNow = Count;
			}
		}
		return true;
	}
	Vk->Api.WaitForFences(Vk->Device, 1, &Vk->InFlight[0], VK_TRUE, 50ull * 1000ull * 1000ull);
	Vk->Api.ResetFences(Vk->Device, 1, &Vk->InFlight[0]);

	VkCommandBuffer Cmd = Vk->Cmds[0];
	Vk->Api.ResetCommandBuffer(Cmd, 0);
	VkCommandBufferBeginInfo Bi = {};
	Bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	Bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Vk->Api.BeginCommandBuffer(Cmd, &Bi);

	VkImage DstImg = Vk->SharedImage;
	VkImageMemoryBarrier ToDst = {};
	ToDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	ToDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	ToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ToDst.image = DstImg;
	ToDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ToDst.subresourceRange.levelCount = 1;
	ToDst.subresourceRange.layerCount = 1;
	ToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	Vk->Api.CmdPipelineBarrier(
		Cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ToDst);

	if (bSolidColor || SrcImage == 0)
	{
		VkClearColorValue Clear;
		Clear.float32[0] = Cr;
		Clear.float32[1] = Cg;
		Clear.float32[2] = Cb;
		Clear.float32[3] = 1.f;
		VkImageSubresourceRange Range = ToDst.subresourceRange;
		Vk->Api.CmdClearColorImage(Cmd, DstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &Clear, 1, &Range);
	}
	else
	{
		VkImage Src = reinterpret_cast<VkImage>(SrcImage);
		VkImageMemoryBarrier SrcBar = ToDst;
		SrcBar.image = Src;
		SrcBar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		SrcBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		SrcBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		SrcBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		Vk->Api.CmdPipelineBarrier(
			Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &SrcBar);

		const int32 CopyW = FMath::Min(FMath::Max(1, SrcW), SwapW);
		const int32 CopyH = FMath::Min(FMath::Max(1, SrcH), SwapH);
		if (SrcW == SwapW && SrcH == SwapH && Vk->Api.CmdCopyImage)
		{
			VkImageCopy Rgn = {};
			Rgn.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			Rgn.srcSubresource.layerCount = 1;
			Rgn.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			Rgn.dstSubresource.layerCount = 1;
			Rgn.extent.width = static_cast<uint32>(CopyW);
			Rgn.extent.height = static_cast<uint32>(CopyH);
			Rgn.extent.depth = 1;
			Vk->Api.CmdCopyImage(
				Cmd, Src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, DstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Rgn);
		}
		else
		{
			VkImageBlit Blit = {};
			Blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			Blit.srcSubresource.layerCount = 1;
			Blit.srcOffsets[1].x = FMath::Max(1, SrcW);
			Blit.srcOffsets[1].y = FMath::Max(1, SrcH);
			Blit.srcOffsets[1].z = 1;
			Blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			Blit.dstSubresource.layerCount = 1;
			Blit.dstOffsets[1].x = SwapW;
			Blit.dstOffsets[1].y = SwapH;
			Blit.dstOffsets[1].z = 1;
			Vk->Api.CmdBlitImage(
				Cmd, Src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, DstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Blit,
				VK_FILTER_NEAREST);
		}
	}

	VkImageMemoryBarrier ToGeneral = ToDst;
	ToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	ToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	ToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	ToGeneral.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	Vk->Api.CmdPipelineBarrier(
		Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
		&ToGeneral);
	Vk->Api.EndCommandBuffer(Cmd);

	VkSubmitInfo Si = {};
	Si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	Si.commandBufferCount = 1;
	Si.pCommandBuffers = &Cmd;

	VkResult SubRc = VK_ERROR_DEVICE_LOST;
	VkFence SubmitFence = Vk->InFlight[0];
	SubmitOnQueue(
		[&](void* QPtr)
		{
			VkQueue Q = static_cast<VkQueue>(QPtr);
			SubRc = Vk->Api.QueueSubmit(Q, 1, &Si, SubmitFence);
		});
	if (SubRc != VK_SUCCESS)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MagicSR] Presenter vkQueueSubmit rc=%d exclusive=%d"),
			   static_cast<int32>(SubRc), bExclusiveQueue ? 1 : 0);
		return false;
	}
	const double TBlit0 = bPerf ? FPlatformTime::Seconds() : 0.0;
	Vk->Api.WaitForFences(Vk->Device, 1, &SubmitFence, VK_TRUE, 50ull * 1000ull * 1000ull);
	const double TBlit1 = bPerf ? FPlatformTime::Seconds() : 0.0;
	if (Ablation >= 2)
	{
		if (bPerf)
		{
			FScopeLock PerfLock(&PerfCs);
			PerfCopyMsSum += (TBlit1 - T0) * 1000.0;
			PerfBlitWaitMsSum += (TBlit1 - TBlit0) * 1000.0;
			++PerfCopySamples;
		}
		return true;
	}

	const double TCopy0 = bPerf ? FPlatformTime::Seconds() : 0.0;
	Vk->Ctx->CopyResource(Vk->DxgiBB, Vk->SharedTex);
	const double TCopy1 = bPerf ? FPlatformTime::Seconds() : 0.0;
	HRESULT PresHr = S_OK;
	if (Ablation < 1)
	{
		PresHr = Vk->DxgiSwap->Present(0, 0);
	}
	const double TPres1 = bPerf ? FPlatformTime::Seconds() : 0.0;
	if (FAILED(PresHr))
	{
		UE_LOG(LogTemp, Warning, TEXT("[MagicSR] Presenter DXGI Present hr=0x%08x"), PresHr);
		FPlatformAtomics::InterlockedExchange(&RecreateFlag, 1);
		return false;
	}
	Vk->Frame++;
	if (bPerf)
	{
		UINT Count = 0;
		const bool bCount = (Ablation < 1) && SUCCEEDED(Vk->DxgiSwap->GetLastPresentCount(&Count));
		FScopeLock PerfLock(&PerfCs);
		PerfCopyMsSum += (TPres1 - T0) * 1000.0;
		PerfBlitWaitMsSum += (TBlit1 - TBlit0) * 1000.0;
		PerfD3dCopyMsSum += (TCopy1 - TCopy0) * 1000.0;
		PerfDxgiPresentMsSum += (TPres1 - TCopy1) * 1000.0;
		++PerfCopySamples;
		if (bCount)
		{
			PerfDxgiCountNow = Count;
		}
	}
	return true;
}

bool FMagicSRFgPresenter::PresentSlot(int32 SlotIndex)
{
	if (SlotIndex < 0 || SlotIndex >= kSlotCount)
	{
		return false;
	}
	const FSlot& Slot = Slots[SlotIndex];
	if (Slot.VkImage == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MagicSR] Presenter slot %d has null VkImage"), SlotIndex);
		return false;
	}
	return AcquireAndBlit(Slot.VkImage, Slot.Width, Slot.Height, false, 0, 0, 0);
}

void FMagicSRFgPresenter::PresentColor(float R, float G, float B, int32 Width, int32 Height)
{
	(void)Width;
	(void)Height;
	if (AcquireAndBlit(0, 0, 0, true, R, G, B))
	{
		FMagicSRViewportBridge::Get().NotifyPresenterPresent(2);
		FPlatformAtomics::InterlockedExchange(&bHasPresented, 1);
	}
}

#else // !PLATFORM_WINDOWS

void FMagicSRFgPresenter::EnsureOverlayWindow() {}
void FMagicSRFgPresenter::SyncOverlayWindow() {}
void FMagicSRFgPresenter::HideAndDestroyWindow() {}
bool FMagicSRFgPresenter::EnsureVulkan(void*) { return false; }
void FMagicSRFgPresenter::DestroyVulkan() {}
bool FMagicSRFgPresenter::RecreateSwapchain(void*, int32, int32) { return false; }
void FMagicSRFgPresenter::DestroySwapchain() {}
bool FMagicSRFgPresenter::PresentSlot(int32) { return false; }
void FMagicSRFgPresenter::PresentColor(float, float, float, int32, int32) {}
bool FMagicSRFgPresenter::AcquireAndBlit(uint64, int32, int32, bool, float, float, float) { return false; }
void FMagicSRFgPresenter::SubmitOnQueue(TFunction<void(void*)>&&) {}

#endif

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
