#include "MagicSRBlueprintLibrary.h"
#include "MagicSRReactiveMarker.h"
#include "MagicSRRHI.h"
#include "MagicSRD3D11Bridge.h"

#include "Containers/Map.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "RHICommandList.h"
#include "RHITransition.h"
#include "RenderingThread.h"
#include "TextureResource.h"

#if PLATFORM_WINDOWS || PLATFORM_LINUX
#include "IVulkanDynamicRHI.h"
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
ID3D11Texture2D* GD3D11SpatialIn = nullptr;
ID3D11Texture2D* GD3D11SpatialOut = nullptr;
}
#endif

#if MAGIC_SR_SUPPORTED
extern "C" {
#include "mc_interface.h"
}
#endif

namespace
{
FCriticalSection GSessionMutex;
TMap<int32, void*> GSessions;
int32 GNextSessionId = 1;

#if MAGIC_SR_SUPPORTED
void* FindHandle(int32 SessionId)
{
    FScopeLock Lock(&GSessionMutex);
    void** Found = GSessions.Find(SessionId);
    return (Found && *Found) ? *Found : nullptr;
}

void StoreHandle(int32 SessionId, void* Handle)
{
    FScopeLock Lock(&GSessionMutex);
    if (void** Found = GSessions.Find(SessionId))
    {
        *Found = Handle;
    }
}

int32 EnableAndStore(int32 SessionId, void*& Handle, mc_nscaler_input_frame_t* In, mc_nscaler_output_frame_t* Out)
{
    mc_nscaler_input_frame_t* Frames[2] = { In, nullptr };
    const int32 Rc = mc_nscaler_enable(&Handle, Frames, Out);
    // enable/control may replace *handle; keep session map in sync.
    StoreHandle(SessionId, Handle);
    return Rc;
}

void FillStatus(const output_status_params_t& Status, FMagicSRStatus& Out)
{
    Out.Width = static_cast<int32>(Status.width);
    Out.Height = static_cast<int32>(Status.height);
    Out.OutputWidth = static_cast<int32>(Status.output_width);
    Out.OutputHeight = static_cast<int32>(Status.output_height);
    Out.ScalerFactor = Status.scaler_factor;
    Out.AlgMode = static_cast<int32>(Status.alg_mode);
    Out.Backend = static_cast<int32>(Status.backend);
    Out.NumThreads = static_cast<int32>(Status.num_threads);
    Out.GpuTimeMs = static_cast<float>(Status.gpu_time);
    Out.ErrorCode = static_cast<int64>(Status.error_code);
    Out.TemporalStatusValid = static_cast<int32>(Status.temporal_status_valid);
    Out.TemporalFrameIndex = static_cast<int32>(Status.temporal_frame_index);
    Out.TemporalJitterX = Status.temporal_jitter_offset_x;
    Out.TemporalJitterY = Status.temporal_jitter_offset_y;
}

bool ResolveUTextureResources(
    UTexture* Texture,
    FTextureRenderTargetResource*& OutRT,
    FTextureResource*& OutTex)
{
    OutRT = nullptr;
    OutTex = nullptr;
    if (Texture == nullptr)
    {
        return false;
    }
    if (UTextureRenderTarget2D* RT = Cast<UTextureRenderTarget2D>(Texture))
    {
        RT->UpdateResourceImmediate(false);
        OutRT = RT->GameThread_GetRenderTargetResource();
        return OutRT != nullptr;
    }
    OutTex = Texture->GetResource();
    return OutTex != nullptr;
}

void ApplyNativeToInFrame(const FMagicSRNativeResource& Native, mc_nscaler_input_frame_t& In)
{
    MagicSRRHI::FillDataHandle(Native, In.handle);
    In.format = Native.Format;
    In.layout = Native.Layout;
    In.mip_count = 1;
}

void ApplyNativeToOutFrame(const FMagicSRNativeResource& Native, mc_nscaler_output_frame_t& Out)
{
    MagicSRRHI::FillDataHandle(Native, Out.handle);
    Out.format = Native.Format;
    Out.layout = Native.Layout;
    Out.mip_count = 1;
}

void FillTemporalExtras(const FMagicSRTemporalFrameParams& Temporal, temporal_frame_t& Tf)
{
    FMemory::Memzero(&Tf, sizeof(Tf));
    Tf.struct_size = sizeof(Tf);
    Tf.jitter_offset_x = Temporal.JitterOffsetX;
    Tf.jitter_offset_y = Temporal.JitterOffsetY;
    Tf.motion_vector_scale_x = Temporal.MotionVectorScaleX;
    Tf.motion_vector_scale_y = Temporal.MotionVectorScaleY;
    Tf.frame_time_delta_ms = Temporal.FrameTimeDeltaMs;
    Tf.frame_index = static_cast<unsigned int>(FMath::Max(0, Temporal.FrameIndex));
    Tf.reset_history = Temporal.ResetHistory ? 1 : 0;
    Tf.camera_near = Temporal.CameraNear;
    Tf.camera_far = Temporal.CameraFar;
    Tf.camera_fov_y = Temporal.CameraFovY;
    Tf.view_to_meters = Temporal.ViewToMeters;
    Tf.enable_sharpening = Temporal.EnableSharpening ? 1u : 0u;
    Tf.sharpness = Temporal.Sharpness;
    Tf.mv_jitter = static_cast<uint32_t>(Temporal.MvJitter);
}
#endif
} // namespace

FString UMagicSRBlueprintLibrary::GetVersion()
{
#if MAGIC_SR_SUPPORTED
    const char* Version = mc_nscaler_version();
    return Version ? UTF8_TO_TCHAR(Version) : TEXT("unknown");
#else
    return TEXT("unsupported");
#endif
}

FString UMagicSRBlueprintLibrary::GetBundledModelPath(bool bGpuModel)
{
    const FString FileName = bGpuModel ? TEXT("magic_sr_gpu_params.bin") : TEXT("magic_sr_cpu_params.bin");
    TArray<FString> Candidates;
    Candidates.Add(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("MagicSRModels"), FileName));
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MagicSR")))
    {
        Candidates.Add(FPaths::Combine(Plugin->GetBaseDir(), TEXT("ThirdParty/MagicSR/models"), FileName));
    }
    Candidates.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/MagicSR/ThirdParty/MagicSR/models"), FileName));
    for (const FString& Path : Candidates)
    {
        if (FPaths::FileExists(Path))
        {
            return Path;
        }
    }
    return Candidates.Num() > 0 ? Candidates[0] : FString();
}

void UMagicSRBlueprintLibrary::SetMagicSRReactiveMarker(UPrimitiveComponent* Component, bool bMarked)
{
    MagicSRReactive::SetMarker(Component, bMarked);
}

bool UMagicSRBlueprintLibrary::CreateHandle(const FMagicSRCreateParams& Params, int32& OutSessionId)
{
    OutSessionId = -1;
#if MAGIC_SR_SUPPORTED
    ctrl_param_t Ctrl;
    FMemory::Memzero(&Ctrl, sizeof(Ctrl));
    Ctrl.input_type = static_cast<input_type_e>(FMath::Clamp(Params.InputType, 0, static_cast<int32>(MAX_INPUT_TYPE) - 1));
    Ctrl.scaler_factor = Params.Scale <= 0.0f ? 2.0f : Params.Scale;
    Ctrl.alg_mode = static_cast<alg_mode_e>(Params.AlgMode);
    Ctrl.backend = static_cast<magic_backend_e>(Params.Backend);
    Ctrl.log_level = MAGIC_LOG_ERROR;
    Ctrl.spatial_sharpen_level = static_cast<unsigned int>(FMath::Clamp(Params.SpatialSharpenLevel, 0, 5));
    Ctrl.depth_reversed = static_cast<uint32_t>(Params.DepthReversed);
    Ctrl.depth_infinite = static_cast<uint32_t>(Params.DepthInfinite);
    Ctrl.hdr_color = static_cast<uint32_t>(Params.HdrColor);
    Ctrl.gpu_context.physical_device = reinterpret_cast<void*>(static_cast<uintptr_t>(Params.GpuPhysicalDevice));
    Ctrl.gpu_context.device = reinterpret_cast<void*>(static_cast<uintptr_t>(Params.GpuDevice));
    Ctrl.gpu_context.device_context = reinterpret_cast<void*>(static_cast<uintptr_t>(Params.GpuDeviceContext));
    Ctrl.gpu_context.native_context = reinterpret_cast<void*>(static_cast<uintptr_t>(Params.GpuNativeContext));
    Ctrl.gpu_context.get_device_proc_addr = reinterpret_cast<void*>(static_cast<uintptr_t>(Params.GpuGetDeviceProcAddr));

    FString ModelPath = Params.ModelPath;
    if (ModelPath.IsEmpty())
    {
        const int32 SrBits = Params.AlgMode & 0x3;
        const bool bTemporalOrGpu = SrBits >= 2 || Params.AlgMode >= 2 || Params.Backend == 6 || Params.Backend == 7 || Params.Backend == 4;
        ModelPath = GetBundledModelPath(bTemporalOrGpu);
    }
    {
        FTCHARToUTF8 Utf8(*ModelPath);
        FCStringAnsi::Strncpy(Ctrl.model_path, Utf8.Get(), sizeof(Ctrl.model_path) - 1);
    }

    void* Handle = nullptr;
    const int Rc = mc_nscaler_control(&Handle, MC_NSCALER_CMD_SET_PARAM, &Ctrl, nullptr);
    if (Rc != 0 || Handle == nullptr)
    {
        UE_LOG(LogTemp,
               Error,
               TEXT("[MagicSR] CreateHandle mc_nscaler_control rc=%d backend=%d alg=%d device=0x%llx phys=0x%llx gipa=0x%llx gdpa=0x%llx model=%s"),
               Rc,
               Params.Backend,
               Params.AlgMode,
               static_cast<unsigned long long>(Params.GpuDevice),
               static_cast<unsigned long long>(Params.GpuPhysicalDevice),
               static_cast<unsigned long long>(Params.GpuDeviceContext),
               static_cast<unsigned long long>(Params.GpuGetDeviceProcAddr),
               *ModelPath);
        return false;
    }

    FScopeLock Lock(&GSessionMutex);
    OutSessionId = GNextSessionId++;
    GSessions.Add(OutSessionId, Handle);
    return true;
#else
    return false;
#endif
}

int32 UMagicSRBlueprintLibrary::DestroyHandle(int32 SessionId)
{
#if MAGIC_SR_SUPPORTED
    void* Handle = nullptr;
    {
        FScopeLock Lock(&GSessionMutex);
        void** Found = GSessions.Find(SessionId);
        if (Found == nullptr || *Found == nullptr)
        {
            return 0;
        }
        Handle = *Found;
        GSessions.Remove(SessionId);
    }
    return mc_nscaler_disable(Handle);
#else
    return -3999;
#endif
}

int32 UMagicSRBlueprintLibrary::ProcessSpatialNative(
    int32 SessionId,
    int64 ColorIn,
    int64 ColorOut,
    int32 InputWidth,
    int32 InputHeight,
    int32 ColorFormat,
    int32 OutputFormat,
    int32 ColorLayout,
    int32 OutputLayout)
{
#if MAGIC_SR_SUPPORTED
    void* Handle = FindHandle(SessionId);
    if (Handle == nullptr || ColorIn == 0 || ColorOut == 0 || InputWidth <= 0 || InputHeight <= 0)
    {
        return -3001;
    }

    mc_nscaler_input_frame_t In;
    mc_nscaler_output_frame_t Out;
    FMemory::Memzero(&In, sizeof(In));
    FMemory::Memzero(&Out, sizeof(Out));
    In.width = static_cast<unsigned int>(InputWidth);
    In.height = static_cast<unsigned int>(InputHeight);
    In.format = static_cast<uint32_t>(ColorFormat);
    In.layout = static_cast<uint32_t>(ColorLayout);
    In.mip_count = 1;
    In.frame = nullptr;

    Out.format = static_cast<uint32_t>(OutputFormat);
    Out.layout = static_cast<uint32_t>(OutputLayout);
    Out.mip_count = 1;

    const EMagicSRRHIKind Kind = MagicSRRHI::GetKind();
    if (Kind == EMagicSRRHIKind::Vulkan)
    {
        In.handle.vk_image = static_cast<uint64_t>(ColorIn);
        Out.handle.vk_image = static_cast<uint64_t>(ColorOut);
    }
    else if (Kind == EMagicSRRHIKind::OpenGL)
    {
        In.handle.gl_texture = static_cast<uint32_t>(ColorIn);
        Out.handle.gl_texture = static_cast<uint32_t>(ColorOut);
    }
    else
    {
        In.handle.pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(ColorIn));
        Out.handle.pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(ColorOut));
    }

    return EnableAndStore(SessionId, Handle, &In, &Out);
#else
    return -3999;
#endif
}

int32 UMagicSRBlueprintLibrary::ProcessTemporalNative(
    int32 SessionId,
    int64 ColorIn,
    int64 Depth,
    int64 Motion,
    int64 ColorOut,
    int32 InputWidth,
    int32 InputHeight,
    int32 ColorFormat,
    int32 DepthFormat,
    int32 MotionFormat,
    int32 OutputFormat,
    int32 ColorLayout,
    int32 DepthLayout,
    int32 MotionLayout,
    int32 OutputLayout,
    const FMagicSRTemporalFrameParams& Temporal)
{
#if MAGIC_SR_SUPPORTED
    void* Handle = FindHandle(SessionId);
    if (Handle == nullptr || ColorIn == 0 || Depth == 0 || Motion == 0 || ColorOut == 0 ||
        InputWidth <= 0 || InputHeight <= 0)
    {
        return -3001;
    }

    temporal_frame_t Tf;
    FillTemporalExtras(Temporal, Tf);

    const EMagicSRRHIKind Kind = MagicSRRHI::GetKind();
    auto BindResource = [Kind](int64 Value, uint32 Format, uint32 Layout, magic_resource_t& Res)
    {
        FMemory::Memzero(&Res, sizeof(Res));
        Res.format = Format;
        Res.layout = Layout;
        Res.mip_count = 1;
        if (Kind == EMagicSRRHIKind::Vulkan)
        {
            Res.handle.vk_image = static_cast<uint64_t>(Value);
        }
        else if (Kind == EMagicSRRHIKind::OpenGL)
        {
            Res.handle.gl_texture = static_cast<uint32_t>(Value);
        }
        else
        {
            Res.handle.pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(Value));
        }
    };

    BindResource(Depth, static_cast<uint32>(DepthFormat), static_cast<uint32>(DepthLayout), Tf.depth);
    BindResource(Motion, static_cast<uint32>(MotionFormat), static_cast<uint32>(MotionLayout), Tf.motion);

    mc_nscaler_input_frame_t In;
    mc_nscaler_output_frame_t Out;
    FMemory::Memzero(&In, sizeof(In));
    FMemory::Memzero(&Out, sizeof(Out));
    In.width = static_cast<unsigned int>(InputWidth);
    In.height = static_cast<unsigned int>(InputHeight);
    In.format = static_cast<uint32_t>(ColorFormat);
    In.layout = static_cast<uint32_t>(ColorLayout);
    In.mip_count = 1;
    In.frame = &Tf;
    In.command_buffer = reinterpret_cast<void*>(static_cast<uintptr_t>(Temporal.CommandBuffer));

    Out.format = static_cast<uint32_t>(OutputFormat);
    Out.layout = static_cast<uint32_t>(OutputLayout);
    Out.mip_count = 1;

    if (Kind == EMagicSRRHIKind::Vulkan)
    {
        In.handle.vk_image = static_cast<uint64_t>(ColorIn);
        Out.handle.vk_image = static_cast<uint64_t>(ColorOut);
    }
    else if (Kind == EMagicSRRHIKind::OpenGL)
    {
        In.handle.gl_texture = static_cast<uint32_t>(ColorIn);
        Out.handle.gl_texture = static_cast<uint32_t>(ColorOut);
    }
    else
    {
        In.handle.pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(ColorIn));
        Out.handle.pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(ColorOut));
    }

    return EnableAndStore(SessionId, Handle, &In, &Out);
#else
    return -3999;
#endif
}

int32 UMagicSRBlueprintLibrary::ProcessSpatial(
    int32 SessionId,
    UTexture* ColorIn,
    UTexture* ColorOut,
    int32 InputWidth,
    int32 InputHeight)
{
#if MAGIC_SR_SUPPORTED
    void* Handle = FindHandle(SessionId);
    if (Handle == nullptr || ColorIn == nullptr || ColorOut == nullptr || InputWidth <= 0 || InputHeight <= 0)
    {
        return -3007;
    }

    FTextureRenderTargetResource* InRT = nullptr;
    FTextureRenderTargetResource* OutRT = nullptr;
    FTextureResource* InTex = nullptr;
    FTextureResource* OutTex = nullptr;
    if (!ResolveUTextureResources(ColorIn, InRT, InTex) || !ResolveUTextureResources(ColorOut, OutRT, OutTex))
    {
        return -3008;
    }

    int32 ProcessRet = -3010;
    FEvent* VulkanDone = nullptr;
    ENQUEUE_RENDER_COMMAND(MagicSRProcessSpatial)(
        [SessionId, Handle, InRT, OutRT, InTex, OutTex, InputWidth, InputHeight, &ProcessRet,
         &VulkanDone](FRHICommandListImmediate& RHICmdList) mutable
        {
            FRHITexture* InRHI = MagicSRRHI::ResolveFromGameResources(InRT, InTex);
            FRHITexture* OutRHI = MagicSRRHI::ResolveFromGameResources(OutRT, OutTex);
            FMagicSRNativeResource InNative;
            FMagicSRNativeResource OutNative;
            if (!MagicSRRHI::ResolveTexture(InRHI, InNative) || !MagicSRRHI::ResolveTexture(OutRHI, OutNative))
            {
                ProcessRet = -3008;
                return;
            }

            mc_nscaler_input_frame_t In;
            mc_nscaler_output_frame_t Out;
            FMemory::Memzero(&In, sizeof(In));
            FMemory::Memzero(&Out, sizeof(Out));
            ApplyNativeToInFrame(InNative, In);
            ApplyNativeToOutFrame(OutNative, Out);
            In.width = static_cast<unsigned int>(InputWidth);
            In.height = static_cast<unsigned int>(InputHeight);
            In.frame = nullptr;

#if PLATFORM_WINDOWS || PLATFORM_LINUX
            if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan)
            {
                FRHITexture* OutTexRHI = OutRHI;
                const bool bOutUAV =
                    OutTexRHI && EnumHasAnyFlags(OutTexRHI->GetDesc().Flags, ETextureCreateFlags::UAV);
                In.layout = 1;  // VK_IMAGE_LAYOUT_GENERAL
                Out.layout = 1; // VK_IMAGE_LAYOUT_GENERAL
                if (OutTexRHI && bOutUAV)
                {
                    RHICmdList.Transition(
                        FRHITransitionInfo(OutTexRHI, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
                }
                VulkanDone = FPlatformProcess::GetSynchEventFromPool(true);
                RHICmdList.EnqueueLambda(
                    [SessionId, Handle, In, Out, &ProcessRet, Done = VulkanDone](
                        FRHICommandListBase& ExecutingCmdList) mutable
                    {
                        IVulkanDynamicRHI* VulkanRHI = GetIVulkanDynamicRHI();
                        In.command_buffer =
                            reinterpret_cast<void*>(VulkanRHI->RHIGetVkCommandBuffer(ExecutingCmdList));
                        ProcessRet = EnableAndStore(SessionId, Handle, &In, &Out);
                        Done->Trigger();
                    });
                if (OutTexRHI)
                {
                    // Make result sampleable by HUD / Canvas (after process records into same list).
                    RHICmdList.Transition(FRHITransitionInfo(
                        OutTexRHI, bOutUAV ? ERHIAccess::UAVCompute : ERHIAccess::Unknown,
                        ERHIAccess::SRVGraphics));
                }
                return;
            }
#endif

#if PLATFORM_WINDOWS
            // UE D3D11 RTs are often R8G8B8A8_TYPELESS; MagicSR requires UNORM.
            if (MagicSRRHI::GetKind() == EMagicSRRHIKind::D3D11 && InNative.Pointer && OutNative.Pointer &&
                GDynamicRHI != nullptr)
            {
                ID3D11Device* Device = static_cast<ID3D11Device*>(GDynamicRHI->RHIGetNativeDevice());
                ID3D11DeviceContext* Ctx = nullptr;
                if (Device)
                {
                    Device->GetImmediateContext(&Ctx);
                }
                ID3D11Texture2D* SrcIn = static_cast<ID3D11Texture2D*>(InNative.Pointer);
                ID3D11Texture2D* SrcOut = static_cast<ID3D11Texture2D*>(OutNative.Pointer);
                D3D11_TEXTURE2D_DESC SrcInDesc = {};
                D3D11_TEXTURE2D_DESC SrcOutDesc = {};
                SrcIn->GetDesc(&SrcInDesc);
                SrcOut->GetDesc(&SrcOutDesc);
                if (Ctx &&
                    MagicSRD3D11Bridge::EnsureRgba8Unorm(Device, GD3D11SpatialIn, SrcInDesc.Width, SrcInDesc.Height, false) &&
                    MagicSRD3D11Bridge::EnsureRgba8Unorm(Device, GD3D11SpatialOut, SrcOutDesc.Width, SrcOutDesc.Height, true) &&
                    MagicSRD3D11Bridge::CopyTexture(Ctx, SrcIn, GD3D11SpatialIn))
                {
                    In.handle.pointer = GD3D11SpatialIn;
                    In.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
                    Out.handle.pointer = GD3D11SpatialOut;
                    Out.format = 28;
                    ProcessRet = EnableAndStore(SessionId, Handle, &In, &Out);
                    (void)SrcOut;
                    Ctx->Release();
                    return;
                }
                if (Ctx)
                {
                    Ctx->Release();
                }
            }
#endif

            ProcessRet = EnableAndStore(SessionId, Handle, &In, &Out);
        });
    FlushRenderingCommands();
    if (VulkanDone)
    {
        VulkanDone->Wait();
        FPlatformProcess::ReturnSynchEventToPool(VulkanDone);
    }
    return ProcessRet;
#else
    return -3999;
#endif
}

int32 UMagicSRBlueprintLibrary::ProcessTemporal(
    int32 SessionId,
    UTexture* ColorIn,
    UTexture* Depth,
    UTexture* Motion,
    UTexture* ColorOut,
    int32 InputWidth,
    int32 InputHeight,
    const FMagicSRTemporalFrameParams& Temporal)
{
#if MAGIC_SR_SUPPORTED
    void* Handle = FindHandle(SessionId);
    if (Handle == nullptr || ColorIn == nullptr || Depth == nullptr || Motion == nullptr || ColorOut == nullptr ||
        InputWidth <= 0 || InputHeight <= 0)
    {
        return -3007;
    }

    FTextureRenderTargetResource *ColorRT = nullptr, *DepthRT = nullptr, *MotionRT = nullptr, *OutRT = nullptr;
    FTextureResource *ColorTex = nullptr, *DepthTex = nullptr, *MotionTex = nullptr, *OutTex = nullptr;
    if (!ResolveUTextureResources(ColorIn, ColorRT, ColorTex) ||
        !ResolveUTextureResources(Depth, DepthRT, DepthTex) ||
        !ResolveUTextureResources(Motion, MotionRT, MotionTex) ||
        !ResolveUTextureResources(ColorOut, OutRT, OutTex))
    {
        return -3008;
    }

    FMagicSRTemporalFrameParams TemporalCopy = Temporal;
    int32 ProcessRet = -3010;
    FEvent* VulkanDone = nullptr;
    ENQUEUE_RENDER_COMMAND(MagicSRProcessTemporal)(
        [SessionId, Handle, ColorRT, DepthRT, MotionRT, OutRT, ColorTex, DepthTex, MotionTex, OutTex, InputWidth,
         InputHeight, TemporalCopy, &ProcessRet, &VulkanDone](FRHICommandListImmediate& RHICmdList) mutable
        {
            FMagicSRNativeResource ColorN, DepthN, MotionN, OutN;
            if (!MagicSRRHI::ResolveTexture(MagicSRRHI::ResolveFromGameResources(ColorRT, ColorTex), ColorN) ||
                !MagicSRRHI::ResolveTexture(MagicSRRHI::ResolveFromGameResources(DepthRT, DepthTex), DepthN) ||
                !MagicSRRHI::ResolveTexture(MagicSRRHI::ResolveFromGameResources(MotionRT, MotionTex), MotionN) ||
                !MagicSRRHI::ResolveTexture(MagicSRRHI::ResolveFromGameResources(OutRT, OutTex), OutN))
            {
                ProcessRet = -3008;
                return;
            }

            temporal_frame_t Tf;
            FillTemporalExtras(TemporalCopy, Tf);
            MagicSRRHI::FillColorResource(DepthN, Tf.depth);
            MagicSRRHI::FillColorResource(MotionN, Tf.motion);

            mc_nscaler_input_frame_t In;
            mc_nscaler_output_frame_t Out;
            FMemory::Memzero(&In, sizeof(In));
            FMemory::Memzero(&Out, sizeof(Out));
            ApplyNativeToInFrame(ColorN, In);
            ApplyNativeToOutFrame(OutN, Out);
            In.width = static_cast<unsigned int>(InputWidth);
            In.height = static_cast<unsigned int>(InputHeight);
            In.frame = &Tf;

            if (TemporalCopy.CommandBuffer != 0)
            {
                In.command_buffer = reinterpret_cast<void*>(static_cast<uintptr_t>(TemporalCopy.CommandBuffer));
                ProcessRet = EnableAndStore(SessionId, Handle, &In, &Out);
                return;
            }

#if PLATFORM_WINDOWS || PLATFORM_LINUX
            if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan)
            {
                FRHITexture* ColorTexRHI = MagicSRRHI::ResolveFromGameResources(ColorRT, ColorTex);
                FRHITexture* DepthTexRHI = MagicSRRHI::ResolveFromGameResources(DepthRT, DepthTex);
                FRHITexture* MotionTexRHI = MagicSRRHI::ResolveFromGameResources(MotionRT, MotionTex);
                FRHITexture* OutTexRHI = MagicSRRHI::ResolveFromGameResources(OutRT, OutTex);
                const bool bOutUAV =
                    OutTexRHI && EnumHasAnyFlags(OutTexRHI->GetDesc().Flags, ETextureCreateFlags::UAV);
                In.layout = 1;  // VK_IMAGE_LAYOUT_GENERAL
                Out.layout = 1; // VK_IMAGE_LAYOUT_GENERAL
                Tf.depth.layout = 1;
                Tf.motion.layout = 1;
                if (ColorTexRHI)
                {
                    RHICmdList.Transition(
                        FRHITransitionInfo(ColorTexRHI, ERHIAccess::Unknown, ERHIAccess::SRVMask));
                }
                if (DepthTexRHI)
                {
                    RHICmdList.Transition(
                        FRHITransitionInfo(DepthTexRHI, ERHIAccess::Unknown, ERHIAccess::SRVMask));
                }
                if (MotionTexRHI)
                {
                    RHICmdList.Transition(
                        FRHITransitionInfo(MotionTexRHI, ERHIAccess::Unknown, ERHIAccess::SRVMask));
                }
                if (OutTexRHI && bOutUAV)
                {
                    RHICmdList.Transition(
                        FRHITransitionInfo(OutTexRHI, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
                }
                else if (OutTexRHI && !bOutUAV)
                {
                    UE_LOG(LogTemp, Error,
                           TEXT("[MagicSR] ColorOut missing TexCreate_UAV — Vulkan write will stay black"));
                }
                VulkanDone = FPlatformProcess::GetSynchEventFromPool(true);
                RHICmdList.EnqueueLambda(
                    [SessionId, Handle, In, Out, Tf, &ProcessRet, Done = VulkanDone](
                        FRHICommandListBase& ExecutingCmdList) mutable
                    {
                        In.frame = &Tf;
                        IVulkanDynamicRHI* VulkanRHI = GetIVulkanDynamicRHI();
                        In.command_buffer =
                            reinterpret_cast<void*>(VulkanRHI->RHIGetVkCommandBuffer(ExecutingCmdList));
                        ProcessRet = EnableAndStore(SessionId, Handle, &In, &Out);
                        Done->Trigger();
                    });
                if (OutTexRHI)
                {
                    RHICmdList.Transition(FRHITransitionInfo(
                        OutTexRHI, bOutUAV ? ERHIAccess::UAVCompute : ERHIAccess::Unknown,
                        ERHIAccess::SRVGraphics));
                }
                return;
            }
#endif

            ProcessRet = EnableAndStore(SessionId, Handle, &In, &Out);
        });
    FlushRenderingCommands();
    if (VulkanDone)
    {
        VulkanDone->Wait();
        FPlatformProcess::ReturnSynchEventToPool(VulkanDone);
    }
    return ProcessRet;
#else
    return -3999;
#endif
}

int32 UMagicSRBlueprintLibrary::ProcessSpatialBuffer(
    int32 SessionId,
    const TArray<uint8>& InputBuffer,
    TArray<uint8>& OutputBuffer,
    int32 InputWidth,
    int32 InputHeight)
{
#if MAGIC_SR_SUPPORTED
    void* Handle = FindHandle(SessionId);
    if (Handle == nullptr || InputWidth <= 0 || InputHeight <= 0 || InputBuffer.Num() <= 0)
    {
        return -3001;
    }

    output_status_params_t Status;
    FMemory::Memzero(&Status, sizeof(Status));
    if (mc_nscaler_control(&Handle, MC_NSCALER_CMD_QUERY_STATUS, nullptr, &Status) != 0)
    {
        StoreHandle(SessionId, Handle);
        return -3002;
    }
    StoreHandle(SessionId, Handle);

    const int32 OutW = Status.output_width > 0
                           ? static_cast<int32>(Status.output_width)
                           : FMath::Max(1, FMath::RoundToInt(InputWidth * Status.scaler_factor));
    const int32 OutH = Status.output_height > 0
                           ? static_cast<int32>(Status.output_height)
                           : FMath::Max(1, FMath::RoundToInt(InputHeight * Status.scaler_factor));
    const int32 Channels = (Status.input_type == INPUT_BUFFER_RGB) ? 3 : 1;
    OutputBuffer.SetNumUninitialized(OutW * OutH * Channels);

    mc_nscaler_input_frame_t In;
    mc_nscaler_output_frame_t Out;
    FMemory::Memzero(&In, sizeof(In));
    FMemory::Memzero(&Out, sizeof(Out));
    In.width = static_cast<unsigned int>(InputWidth);
    In.height = static_cast<unsigned int>(InputHeight);
    In.handle.pointer = const_cast<uint8*>(InputBuffer.GetData());
    In.frame = nullptr;
    Out.handle.pointer = OutputBuffer.GetData();
    Out.width = static_cast<unsigned int>(OutW);
    Out.height = static_cast<unsigned int>(OutH);

    return EnableAndStore(SessionId, Handle, &In, &Out);
#else
    return -3999;
#endif
}

bool UMagicSRBlueprintLibrary::QueryStatus(int32 SessionId, FMagicSRStatus& OutStatus)
{
#if MAGIC_SR_SUPPORTED
    void* Handle = FindHandle(SessionId);
    if (Handle == nullptr)
    {
        return false;
    }
    output_status_params_t Status;
    FMemory::Memzero(&Status, sizeof(Status));
    if (mc_nscaler_control(&Handle, MC_NSCALER_CMD_QUERY_STATUS, nullptr, &Status) != 0)
    {
        StoreHandle(SessionId, Handle);
        return false;
    }
    StoreHandle(SessionId, Handle);
    FillStatus(Status, OutStatus);
    return true;
#else
    return false;
#endif
}
