#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "MagicSRCaptureRig.generated.h"

/**
 * Fills Color/Depth (and cleared Motion) RTs for MagicSR Temporal/Spatial validation.
 * Place in front of the scene; assign the same RTs to AMagicSRViewportUpscaler.
 */
UCLASS(BlueprintType)
class MAGICSR_API AMagicSRCaptureRig : public AActor
{
    GENERATED_BODY()

public:
    AMagicSRCaptureRig();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MagicSR")
    TObjectPtr<USceneCaptureComponent2D> ColorCapture = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MagicSR")
    TObjectPtr<USceneCaptureComponent2D> DepthCapture = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    int32 CaptureWidth = 960;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    int32 CaptureHeight = 540;

    /** When true, Halton-jitter Color+Depth projection (required for Temporal AA). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MagicSR")
    bool bEnableTemporalJitter = true;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MagicSR")
    TObjectPtr<UTextureRenderTarget2D> ColorRT = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MagicSR")
    TObjectPtr<UTextureRenderTarget2D> DepthRT = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MagicSR")
    TObjectPtr<UTextureRenderTarget2D> MotionRT = nullptr;

    /** Jitter baked into the ColorRT currently readable on the game thread (+Y down, px). */
    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    FVector2D ActiveJitterPixels = FVector2D::ZeroVector;

    /** Set when camera teleported / cut — Temporal should reset history. */
    UPROPERTY(BlueprintReadOnly, Category = "MagicSR")
    bool bCameraCut = false;

    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    void CaptureOnce();

    /** Call after moving the capture (ValidateSubsystem). Detects cuts. */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    void NotifyCameraPose(const FVector& WorldLocation, const FRotator& WorldRotation, float FOVDegrees);

    /**
     * Call once per frame before Process: ActiveJitter matches ColorRT from last render.
     * Call ProgramNextJitter after Process (Flush may re-capture — must not advance early).
     */
    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    void CommitJitterAfterRender();

    UFUNCTION(BlueprintCallable, Category = "MagicSR")
    void ProgramNextJitter();

protected:
    void EnsureTargets();
    void ApplyProjectionForJitter(const FVector2D& JitterPx);
    static float Halton(int32 Index, int32 Base);
    FMatrix BuildJitteredProjection(float FOVDegrees, float JitterXPx, float JitterYPx) const;

    FVector2D ProgrammedJitterPixels = FVector2D::ZeroVector;
    int32 JitterFrameIndex = 0;
    bool bJitterProgrammed = false;
    FVector PrevCameraLocation = FVector::ZeroVector;
    FRotator PrevCameraRotation = FRotator::ZeroRotator;
    bool bHasPrevCamera = false;
};
