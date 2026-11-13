// Copyright Aeros. All Rights Reserved.

#include "AerosStormSystem.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosStormManager.h"
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

namespace
{
	FVector GroundAt(const UWorld* World, const FVector& Loc)
	{
		if (!World) return Loc;
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosGround), false);
		const FVector Start(Loc.X, Loc.Y, 2000000.0), End(Loc.X, Loc.Y, -2000000.0);
		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params)) return Hit.ImpactPoint;
		return FVector(Loc.X, Loc.Y, 0.0);
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

void AAerosStormSystem::ProcessExtremeHailDamage()
{
	const float Threshold = UAerosWeatherSettings::Get()->HailDamageThreshold;
	if (StormData.MaxHail <= Threshold) return;

	const FVector Center = GroundAt(GetWorld(), StormData.HailCoreLocation);
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

// =====================================================================
//  Lightning flash
// =====================================================================

AAerosLightningFlash::AAerosLightningFlash()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	RootComponent = Light;
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(0.f);
	Light->SetAttenuationRadius(2000000.f);
	Light->SetLightColor(FLinearColor(0.85f, 0.88f, 1.f));
	Light->SetCastShadows(false);
	Light->SetMobility(EComponentMobility::Movable);
}

void AAerosLightningFlash::Fire(const FAerosLightningStrike& Strike, const FVector& ViewerLocation)
{
	const FVector Ground = GroundAt(GetWorld(), Strike.Location);
	SetActorLocation(Ground + FVector(0, 0, Strike.bCloudToGround ? 150000.0 : 600000.0));
	Peak = Strike.PeakCurrent * (Strike.bCloudToGround ? 4.0e5f : 1.5e5f);
	Age = 0.f;

	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();
	if (UNiagaraSystem* Bolt = Settings->LightningSystem.LoadSynchronous(); Bolt && Strike.bCloudToGround)
	{
		if (UNiagaraComponent* NC = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, Bolt, Ground))
		{
			NC->SetVariableVec3(TEXT("Start"), Ground + FVector(0, 0, 150000.0));
			NC->SetVariableVec3(TEXT("End"), Ground);
		}
	}

	// Thunder: speed of sound, real time (sound does not care about the weather time scale)
	const float DistM = float(FVector::Dist(Ground, ViewerLocation)) * 0.01f;
	ThunderDelay = DistM / 343.f;
	ThunderVolume = FMath::Clamp(1.f - DistM / (Settings->LightningVisualRangeKm * 1000.f), 0.1f, 1.f);
	bThunderPlayed = false;
	SetLifeSpan(FMath::Max(1.f, ThunderDelay + 0.5f));
}

void AAerosLightningFlash::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	// Return strokes: 3 quick pulses
	const float Pulse = FMath::Max3(FMath::Exp(-Age * 40.f), 0.6f * FMath::Exp(-FMath::Abs(Age - 0.09f) * 60.f), 0.4f * FMath::Exp(-FMath::Abs(Age - 0.2f) * 60.f));
	Light->SetIntensity(Peak * Pulse);

	if (!bThunderPlayed && Age >= ThunderDelay)
	{
		bThunderPlayed = true;
		if (USoundBase* Snd = UAerosWeatherSettings::Get()->ThunderSound.LoadSynchronous())
		{
			UGameplayStatics::PlaySoundAtLocation(this, Snd, GetActorLocation(), ThunderVolume);
		}
	}
}
