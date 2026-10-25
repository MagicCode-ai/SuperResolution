#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "MagicSRBlueprintLibrary.generated.h"

class UPrimitiveComponent;

USTRUCT(BlueprintType)
struct FMagicSRStatus
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 Width = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 Height = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 OutputWidth = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 OutputHeight = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    float ScalerFactor = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 AlgMode = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 Backend = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 NumThreads = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    float GpuTimeMs = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int64 ErrorCode = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 TemporalStatusValid = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 TemporalFrameIndex = 0;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    float TemporalJitterX = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    float TemporalJitterY = 0.0f;
};

USTRUCT(BlueprintType)
struct FMagicSRCreateParams
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    FString ModelPath;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float Scale = 2.0f;

    /** SR bits: 0=ImageSpeed 1=ImageBalanced 2=GraphicSpeed 3=GraphicBalanced (v2 TEMPORAL_*). Optional GEN_SPEED=4 / GEN_BALANCED=8. */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 AlgMode = 0;

    /** 0=Default 1=X86 4=OpenGL 6=Vulkan 7=D3D11 */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 Backend = 0;

    /** 0=BufferR8 1=TextureRgb8Unorm 2=TextureR8Unorm 3=BufferRGB */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 InputType = 1;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 SpatialSharpenLevel = 0;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 DepthReversed = 0;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 DepthInfinite = 0;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 HdrColor = 0;

    /** Optional: VkPhysicalDevice / ID3D11Device* as int64 */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int64 GpuPhysicalDevice = 0;

    /** Optional: VkDevice / ID3D11Device* as int64 */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int64 GpuDevice = 0;

    /** Optional: PFN_vkGetInstanceProcAddr / ID3D11DeviceContext* as int64 */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int64 GpuDeviceContext = 0;

    /** Optional: VkInstance as int64 */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int64 GpuNativeContext = 0;

    /** Optional: PFN_vkGetDeviceProcAddr as int64 */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int64 GpuGetDeviceProcAddr = 0;
};

USTRUCT(BlueprintType)
struct FMagicSRTemporalFrameParams
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float JitterOffsetX = 0.0f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float JitterOffsetY = 0.0f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float MotionVectorScaleX = 0.0f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float MotionVectorScaleY = 0.0f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float FrameTimeDeltaMs = 16.666f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 FrameIndex = 0;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    bool ResetHistory = false;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float CameraNear = 5.0f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float CameraFar = 65536.0f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float CameraFovY = 1.04719755f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float ViewToMeters = 1.0f;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    bool EnableSharpening = false;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    float Sharpness = 0.0f;

    /** mc_mv_jitter_e: 0 unspecified/excluded, 2 included */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 MvJitter = 0;

    /** Vulkan: recording VkCommandBuffer as int64; other backends 0 */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int64 CommandBuffer = 0;
};

UCLASS()
class MAGICSR_API UMagicSRBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static FString GetVersion();

    /** Create handle via mc_nscaler_control(SET_PARAM). Returns SessionId (>=0) or -1. */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static bool CreateHandle(const FMagicSRCreateParams& Params, int32& OutSessionId);

    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static int32 DestroyHandle(int32 SessionId);

    /**
     * Spatial path: color in/out only (temporal_frame_t left NULL).
     * Resolves UTexture/RT native handles on the render thread.
     */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static int32 ProcessSpatial(
        int32 SessionId,
        UTexture* ColorIn,
        UTexture* ColorOut,
        int32 InputWidth,
        int32 InputHeight);

    /**
     * Temporal path with full inputs: color, depth, motion, jitter, camera.
     * ColorIn/Depth/Motion at input size; ColorOut at output size.
     */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static int32 ProcessTemporal(
        int32 SessionId,
        UTexture* ColorIn,
        UTexture* Depth,
        UTexture* Motion,
        UTexture* ColorOut,
        int32 InputWidth,
        int32 InputHeight,
        const FMagicSRTemporalFrameParams& Temporal);

    /** Low-level native handles (Vulkan: VkImage as uint64; D3D11/Metal: pointer as int64; GL: GLuint). */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static int32 ProcessTemporalNative(
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
        const FMagicSRTemporalFrameParams& Temporal);

    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static int32 ProcessSpatialNative(
        int32 SessionId,
        int64 ColorIn,
        int64 ColorOut,
        int32 InputWidth,
        int32 InputHeight,
        int32 ColorFormat,
        int32 OutputFormat,
        int32 ColorLayout,
        int32 OutputLayout);

    /** CPU buffer spatial (Linux/Windows X86). Input planar RGB or R8 per CreateHandle InputType. */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static int32 ProcessSpatialBuffer(
        int32 SessionId,
        const TArray<uint8>& InputBuffer,
        TArray<uint8>& OutputBuffer,
        int32 InputWidth,
        int32 InputHeight);

    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static bool QueryStatus(int32 SessionId, FMagicSRStatus& OutStatus);

    /** Default model path under plugin ThirdParty/models. */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static FString GetBundledModelPath(bool bGpuModel = true);

    /**
     * Mark a mesh whose content changes without motion vectors (TV / video wall / LED screen) so
     * Graphic Speed drops its accumulated history there (removes ghosting on content changes).
     * Uses CustomDepth stencil (r.MagicSR.StencilReactiveValue); needs r.CustomDepth=3, raised
     * automatically on first use. Also overrides the component's own CustomDepth settings.
     */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    static void SetMagicSRReactiveMarker(UPrimitiveComponent* Component, bool bMarked);
};
