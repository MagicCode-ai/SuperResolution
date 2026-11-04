#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

/**
 * Injects FMagicSRTemporalUpscaler into the player view family when the viewport bridge is enabled.
 */
class FMagicSRSceneViewExtension final : public FSceneViewExtensionBase
{
public:
	FMagicSRSceneViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override;
	virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs) override;
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

private:
	FScreenPassTexture PostProcessPassAfterTonemap_RenderThread(
		FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessMaterialInputs& Inputs);

	/** Family that should receive the upscaler after jitter is computed. */
	FSceneViewFamily* PendingUpscaleFamily = nullptr;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
};
