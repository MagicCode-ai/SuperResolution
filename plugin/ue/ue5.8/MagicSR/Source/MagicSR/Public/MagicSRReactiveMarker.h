#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MagicSRReactiveMarker.generated.h"

class UPrimitiveComponent;
class ULevel;
class IConsoleVariable;

/**
 * Reactive marking for content that changes without motion vectors (TV / video wall / LED screen
 * textures). Marked components write a reserved CustomDepth stencil value; the MagicSR velocity
 * pass raises the reactive mask there (r.MagicSR.StencilReactive, default on), so Speed's
 * still-accumulation path drops its history on those pixels.
 *
 * Requires r.CustomDepth = 3 (CustomDepth with stencil). Marking a component raises it to 3 at
 * runtime if needed (one-time scene proxy re-registration hitch); set it in DefaultEngine.ini to avoid.
 */
namespace MagicSRReactive
{
/** Reserved stencil value (r.MagicSR.StencilReactiveValue, default 200). */
MAGICSR_API int32 GetStencilValue();

/**
 * Render-thread snapshot of the shader-side settings. Returns false when stencil reactive is off.
 */
MAGICSR_API bool GetRenderThreadParams(uint32& OutStencilValue, float& OutStrength);

/** Mark / unmark one component (game thread). Unmark only clears components still carrying our value. */
MAGICSR_API void SetMarker(UPrimitiveComponent* Component, bool bMarked);

/** Mark every mesh component of actors matching r.MagicSR.StencilReactiveActors or tag MagicSR.Reactive. */
MAGICSR_API int32 ApplyAutoMarks(UWorld* World);
} // namespace MagicSRReactive

/** Applies auto-marking at world begin, level streaming-in, actor spawn and CVar change. */
UCLASS()
class UMagicSRReactiveMarkerSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

private:
	void OnActorSpawned(AActor* Actor);
	void OnLevelAdded(ULevel* Level, UWorld* World);
	void OnActorsCVarChanged(IConsoleVariable* Var);

	FDelegateHandle ActorSpawnedHandle;
	FDelegateHandle LevelAddedHandle;
	FDelegateHandle CVarChangedHandle;
	bool bBegun = false;
};
