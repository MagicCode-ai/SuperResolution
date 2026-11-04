#include "MagicSRCompareHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"

void AMagicSRCompareHUD::DrawHUD()
{
    Super::DrawHUD();
    if (Canvas == nullptr)
    {
        return;
    }

    const float ScreenW = Canvas->ClipX;
    const float ScreenH = Canvas->ClipY;
    if (ScreenW < 2.f || ScreenH < 2.f)
    {
        return;
    }

    // Viewport Temporal path: status overlay only (fullscreen SR already on view).
    if (ColorInRT == nullptr || ColorOutRT == nullptr)
    {
        Canvas->SetDrawColor(FColor::White);
        Canvas->DrawText(
            GEngine->GetSmallFont(),
            FString::Printf(TEXT("%s  rc=%d"), *ModeLabel, ProcessRc),
            16.f,
            16.f);
        return;
    }

    const float HalfW = ScreenW * 0.5f;

    auto DrawRT = [this](UTextureRenderTarget2D* RT, float X, float Y, float W, float H)
    {
        if (RT == nullptr || RT->GetResource() == nullptr)
        {
            return;
        }
        FCanvasTileItem Tile(FVector2D(X, Y), RT->GetResource(), FVector2D(W, H), FLinearColor::White);
        Tile.BlendMode = SE_BLEND_Opaque;
        Canvas->DrawItem(Tile);
    };

    // Draw at output native pixels (may clip). Downscaling both panes to fit the
    // window hides 2× SR vs bilinear — that was making Off/On look identical.
    const float OutW = ColorOutRT->SizeX > 0 ? static_cast<float>(ColorOutRT->SizeX) : HalfW;
    const float OutH = ColorOutRT->SizeY > 0 ? static_cast<float>(ColorOutRT->SizeY) : ScreenH;
    const float LeftX = (HalfW - OutW) * 0.5f;
    const float RightX = HalfW + (HalfW - OutW) * 0.5f;
    const float DrawY = (ScreenH - OutH) * 0.5f;

    DrawRT(ColorInRT, LeftX, DrawY, OutW, OutH);
    DrawRT(ColorOutRT, RightX, DrawY, OutW, OutH);

    FCanvasTileItem Line(FVector2D(HalfW - 1.f, 0.f), GWhiteTexture, FVector2D(2.f, ScreenH), FLinearColor::Yellow);
    Line.BlendMode = SE_BLEND_Opaque;
    Canvas->DrawItem(Line);

    const FString Left = FString::Printf(
        TEXT("INPUT %dx%d (nearest stretch)"),
        ColorInRT->SizeX,
        ColorInRT->SizeY);
    const FString Right = FString::Printf(
        TEXT("%s  %dx%d  rc=%d"),
        *ModeLabel,
        ColorOutRT->SizeX,
        ColorOutRT->SizeY,
        ProcessRc);
    Canvas->SetDrawColor(FColor::White);
    Canvas->DrawText(GEngine->GetSmallFont(), Left, 16.f, 16.f);
    Canvas->DrawText(GEngine->GetSmallFont(), Right, HalfW + 16.f, 16.f);
}
