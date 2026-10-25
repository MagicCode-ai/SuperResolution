#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "MagicSRBlueprintLibrary.h"
#include "MagicSRViewportUpscaler.generated.h"

class AMagicSRCaptureRig;

UENUM(BlueprintType)
enum class EMagicSRCompareMode : uint8
{
    Off UMETA(DisplayName = "Off"),
    Spatial UMETA(DisplayName = "Spatial"),
    Temporal UMETA(DisplayName = "Temporal"),
    Native UMETA(DisplayName = "Native100"),
    /** Full res, viewport temporal off, r.AntiAliasingMethod 0, tonemapper sharpen 0. */
    NativeNoAA UMETA(DisplayName = "NativeNoAA"),
    /** Viewport temporal off, no library session. r.ScreenPercentage=50, TSR, tonemapper sharpen 0. */
    UETSR50 UMETA(DisplayName = "UETSR50")
};

/**
 * Viewport capture + MagicSR process + left/right compare RTs.
 * Temporal uses color/depth/motion RTs supplied by the owner (or SceneCapture children).
 */
UCLASS(BlueprintType)
class MAGICSR_API AMagicSRViewportUpscaler : public AActor
{
    GENERATED_BODY()

public:
    AMagicSRViewportUpscaler();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaSeconds) override;

    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    void SetCompareMode(EMagicSRCompareMode Mode);

    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    EMagicSRCompareMode GetCompareMode() const { return CompareMode; }

    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    void CycleCompareMode();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    EMagicSRCompareMode CompareMode = EMagicSRCompareMode::Temporal;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    float Scale = 2.0f;

    /** 2=TemporalSpeed 3=TemporalBalanced for Temporal mode; 0/1 for Spatial */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    int32 TemporalAlgMode = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    int32 SpatialAlgMode = 0;

    /** 0=auto (D3D11 on Win, Vulkan on Linux), 6=Vulkan, 7=D3D11, 1=X86 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    int32 BackendOverride = 0;

    /** Capture res. 960×540 → 1920×1080 keeps aliases milder than 640×360. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    int32 InputWidth = 960;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    int32 InputHeight = 540;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR|Inputs")
    TObjectPtr<UTextureRenderTarget2D> ColorInRT = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR|Inputs")
    TObjectPtr<UTextureRenderTarget2D> DepthRT = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR|Inputs")
    TObjectPtr<UTextureRenderTarget2D> MotionRT = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MagicSR|Outputs")
    TObjectPtr<UTextureRenderTarget2D> ColorOutRT = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MagicSR|Outputs")
    TObjectPtr<UTextureRenderTarget2D> CompareRT = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    FMagicSRStatus LastStatus;

    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    int32 LastProcessResult = 0;

    /** From CaptureRig: jitter baked into the ColorIn RT (input pixels, +Y down). */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR|Inputs")
    FVector2D InjectedJitterPixels = FVector2D::ZeroVector;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR|Inputs")
    bool bUseInjectedJitter = false;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR|Inputs")
    bool bInjectedCameraCut = false;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR|Inputs")
    float InjectedCameraFovYDegrees = 90.0f;

    /** Optional: programs next Halton projection after Process (avoids jitter mismatch). */
    UPROPERTY(BlueprintReadWrite, Category = "MagicSR|Inputs")
    TObjectPtr<AMagicSRCaptureRig> CaptureRig = nullptr;

protected:
    void EnsureOutputTargets();
    void RecreateHandle();
    void DestroySession();
    int32 ResolveBackend() const;
    void ProcessFrame(float DeltaSeconds);
    void BuildCompareComposite();
    /** Off-mode reference: bilinear upsample ColorIn → ColorOut (same size as SR). */
    void UpsampleBilinearReference();

    int32 SessionId = -1;
    EMagicSRCompareMode ActiveMode = EMagicSRCompareMode::Off;
    bool bCreateFailed = false;
    int32 FrameIndex = 0;
    FVector2D LastJitter = FVector2D::ZeroVector;
};
