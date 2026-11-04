#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"

class FMagicSRSceneViewExtension;

class MAGICSR_API FMagicSRModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FMagicSRModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FMagicSRModule>(TEXT("MagicSR"));
	}

	/** Register SceneViewExtension (idempotent). Called from StartupModule. */
	void EnsureSceneViewExtension();

	/**
	 * Enable/disable player-viewport Temporal path (ITemporalUpscaler).
	 * When enabling, sets r.ScreenPercentage to InScreenPercentage (default 50)
	 * and r.Tonemapper.Sharpen to r.MagicSR.SharpenAmount.
	 * r.MagicSR.LibrarySharpness (default 0) feeds Tf.enable_sharpening for
	 * Speed and Balanced. 0 disables it. r.MagicSR.SharpenAmount stays the
	 * engine tonemapper dose.
	 * When disabling, restores both if we changed them.
	 */
	void SetViewportTemporalEnabled(bool bEnabled, float InScreenPercentage = 50.0f);

	bool IsViewportTemporalEnabled() const;

private:
	TSharedPtr<FMagicSRSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
	TOptional<float> SavedScreenPercentage;
	TOptional<float> SavedTonemapSharpen;
};
