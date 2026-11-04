#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "MagicSRCompareHUD.generated.h"

class UTextureRenderTarget2D;

/** Fullscreen left/right compare: ColorIn | ColorOut. */
UCLASS()
class MAGICSR_API AMagicSRCompareHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    TObjectPtr<UTextureRenderTarget2D> ColorInRT = nullptr;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    TObjectPtr<UTextureRenderTarget2D> ColorOutRT = nullptr;

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    FString ModeLabel = TEXT("MagicSR");

    UPROPERTY(BlueprintReadWrite, Category = "MagicSR")
    int32 ProcessRc = 0;
};
