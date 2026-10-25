#pragma once

#include "CoreMinimal.h"
#include "ScreenPass.h"
#include "Templates/RefCounting.h"
#include "TemporalUpscaler.h"

/**
 * UE third-party temporal upscaler: feeds player-view SceneColor/Depth/Velocity +
 * TemporalJitterPixels into MagicSR Temporal (Vulkan).
 */
class FMagicSRTemporalUpscaler final : public UE::Renderer::Private::ITemporalUpscaler
{
public:
	class FHistory final : public IHistory, public TRefCountingMixin<FHistory>
	{
	public:
		virtual const TCHAR* GetDebugName() const override { return GetUpscalerDebugName(); }
		virtual uint64 GetGPUSizeBytes() const override { return 0; }
		virtual void AddRef() const override { TRefCountingMixin<FHistory>::AddRef(); }
		virtual FReturnedRefCountValue Release() const override
		{
			return TRefCountingMixin<FHistory>::Release();
		}
		virtual FReturnedRefCountValue GetRefCount() const override
		{
			return TRefCountingMixin<FHistory>::GetRefCount();
		}
	};

	static const TCHAR* GetUpscalerDebugName() { return TEXT("MagicSR"); }

	FMagicSRTemporalUpscaler() = default;
	virtual ~FMagicSRTemporalUpscaler() = default;

	virtual const TCHAR* GetDebugName() const override { return GetUpscalerDebugName(); }

	virtual FOutputs AddPasses(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FInputs& Inputs) const override;

	virtual float GetMinUpsampleResolutionFraction() const override { return 0.5f; }
	virtual float GetMaxUpsampleResolutionFraction() const override { return 1.0f; }

	virtual ITemporalUpscaler* Fork_GameThread(const class FSceneViewFamily& ViewFamily) const override;
};
