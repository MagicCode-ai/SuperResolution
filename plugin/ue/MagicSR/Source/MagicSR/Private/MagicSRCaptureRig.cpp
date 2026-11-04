#include "MagicSRCaptureRig.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Math/PerspectiveMatrix.h"
#include "SceneView.h"

AMagicSRCaptureRig::AMagicSRCaptureRig()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;

    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

    ColorCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("ColorCapture"));
    ColorCapture->SetupAttachment(RootComponent);
    ColorCapture->bCaptureEveryFrame = true;
    ColorCapture->bCaptureOnMovement = false;
    ColorCapture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    ColorCapture->bAlwaysPersistRenderingState = true;
    // No engine TAA — MagicSR Temporal owns AA via Halton-jittered projection.
    ColorCapture->ShowFlags.SetAntiAliasing(false);
    ColorCapture->ShowFlags.SetTemporalAA(false);
    ColorCapture->ShowFlags.SetMotionBlur(false);

    DepthCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("DepthCapture"));
    DepthCapture->SetupAttachment(RootComponent);
    DepthCapture->bCaptureEveryFrame = true;
    DepthCapture->bCaptureOnMovement = false;
    DepthCapture->CaptureSource = ESceneCaptureSource::SCS_DeviceDepth;
    DepthCapture->bAlwaysPersistRenderingState = true;
}

void AMagicSRCaptureRig::BeginPlay()
{
    Super::BeginPlay();
    EnsureTargets();
    if (bEnableTemporalJitter)
    {
        ProgramNextJitter();
    }
}

void AMagicSRCaptureRig::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (MotionRT)
    {
        UKismetRenderingLibrary::ClearRenderTarget2D(this, MotionRT, FLinearColor(0.f, 0.f, 0.f, 0.f));
    }
}

float AMagicSRCaptureRig::Halton(int32 Index, int32 Base)
{
    float F = 1.0f;
    float R = 0.0f;
    int32 I = Index;
    while (I > 0)
    {
        F /= static_cast<float>(Base);
        R += F * static_cast<float>(I % Base);
        I /= Base;
    }
    return R;
}

FMatrix AMagicSRCaptureRig::BuildJitteredProjection(float FOVDegrees, float JitterXPx, float JitterYPx) const
{
    const int32 W = FMath::Max(1, CaptureWidth);
    const int32 H = FMath::Max(1, CaptureHeight);
    const float UnscaledHalfFov = FMath::DegreesToRadians(FOVDegrees) * 0.5f;
    const float Overscan = ColorCapture ? ColorCapture->Overscan : 0.0f;
    const float HalfFov = FMath::Atan((1.0f + Overscan) * FMath::Tan(UnscaledHalfFov));
    const float NearClip = (ColorCapture && ColorCapture->bOverride_CustomNearClippingPlane)
                               ? ColorCapture->CustomNearClippingPlane
                               : GNearClippingPlane;
    const float FarClip =
        (ColorCapture && ColorCapture->MaxViewDistanceOverride > 0.0f && ColorCapture->bFiniteFarPlane)
            ? FMath::Max(ColorCapture->MaxViewDistanceOverride, NearClip)
            : NearClip;

    FMatrix Proj = FReversedZPerspectiveMatrix(
        HalfFov,
        HalfFov,
        1.0f,
        static_cast<float>(W) / static_cast<float>(H),
        NearClip,
        FarClip);

    // UE clip-space Y is up; MagicSR / view-rect jitter is +Y down.
    Proj.M[2][0] += 2.0f * JitterXPx / static_cast<float>(W);
    Proj.M[2][1] += -2.0f * JitterYPx / static_cast<float>(H);
    return Proj;
}

void AMagicSRCaptureRig::ApplyProjectionForJitter(const FVector2D& JitterPx)
{
    if (ColorCapture == nullptr || DepthCapture == nullptr)
    {
        return;
    }
    if (!bEnableTemporalJitter)
    {
        ColorCapture->bUseCustomProjectionMatrix = false;
        DepthCapture->bUseCustomProjectionMatrix = false;
        return;
    }
    const FMatrix Proj = BuildJitteredProjection(ColorCapture->FOVAngle, JitterPx.X, JitterPx.Y);
    ColorCapture->bUseCustomProjectionMatrix = true;
    ColorCapture->CustomProjectionMatrix = Proj;
    DepthCapture->bUseCustomProjectionMatrix = true;
    DepthCapture->CustomProjectionMatrix = Proj;
}

void AMagicSRCaptureRig::CommitJitterAfterRender()
{
    EnsureTargets();
    if (!bEnableTemporalJitter)
    {
        ActiveJitterPixels = FVector2D::ZeroVector;
        ColorCapture->bUseCustomProjectionMatrix = false;
        DepthCapture->bUseCustomProjectionMatrix = false;
        return;
    }
    // Last ProgramNextJitter drove the capture that is now in ColorRT.
    if (bJitterProgrammed)
    {
        ActiveJitterPixels = ProgrammedJitterPixels;
    }
}

void AMagicSRCaptureRig::ProgramNextJitter()
{
    EnsureTargets();
    if (!bEnableTemporalJitter || ColorCapture == nullptr || DepthCapture == nullptr)
    {
        ColorCapture->bUseCustomProjectionMatrix = false;
        DepthCapture->bUseCustomProjectionMatrix = false;
        return;
    }

    const int32 Phase = JitterFrameIndex % 32;
    ProgrammedJitterPixels.X = Halton(Phase + 1, 2) - 0.5f;
    ProgrammedJitterPixels.Y = Halton(Phase + 1, 3) - 0.5f;
    ++JitterFrameIndex;
    bJitterProgrammed = true;
    ApplyProjectionForJitter(ProgrammedJitterPixels);
}

void AMagicSRCaptureRig::NotifyCameraPose(const FVector& WorldLocation, const FRotator& WorldRotation, float FOVDegrees)
{
    if (ColorCapture)
    {
        ColorCapture->FOVAngle = FOVDegrees;
    }
    if (DepthCapture)
    {
        DepthCapture->FOVAngle = FOVDegrees;
    }

    bCameraCut = false;
    if (bHasPrevCamera)
    {
        const float LocDelta = FVector::Dist(WorldLocation, PrevCameraLocation);
        const float YawDelta = FMath::Abs(FRotator::NormalizeAxis(WorldRotation.Yaw - PrevCameraRotation.Yaw));
        const float PitchDelta = FMath::Abs(FRotator::NormalizeAxis(WorldRotation.Pitch - PrevCameraRotation.Pitch));
        if (LocDelta > 50.0f || YawDelta > 8.0f || PitchDelta > 8.0f)
        {
            bCameraCut = true;
        }
    }
    PrevCameraLocation = WorldLocation;
    PrevCameraRotation = WorldRotation;
    bHasPrevCamera = true;
}

void AMagicSRCaptureRig::EnsureTargets()
{
    auto MakeRT = [this](const TCHAR* Name, int32 W, int32 H, EPixelFormat Format) -> UTextureRenderTarget2D*
    {
        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, Name);
        RT->RenderTargetFormat = RTF_RGBA8;
        RT->bGPUSharedFlag = false;
        RT->InitCustomFormat(W, H, Format, false);
        RT->ClearColor = FLinearColor::Black;
        RT->UpdateResourceImmediate(true);
        return RT;
    };

    if (ColorRT == nullptr || ColorRT->SizeX != CaptureWidth || ColorRT->SizeY != CaptureHeight)
    {
        ColorRT = MakeRT(TEXT("MagicSR_CapColor"), CaptureWidth, CaptureHeight, PF_R8G8B8A8);
        ColorRT->Filter = TF_Nearest;
        ColorRT->UpdateResourceImmediate(true);
    }
    if (DepthRT == nullptr || DepthRT->SizeX != CaptureWidth || DepthRT->SizeY != CaptureHeight)
    {
        DepthRT = MakeRT(TEXT("MagicSR_CapDepth"), CaptureWidth, CaptureHeight, PF_R32_FLOAT);
    }
    if (MotionRT == nullptr || MotionRT->SizeX != CaptureWidth || MotionRT->SizeY != CaptureHeight)
    {
        MotionRT = NewObject<UTextureRenderTarget2D>(this, TEXT("MagicSR_CapMotion"));
        MotionRT->RenderTargetFormat = RTF_RGBA16f;
        MotionRT->InitCustomFormat(CaptureWidth, CaptureHeight, PF_G16R16F, false);
        MotionRT->UpdateResourceImmediate(true);
    }

    ColorCapture->TextureTarget = ColorRT;
    DepthCapture->TextureTarget = DepthRT;
}

void AMagicSRCaptureRig::CaptureOnce()
{
    EnsureTargets();
    if (ColorCapture)
    {
        ColorCapture->CaptureScene();
    }
    if (DepthCapture)
    {
        DepthCapture->CaptureScene();
    }
}
