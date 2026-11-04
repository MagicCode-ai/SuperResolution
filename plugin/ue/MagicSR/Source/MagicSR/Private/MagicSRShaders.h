#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ScreenPass.h"

class FRDGBuilder;
class FSceneView;

/** Optional CustomDepth-stencil reactive marking (merged into the reactive MRT, no extra pass). */
struct FMagicSRDecodeVelocityExtras
{
	/** Scene CustomDepth stencil SRV. Null disables marking (shader skips the fetch). */
	FRDGTextureSRVRef CustomStencil = nullptr;
	uint32 StencilValue = 0;
	float StencilStrength = 1.f;
};

/**
 * Decode UE velocity into MagicSR UV RG16F motion.
 * Pixels with no dynamic velocity (clear / 1x1 black) get camera motion from
 * raw device Z and View.ClipToPrevClip inside this same pass.
 * When ReactiveMask is non-null, the pass also writes an R16F reactive mask
 * from |MV| (MRT, no extra fullscreen pass).
 */
FRDGTextureRef AddMagicSRDecodeVelocityPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef EncodedVelocity,
	FRDGTextureRef SceneDepth,
	const FIntRect& ViewRect,
	const FIntPoint& OutputExtent,
	FRDGTextureRef ReactiveMask,
	float ReactiveStartPx,
	float ReactiveSatPx,
	const FMagicSRDecodeVelocityExtras& Extras = FMagicSRDecodeVelocityExtras());

/** Copy SceneDepth device-Z into R32F (matches SceneCapture SCS_DeviceDepth path). */
FRDGTextureRef AddMagicSRCopyDepthPass(
	FRDGBuilder& GraphBuilder,
	FScreenPassViewInfo ViewInfo,
	FRDGTextureRef SceneDepth,
	const FIntRect& ViewRect,
	const FIntPoint& OutputExtent);
