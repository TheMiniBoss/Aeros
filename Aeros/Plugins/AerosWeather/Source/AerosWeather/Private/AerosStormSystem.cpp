// Copyright Aeros. All Rights Reserved.

#include "AerosStormSystem.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosStormManager.h"
#include "AerosAssetUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "Components/AudioComponent.h"
#include "Components/PointLightComponent.h"
#include "Field/FieldSystemComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"

namespace AerosStormSystemInternal
{
	FVector GroundAt(const UWorld* World, const FVector& Loc)
	{
		if (!World) return Loc;
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosGround), false);
		const FVector Start(Loc.X, Loc.Y, 2000000.0), End(Loc.X, Loc.Y, -2000000.0);
		if (World->LineTraceSingleByObjectType(Hit, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), Params)) return Hit.ImpactPoint; // ignores pawns
		return FVector(Loc.X, Loc.Y, 0.0);
	}

	UMaterialInterface* LoadEffectMaterial(const TCHAR* Path)
	{
		return Aeros::LoadOptional(TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(Path)));
	}
}

AAerosStormSystem::AAerosStormSystem()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	SetNetUpdateFrequency(1.f); // roadmap: macro state only

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	StormVFX = CreateDefaultSubobject<UNiagaraComponent>(TEXT("StormVFX"));
	StormVFX->SetupAttachment(RootComponent);
	StormVFX->SetAutoActivate(false);

	RumbleAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("Rumble"));
	RumbleAudio->SetupAttachment(RootComponent);
	RumbleAudio->SetAutoActivate(false);

	FieldSystem = CreateDefaultSubobject<UFieldSystemComponent>(TEXT("FieldSystem"));
	FieldSystem->SetupAttachment(RootComponent);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	RainShaft = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RainShaft"));
	RainShaft->SetupAttachment(RootComponent);
	RainShaft->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RainShaft->SetCastShadow(false);
	RainShaft->SetVisibility(false);
	if (CylinderMesh.Succeeded()) RainShaft->SetStaticMesh(CylinderMesh.Object);
}

void AAerosStormSystem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAerosStormSystem, StormIntensity);
	DOREPLIFETIME(AAerosStormSystem, StormData);
}

void AAerosStormSystem::BeginPlay()
{
	Super::BeginPlay();
	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();
	if (UNiagaraSystem* Sys = Settings->StormSystem.LoadSynchronous())
	{
		StormVFX->SetAsset(Sys);
		StormVFX->Activate();
	}
	else if (Settings->bUseFallbackVisuals && GetNetMode() != NM_DedicatedServer)
	{
		// Rain shaft: needs the translucent material from aeros_setup.py (an opaque grey wall would hide the storm)
		if (UMaterialInterface* Mat = AerosStormSystemInternal::LoadEffectMaterial(TEXT("/AerosWeather/Materials/M_AerosShaft.M_AerosShaft")))
		{
			RainShaft->SetMaterial(0, Mat);
			ShaftMID = RainShaft->CreateDynamicMaterialInstance(0);
		}
	}
	if (USoundBase* Snd = Settings->WindLoopSound.LoadSynchronous())
	{
		RumbleAudio->SetSound(Snd);
		RumbleAudio->Play();
	}
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (Sub->GetStormManager()) Sub->GetStormManager()->RegisterStormActor(this);
	}
	OnRep_StormIntensity();
}

void AAerosStormSystem::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (Sub->GetStormManager()) Sub->GetStormManager()->UnregisterStormActor(this);
	}
	Super::EndPlay(Reason);
}

void AAerosStormSystem::ServerUpdateFromSim(const FAerosStormData& Data)
{
	StormData = Data;
	StormIntensity = Data.Intensity;
	OnRep_StormData();
	OnRep_StormIntensity();
}

void AAerosStormSystem::OnRep_StormIntensity()
{
	// Roadmap: SpawnRate driven by intensity
	if (StormVFX && StormVFX->GetAsset())
	{
		StormVFX->SetVariableFloat(TEXT("SpawnRate"), StormIntensity * 1000.f);
		StormVFX->SetVariableFloat(TEXT("Intensity"), StormIntensity);
	}
	if (RumbleAudio && RumbleAudio->IsPlaying())
	{
		RumbleAudio->SetVolumeMultiplier(FMath::Clamp(StormIntensity, 0.05f, 1.f));
	}
}

void AAerosStormSystem::OnRep_StormData()
{
	if (!bHasLocation)
	{
		SmoothedLocation = StormData.Location;
		bHasLocation = true;
	}
	if (StormVFX && StormVFX->GetAsset())
	{
		StormVFX->SetVariableFloat(TEXT("HailSize"), StormData.MaxHail);
		StormVFX->SetVariableFloat(TEXT("RadiusKm"), StormData.RadiusKm);
		StormVFX->SetVariableFloat(TEXT("Mesocyclone"), StormData.Mesocyclone);
	}
	OnStormUpdated(StormData);
}

void AAerosStormSystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Extrapolate with storm motion between 1 Hz updates, then pull toward the authoritative spot
	float TimeScale = 60.f;
	if (const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) TimeScale = Sub->GetTimeScale();
	SmoothedLocation += StormData.Velocity * 100.0 * DeltaSeconds * TimeScale;
	SmoothedLocation = FMath::VInterpTo(SmoothedLocation, StormData.Location, DeltaSeconds, 0.5f);
	SetActorLocation(FVector(SmoothedLocation.X, SmoothedLocation.Y, CloudBaseHeight));
	UpdateRainShaft(DeltaSeconds);

	if (HasAuthority())
	{
		DamageTimer += DeltaSeconds;
		if (DamageTimer >= 2.f)
		{
			DamageTimer = 0.f;
			ProcessExtremeHailDamage();
		}
	}
}

void AAerosStormSystem::UpdateRainShaft(float DeltaSeconds)
{
	if (!ShaftMID) return;
	// Heavier rain = darker, denser curtain; hail turns it whitish. Fades in and out smoothly.
	float Target = StormData.MaxRainRate > 2.f ? FMath::Clamp(StormData.MaxRainRate / 80.f, 0.15f, 0.65f) : 0.f;
	// Translucent meshes draw over the volumetric clouds (no depth test against them): seen from above
	// the cloud base, the curtain showed through the storm as a flat ellipse. Up there the real rain is
	// hidden by the cloud anyway.
	if (const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		const double ViewerAboveBase = Sub->GetViewerLocation().Z - GetActorLocation().Z;
		Target *= 1.f - FMath::Clamp(float(ViewerAboveBase) / 100000.f, 0.f, 1.f); // gone 1 km above the base
	}
	ShaftOpacity = FMath::FInterpTo(ShaftOpacity, Target, DeltaSeconds, 0.5f);
	const bool bVisible = ShaftOpacity > 0.01f;
	RainShaft->SetVisibility(bVisible);
	if (!bVisible) return;

	// Precipitation core: about half the storm radius, 2-8 km wide, from the ground to the cloud base
	const float DiameterCm = FMath::Clamp(StormData.RadiusKm * 0.5f, 1.f, 4.f) * 2.f * 100000.f;
	RainShaft->SetRelativeLocation(FVector(0, 0, -CloudBaseHeight * 0.5f));
	RainShaft->SetRelativeScale3D(FVector(DiameterCm / 100.f, DiameterCm / 100.f, CloudBaseHeight / 100.f));
	const float Hail = FMath::Clamp(StormData.MaxHail / 40.f, 0.f, 1.f);
	ShaftMID->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(FLinearColor(0.16f, 0.18f, 0.22f), FLinearColor(0.55f, 0.6f, 0.68f), Hail));
	ShaftMID->SetScalarParameterValue(TEXT("Opacity"), ShaftOpacity);
}

void AAerosStormSystem::ProcessExtremeHailDamage()
{
	const float Threshold = UAerosWeatherSettings::Get()->HailDamageThreshold;
	if (StormData.MaxHail <= Threshold) return;

	const FVector Center = AerosStormSystemInternal::GroundAt(GetWorld(), StormData.HailCoreLocation);
	const float DamageRadius = 5000.f;  // roadmap values
	const float DamageAmount = StormData.MaxHail * 0.5f;

	// The hail core is ~one grid cell wide: hit random spots inside it so the damage is spread out.
	const float CellHalf = 0.5f * StormData.RadiusKm * 100000.f;
	const FVector Jitter(FMath::FRandRange(-CellHalf, CellHalf) * 0.3, FMath::FRandRange(-CellHalf, CellHalf) * 0.3, 0.0);
	const FVector Point = Center + Jitter;

	TArray<AActor*> Ignored;
	UGameplayStatics::ApplyRadialDamage(this, DamageAmount, Point, DamageRadius, UAerosHailDamageType::StaticClass(),
		Ignored, this, nullptr, true);

	if (UAerosWeatherSettings::Get()->bEnableChaosDestruction)
	{
		TriggerChaosFracturesInRadius(Point, DamageRadius, DamageAmount);
	}
}

void AAerosStormSystem::TriggerChaosFracturesInRadius(const FVector& Center, float Radius, float Damage)
{
	if (!FieldSystem) return;
	// Strain breaks Geometry Collection clusters whose damage threshold is exceeded.
	FieldSystem->ApplyStrainField(true, Center, Radius, Damage * 20000.f, 1);
}
