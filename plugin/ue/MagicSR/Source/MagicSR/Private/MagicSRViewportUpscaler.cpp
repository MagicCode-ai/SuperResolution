#include "MagicSRViewportUpscaler.h"

#include "MagicSRCaptureRig.h"
#include "MagicSRRHI.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "TextureResource.h"

#if PLATFORM_WINDOWS || PLATFORM_LINUX
#include "IVulkanDynamicRHI.h"
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

AMagicSRViewportUpscaler::AMagicSRViewportUpscaler()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

void AMagicSRViewportUpscaler::BeginPlay()
{
    Super::BeginPlay();
    EnsureOutputTargets();
    RecreateHandle();
}

void AMagicSRViewportUpscaler::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    DestroySession();
    Super::EndPlay(EndPlayReason);
}

void AMagicSRViewportUpscaler::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (CompareMode == EMagicSRCompareMode::Off)
    {
        // Keep right panel as a true "SR off" bilinear 2× reference (not a stale SR frame).
        UpsampleBilinearReference();
        LastProcessResult = 0;
        return;
    }
    if (CompareMode == EMagicSRCompareMode::Native ||
        CompareMode == EMagicSRCompareMode::NativeNoAA ||
        CompareMode == EMagicSRCompareMode::UETSR50)
    {
        // Viewport renders itself. NativeNoAA / UETSR50 cvars are applied by the
        // validate subsystem. None of these create a SceneCapture library session.
        LastProcessResult = 0;
        return;
    }
    ProcessFrame(DeltaSeconds);
}

void AMagicSRViewportUpscaler::SetCompareMode(EMagicSRCompareMode Mode)
{
    if (CompareMode == Mode && !bCreateFailed)
    {
        return;
    }
    CompareMode = Mode;
    bCreateFailed = false;
    RecreateHandle();
}

void AMagicSRViewportUpscaler::CycleCompareMode()
{
    const int32 Next = (static_cast<int32>(CompareMode) + 1) % 6;
    SetCompareMode(static_cast<EMagicSRCompareMode>(Next));
    if (GEngine)
    {
        const TCHAR* Name =
            CompareMode == EMagicSRCompareMode::Off         ? TEXT("Off")
            : CompareMode == EMagicSRCompareMode::Spatial   ? TEXT("Spatial")
            : CompareMode == EMagicSRCompareMode::Temporal  ? TEXT("Temporal")
            : CompareMode == EMagicSRCompareMode::Native    ? TEXT("Native")
            : CompareMode == EMagicSRCompareMode::NativeNoAA ? TEXT("NativeNoAA")
                                                            : TEXT("UETSR50");
        GEngine->AddOnScreenDebugMessage(
            0x4D535200,
            2.0f,
            FColor::Green,
            FString::Printf(TEXT("MagicSR mode=%s rc=%d gpu=%.3fms"), Name, LastProcessResult, LastStatus.GpuTimeMs));
    }
}

int32 AMagicSRViewportUpscaler::ResolveBackend() const
{
    if (BackendOverride != 0)
    {
        return BackendOverride;
    }
    const EMagicSRRHIKind Kind = MagicSRRHI::GetKind();
#if PLATFORM_WINDOWS
    if (Kind == EMagicSRRHIKind::Vulkan)
    {
        return 6;
    }
    if (Kind == EMagicSRRHIKind::D3D11)
    {
        return 7;
    }
    // D3D12 has no MagicSR backend — caller must run with -d3d11 / DX11 RHI.
    UE_LOG(LogTemp, Error, TEXT("[MagicSR] unsupported RHI for GPU SR (need D3D11 or Vulkan), kind=%d"),
           static_cast<int32>(Kind));
    return 0;
#elif PLATFORM_LINUX
    if (Kind == EMagicSRRHIKind::Vulkan)
    {
        return 6;
    }
    return 1; // CPU spatial fallback
#else
    return 1;
#endif
}

void AMagicSRViewportUpscaler::EnsureOutputTargets()
{
    const int32 OutW = FMath::Max(1, FMath::RoundToInt(InputWidth * Scale));
    const int32 OutH = FMath::Max(1, FMath::RoundToInt(InputHeight * Scale));

    auto MakeRT = [](UObject* Outer, const TCHAR* Name, int32 W, int32 H, EPixelFormat Format,
                     bool bUAV) -> UTextureRenderTarget2D*
    {
        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(Outer, Name);
        RT->RenderTargetFormat = RTF_RGBA8;
        // CreateResource() copies bSupportsUAV → bCanCreateUAV (UAV create flag).
        RT->bSupportsUAV = bUAV;
        RT->bCanCreateUAV = bUAV;
        RT->ClearColor = FLinearColor::Black;
        RT->InitCustomFormat(W, H, Format, false);
        RT->UpdateResourceImmediate(true);
        return RT;
    };

    if (ColorOutRT == nullptr || ColorOutRT->SizeX != OutW || ColorOutRT->SizeY != OutH ||
        ColorOutRT->GetFormat() != PF_R8G8B8A8 || !ColorOutRT->bSupportsUAV)
    {
        // Vulkan temporal/spatial writes via storage images — UAV usage required.
        ColorOutRT = MakeRT(this, TEXT("MagicSR_ColorOut"), OutW, OutH, PF_R8G8B8A8, true);
    }
    if (CompareRT == nullptr || CompareRT->SizeX != OutW * 2 || CompareRT->SizeY != OutH)
    {
        CompareRT = MakeRT(this, TEXT("MagicSR_Compare"), OutW * 2, OutH, PF_R8G8B8A8, false);
    }
    if (ColorInRT == nullptr)
    {
        ColorInRT = MakeRT(this, TEXT("MagicSR_ColorIn"), InputWidth, InputHeight, PF_R8G8B8A8, false);
    }
    if (DepthRT == nullptr)
    {
        DepthRT = MakeRT(this, TEXT("MagicSR_Depth"), InputWidth, InputHeight, PF_R32_FLOAT, false);
    }
    if (MotionRT == nullptr)
    {
        MotionRT = NewObject<UTextureRenderTarget2D>(this, TEXT("MagicSR_Motion"));
        MotionRT->RenderTargetFormat = RTF_RGBA16f;
        MotionRT->InitCustomFormat(InputWidth, InputHeight, PF_G16R16F, false);
        MotionRT->UpdateResourceImmediate(true);
    }
}

void AMagicSRViewportUpscaler::DestroySession()
{
    if (SessionId >= 0)
    {
        UMagicSRBlueprintLibrary::DestroyHandle(SessionId);
        SessionId = -1;
    }
    ActiveMode = EMagicSRCompareMode::Off;
    // Keep bCreateFailed so failed modes do not instantly retry after Destroy.
}

void AMagicSRViewportUpscaler::RecreateHandle()
{
    DestroySession();
    EnsureOutputTargets();
    if (CompareMode == EMagicSRCompareMode::Off ||
        CompareMode == EMagicSRCompareMode::Native ||
        CompareMode == EMagicSRCompareMode::NativeNoAA ||
        CompareMode == EMagicSRCompareMode::UETSR50)
    {
        // Off = bilinear reference only. Native / NativeNoAA / UETSR50 = viewport renders itself.
        // None of these need a SceneCapture library session (avoids bogus mode=3 / -202001).
        return;
    }
    // Temporal runs on the player viewport via ITemporalUpscaler (FMagicSRViewportBridge).
    // SceneCapture Temporal is retired for quality (needs real MV + engine jitter).
    if (CompareMode == EMagicSRCompareMode::Temporal)
    {
        ActiveMode = CompareMode;
        return;
    }

    FMagicSRCreateParams Params;
    Params.ModelPath = UMagicSRBlueprintLibrary::GetBundledModelPath(true);
    Params.Scale = Scale;
    Params.InputType = 1; // TextureRgb8Unorm
    Params.Backend = ResolveBackend();
    if (Params.Backend == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[MagicSR] abort CreateHandle: no compatible GPU backend (use -vulkan or -d3d11)"));
        SessionId = -1;
        ActiveMode = CompareMode;
        return;
    }
    {
        Params.AlgMode = SpatialAlgMode;
        // Make Spatial A/B visibly sharper than bilinear Off reference.
        Params.SpatialSharpenLevel = 3;
    }

    FlushRenderingCommands();
    // Vulkan: import UE's VkDevice for both Temporal and Spatial. Library-owned
    // device (all-zero gpu_context) fails alloc (-202001) in this Editor process.
    const bool bNeedVulkanImport =
        Params.Backend == 6 && CompareMode != EMagicSRCompareMode::Off;
    if (bNeedVulkanImport)
    {
        int64 Phys = 0, Dev = 0, Inst = 0, Gipa = 0, Gdpa = 0;
        ENQUEUE_RENDER_COMMAND(MagicSRFetchVulkanDevice)(
            [&Phys, &Dev, &Inst, &Gipa, &Gdpa](FRHICommandListImmediate&)
            {
                if (GDynamicRHI == nullptr ||
                    GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::Vulkan)
                {
                    return;
                }
#if PLATFORM_WINDOWS || PLATFORM_LINUX
                IVulkanDynamicRHI* VulkanRHI = GetIVulkanDynamicRHI();
                Phys = reinterpret_cast<int64>(VulkanRHI->RHIGetVkPhysicalDevice());
                Dev = reinterpret_cast<int64>(VulkanRHI->RHIGetVkDevice());
                Inst = reinterpret_cast<int64>(VulkanRHI->RHIGetVkInstance());
                Gipa = reinterpret_cast<int64>(VulkanRHI->RHIGetVkInstanceGlobalProcAddr("vkGetInstanceProcAddr"));
                if (Gipa == 0)
                {
                    Gipa = reinterpret_cast<int64>(VulkanRHI->RHIGetVkInstanceProcAddr("vkGetInstanceProcAddr"));
                }
                Gdpa = reinterpret_cast<int64>(VulkanRHI->RHIGetVkDeviceProcAddr("vkGetDeviceProcAddr"));
#endif
            });
        FlushRenderingCommands();
        Params.GpuPhysicalDevice = Phys;
        Params.GpuDevice = Dev;
        Params.GpuNativeContext = Inst;
        Params.GpuDeviceContext = Gipa;
        Params.GpuGetDeviceProcAddr = Gdpa;
        UE_LOG(LogTemp,
               Display,
               TEXT("[MagicSR] Vulkan import phys=0x%llx dev=0x%llx inst=0x%llx gipa=0x%llx gdpa=0x%llx"),
               static_cast<unsigned long long>(Phys),
               static_cast<unsigned long long>(Dev),
               static_cast<unsigned long long>(Inst),
               static_cast<unsigned long long>(Gipa),
               static_cast<unsigned long long>(Gdpa));
    }
#if PLATFORM_WINDOWS
    else if (Params.Backend == 7) // D3D11
    {
        int64 DevicePtr = 0;
        ENQUEUE_RENDER_COMMAND(MagicSRFetchD3D11Device)(
            [&DevicePtr](FRHICommandListImmediate&)
            {
                if (GDynamicRHI == nullptr)
                {
                    return;
                }
                DevicePtr = reinterpret_cast<int64>(GDynamicRHI->RHIGetNativeDevice());
            });
        FlushRenderingCommands();
        Params.GpuDevice = DevicePtr;
        Params.GpuDeviceContext = 0;
    }
#endif

    if (!UMagicSRBlueprintLibrary::CreateHandle(Params, SessionId))
    {
        UE_LOG(LogTemp, Error, TEXT("[MagicSR] CreateHandle failed mode=%d backend=%d model=%s device=0x%llx"),
               static_cast<int32>(CompareMode),
               Params.Backend,
               *Params.ModelPath,
               static_cast<unsigned long long>(Params.GpuDevice));
        SessionId = -1;
        // Latch failure so ProcessFrame does not recreate every tick.
        ActiveMode = CompareMode;
        bCreateFailed = true;
        return;
    }
    ActiveMode = CompareMode;
    bCreateFailed = false;
    FrameIndex = 0;
    UE_LOG(LogTemp, Display, TEXT("[MagicSR] handle ready session=%d mode=%d backend=%d ver=%s model=%s"),
           SessionId,
           static_cast<int32>(CompareMode),
           Params.Backend,
           *UMagicSRBlueprintLibrary::GetVersion(),
           *Params.ModelPath);
}

void AMagicSRViewportUpscaler::ProcessFrame(float DeltaSeconds)
{
    if (CompareMode == EMagicSRCompareMode::Off ||
        CompareMode == EMagicSRCompareMode::Native ||
        CompareMode == EMagicSRCompareMode::NativeNoAA ||
        CompareMode == EMagicSRCompareMode::UETSR50)
    {
        return;
    }
    if (bCreateFailed)
    {
        return;
    }
    if (SessionId < 0 || ActiveMode != CompareMode)
    {
        RecreateHandle();
        if (SessionId < 0)
        {
            return;
        }
    }
    if (ColorInRT == nullptr || ColorOutRT == nullptr)
    {
        return;
    }

    FMagicSRTemporalFrameParams Temporal;
    Temporal.FrameTimeDeltaMs = FMath::Max(1.0f, DeltaSeconds * 1000.0f);
    Temporal.FrameIndex = FrameIndex++;
    Temporal.ResetHistory = (FrameIndex <= 1) || bInjectedCameraCut;
    // Must match CaptureRig jittered projection (infinite reversed-Z → far=0).
    Temporal.CameraNear = GNearClippingPlane > 0.0f ? GNearClippingPlane : 10.0f;
    Temporal.CameraFar = 0.0f;
    // SceneCapture FOVAngle is horizontal degrees; MagicSR wants vertical radians.
    {
        const float Aspect = FMath::Max(
            0.1f,
            static_cast<float>(ColorInRT ? ColorInRT->SizeX : InputWidth) /
                static_cast<float>(ColorInRT ? ColorInRT->SizeY : InputHeight));
        const float FovXRad =
            FMath::DegreesToRadians(InjectedCameraFovYDegrees > 1.0f ? InjectedCameraFovYDegrees : 90.0f);
        Temporal.CameraFovY = 2.0f * FMath::Atan(FMath::Tan(FovXRad * 0.5f) / Aspect);
    }
    // Pixel-space MVs (CaptureRig clears to 0).
    Temporal.MotionVectorScaleX = 1.0f;
    Temporal.MotionVectorScaleY = 1.0f;
    // Sharpening after broken history makes stair-steps worse; keep off for AA check.
    Temporal.EnableSharpening = false;
    Temporal.Sharpness = 0.0f;

    if (bUseInjectedJitter)
    {
        // Must match the Halton offsets baked into ColorIn via CaptureRig projection.
        Temporal.JitterOffsetX = InjectedJitterPixels.X;
        Temporal.JitterOffsetY = InjectedJitterPixels.Y;
    }
    else
    {
        if (IConsoleVariable* JitterX =
                IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAA.CurrentFrameJitter.X")))
        {
            Temporal.JitterOffsetX = JitterX->GetFloat();
        }
        if (IConsoleVariable* JitterY =
                IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAA.CurrentFrameJitter.Y")))
        {
            Temporal.JitterOffsetY = JitterY->GetFloat();
        }
    }
    LastJitter = FVector2D(Temporal.JitterOffsetX, Temporal.JitterOffsetY);

    const int32 UseW = ColorInRT->SizeX > 0 ? ColorInRT->SizeX : InputWidth;
    const int32 UseH = ColorInRT->SizeY > 0 ? ColorInRT->SizeY : InputHeight;

    if (CompareMode == EMagicSRCompareMode::Spatial)
    {
        LastProcessResult = UMagicSRBlueprintLibrary::ProcessSpatial(
            SessionId, ColorInRT, ColorOutRT, UseW, UseH);
    }
    else
    {
        if (DepthRT == nullptr || MotionRT == nullptr)
        {
            LastProcessResult = -3011;
            return;
        }
        // Temporal contract: device depth in [0,1]. SceneDepth is view-space; use DeviceDepth when available.
        LastProcessResult = UMagicSRBlueprintLibrary::ProcessTemporal(
            SessionId, ColorInRT, DepthRT, MotionRT, ColorOutRT, UseW, UseH, Temporal);
    }

    UMagicSRBlueprintLibrary::QueryStatus(SessionId, LastStatus);
    if (FrameIndex <= 10 || (FrameIndex % 120) == 0)
    {
        UE_LOG(LogTemp,
               Display,
               TEXT("[MagicSR] process rc=%d mode=%d frame=%d out=%dx%d jitter=(%.3f,%.3f) cut=%d err=%lld"),
               LastProcessResult,
               static_cast<int32>(CompareMode),
               FrameIndex,
               LastStatus.OutputWidth,
               LastStatus.OutputHeight,
               LastJitter.X,
               LastJitter.Y,
               bInjectedCameraCut ? 1 : 0,
               LastStatus.ErrorCode);
    }
    BuildCompareComposite();

    // Advance capture jitter only after Process+Flush so ColorIn metadata stays matched.
    if (CompareMode == EMagicSRCompareMode::Temporal && CaptureRig)
    {
        CaptureRig->bEnableTemporalJitter = true;
        CaptureRig->ProgramNextJitter();
    }
}

void AMagicSRViewportUpscaler::BuildCompareComposite()
{
    // Side-by-side uses ColorOutRT as the primary visual (right/full SR result).
    // Avoid RHI CopyTexture across mismatched TYPELESS/UNORM or size pairs that
    // can GPU-crash on D3D11; HUD / Blueprints can sample ColorInRT vs ColorOutRT.
    CompareRT = ColorOutRT;
}

void AMagicSRViewportUpscaler::UpsampleBilinearReference()
{
    EnsureOutputTargets();
    if (ColorInRT == nullptr || ColorOutRT == nullptr || ColorInRT->GetResource() == nullptr)
    {
        return;
    }
    FTextureRenderTargetResource* OutRes = ColorOutRT->GameThread_GetRenderTargetResource();
    if (OutRes == nullptr)
    {
        return;
    }

    UWorld* World = GetWorld();
    const ERHIFeatureLevel::Type FeatureLevel =
        World ? World->GetFeatureLevel() : GMaxRHIFeatureLevel;
    FCanvas Canvas(OutRes, nullptr, World, FeatureLevel);
    Canvas.Clear(FLinearColor::Black);
    FCanvasTileItem Tile(
        FVector2D::ZeroVector,
        ColorInRT->GetResource(),
        FVector2D(static_cast<float>(ColorOutRT->SizeX), static_cast<float>(ColorOutRT->SizeY)),
        FLinearColor::White);
    Tile.BlendMode = SE_BLEND_Opaque;
    Canvas.DrawItem(Tile);
    Canvas.Flush_GameThread(true);
}
