#include "MagicSRReactiveMarker.h"

#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"

namespace
{
static TAutoConsoleVariable<int32> CVarMagicSRStencilReactive(
	TEXT("r.MagicSR.StencilReactive"),
	1,
	TEXT("1 = raise the MagicSR reactive mask on pixels whose CustomDepth stencil equals r.MagicSR.StencilReactiveValue "
		 "(components marked via SetMagicSRReactiveMarker / r.MagicSR.StencilReactiveActors / tag MagicSR.Reactive). "
		 "Needs r.MagicSR.ReactiveFromMv=1. Unmarked pixels are unchanged."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarMagicSRStencilReactiveStrength(
	TEXT("r.MagicSR.StencilReactiveStrength"),
	1.0f,
	TEXT("Reactive amount on marked pixels (0..1). Speed's still path only honors strong values (use 0 or >= 0.9)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMagicSRStencilReactiveValue(
	TEXT("r.MagicSR.StencilReactiveValue"),
	200,
	TEXT("Reserved CustomDepth stencil value (1..255) used for marking. Applies to components marked afterwards. "
		 "UE never assigns CustomDepth stencil values itself; pick another value only if your content uses 200."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<FString> CVarMagicSRStencilReactiveActors(
	TEXT("r.MagicSR.StencilReactiveActors"),
	TEXT(""),
	TEXT("Comma-separated, case-insensitive substrings of actor names (or editor labels). Mesh components of matching "
		 "actors are marked reactive at world begin, level streaming-in and actor spawn. Default empty."),
	ECVF_Default);

const FName MagicSRReactiveTag(TEXT("MagicSR.Reactive"));

int32 ClampedStencilValue()
{
	return FMath::Clamp(CVarMagicSRStencilReactiveValue.GetValueOnAnyThread(), 1, 255);
}

void EnsureCustomDepthStencilMode()
{
	IConsoleVariable* CustomDepth = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
	if (CustomDepth && CustomDepth->GetInt() < 3)
	{
		UE_LOG(LogTemp, Display,
			   TEXT("[MagicSR] REACTIVE_MARK raising r.CustomDepth %d -> 3 (stencil writes). Set r.CustomDepth=3 in "
					"DefaultEngine.ini to avoid the one-time re-registration hitch."),
			   CustomDepth->GetInt());
		CustomDepth->Set(3, ECVF_SetByCode);
	}
}

bool ActorMatches(const AActor* Actor, const TArray<FString>& Needles)
{
	if (Actor->ActorHasTag(MagicSRReactiveTag))
	{
		return true;
	}
	if (Needles.Num() == 0)
	{
		return false;
	}
	const FString Name = Actor->GetName();
#if WITH_EDITOR
	const FString Label = Actor->GetActorLabel();
#endif
	for (const FString& N : Needles)
	{
		if (Name.Contains(N, ESearchCase::IgnoreCase)
#if WITH_EDITOR
			|| Label.Contains(N, ESearchCase::IgnoreCase)
#endif
		)
		{
			return true;
		}
	}
	return false;
}

void ParseNeedles(TArray<FString>& Out)
{
	CVarMagicSRStencilReactiveActors.GetValueOnGameThread().ParseIntoArray(Out, TEXT(","), true);
	for (FString& S : Out)
	{
		S.TrimStartAndEndInline();
	}
	Out.RemoveAll([](const FString& S) { return S.IsEmpty(); });
}

int32 MarkActor(AActor* Actor)
{
	int32 Count = 0;
	TInlineComponentArray<UMeshComponent*> Meshes;
	Actor->GetComponents(Meshes);
	for (UMeshComponent* Mesh : Meshes)
	{
		MagicSRReactive::SetMarker(Mesh, true);
		++Count;
	}
	if (Count > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("[MagicSR] REACTIVE_MARK actor=%s mesh_components=%d stencil=%d"),
			   *Actor->GetName(), Count, ClampedStencilValue());
	}
	return Count;
}
} // namespace

namespace MagicSRReactive
{
int32 GetStencilValue()
{
	return ClampedStencilValue();
}

bool GetRenderThreadParams(uint32& OutStencilValue, float& OutStrength)
{
	OutStencilValue = static_cast<uint32>(FMath::Clamp(CVarMagicSRStencilReactiveValue.GetValueOnRenderThread(), 1, 255));
	OutStrength = FMath::Clamp(CVarMagicSRStencilReactiveStrength.GetValueOnRenderThread(), 0.f, 1.f);
	return CVarMagicSRStencilReactive.GetValueOnRenderThread() != 0 && OutStrength > 0.f;
}

void SetMarker(UPrimitiveComponent* Component, bool bMarked)
{
	if (!IsValid(Component))
	{
		return;
	}
	if (bMarked)
	{
		EnsureCustomDepthStencilMode();
		Component->SetCustomDepthStencilValue(ClampedStencilValue());
		Component->SetRenderCustomDepth(true);
	}
	else if (Component->CustomDepthStencilValue == ClampedStencilValue())
	{
		Component->SetRenderCustomDepth(false);
		Component->SetCustomDepthStencilValue(0);
	}
}

int32 ApplyAutoMarks(UWorld* World)
{
	if (!World)
	{
		return 0;
	}
	TArray<FString> Needles;
	ParseNeedles(Needles);
	int32 Total = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (ActorMatches(*It, Needles))
		{
			Total += MarkActor(*It);
		}
	}
	return Total;
}
} // namespace MagicSRReactive

bool UMagicSRReactiveMarkerSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->IsGameWorld());
}

void UMagicSRReactiveMarkerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MagicSR.StencilReactiveActors")))
	{
		CVarChangedHandle = Var->OnChangedDelegate().AddUObject(this, &UMagicSRReactiveMarkerSubsystem::OnActorsCVarChanged);
	}
}

void UMagicSRReactiveMarkerSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
	}
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
	if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MagicSR.StencilReactiveActors")))
	{
		Var->OnChangedDelegate().Remove(CVarChangedHandle);
	}
	Super::Deinitialize();
}

void UMagicSRReactiveMarkerSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	bBegun = true;
	ActorSpawnedHandle = InWorld.AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateUObject(this, &UMagicSRReactiveMarkerSubsystem::OnActorSpawned));
	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(this, &UMagicSRReactiveMarkerSubsystem::OnLevelAdded);
	const int32 N = MagicSRReactive::ApplyAutoMarks(&InWorld);
	UE_LOG(LogTemp, Display, TEXT("[MagicSR] REACTIVE_MARK world_begin marked_components=%d actors_cvar=\"%s\""), N,
		   *CVarMagicSRStencilReactiveActors.GetValueOnGameThread());
}

void UMagicSRReactiveMarkerSubsystem::OnActorSpawned(AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return;
	}
	TArray<FString> Needles;
	ParseNeedles(Needles);
	if (ActorMatches(Actor, Needles))
	{
		MarkActor(Actor);
	}
}

void UMagicSRReactiveMarkerSubsystem::OnLevelAdded(ULevel* Level, UWorld* World)
{
	if (!bBegun || !Level || World != GetWorld())
	{
		return;
	}
	TArray<FString> Needles;
	ParseNeedles(Needles);
	for (AActor* Actor : Level->Actors)
	{
		if (IsValid(Actor) && ActorMatches(Actor, Needles))
		{
			MarkActor(Actor);
		}
	}
}

void UMagicSRReactiveMarkerSubsystem::OnActorsCVarChanged(IConsoleVariable* /*Var*/)
{
	if (bBegun)
	{
		MagicSRReactive::ApplyAutoMarks(GetWorld());
	}
}
