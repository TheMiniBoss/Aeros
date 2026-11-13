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

	/** Transform that stretches the engine cylinder (1 m tall, 1 m wide, centred pivot) from A to B. */
	FTransform SegmentTransform(const FVector& A, const FVector& B, float WidthCm)
	{
		const FVector D = B - A;
		const float Len = float(D.Size());
		return FTransform(FRotationMatrix::MakeFromZ(D.GetSafeNormal()).Rotator(), (A + B) * 0.5,
			FVector(WidthCm / 100.f, WidthCm / 100.f, FMath::Max(Len, 1.f) / 100.f));
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
	const float Target = StormData.MaxRainRate > 2.f ? FMath::Clamp(StormData.MaxRainRate / 80.f, 0.15f, 0.65f) : 0.f;
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

	static ConstructorHelpers::FObjectFinder<UStaticMesh> BoltMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	Bolt = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Bolt"));
	Bolt->SetupAttachment(RootComponent);
	Bolt->SetUsingAbsoluteLocation(true);
	Bolt->SetUsingAbsoluteRotation(true);
	Bolt->SetUsingAbsoluteScale(true);
	Bolt->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Bolt->SetCastShadow(false);
	Bolt->SetVisibility(false);
	if (BoltMesh.Succeeded()) Bolt->SetStaticMesh(BoltMesh.Object);
}

void AAerosLightningFlash::BuildBolt(const FVector& Top, const FVector& Ground, int32 Seed)
{
	FRandomStream Rand(Seed);
	TArray<FTransform> Segments;
	const int32 N = 18;
	const float Height = float(Top.Z - Ground.Z);
	const float Step = Height / N;

	// Main channel: a random walk downwards that ends exactly on the strike point
	TArray<FVector> Points;
	Points.Add(Top);
	FVector Offset = FVector::ZeroVector;
	for (int32 K = 1; K < N; ++K)
	{
		Offset += FVector(Rand.FRandRange(-1.f, 1.f), Rand.FRandRange(-1.f, 1.f), 0.f) * Step * 0.45f;
		const float T = float(K) / N;
		Points.Add(FMath::Lerp(Top, Ground, T) + Offset * (1.f - T * T));
	}
	Points.Add(Ground);
	for (int32 K = 0; K + 1 < Points.Num(); ++K)
	{
		Segments.Add(AerosStormSystemInternal::SegmentTransform(Points[K], Points[K + 1], 350.f));
	}

	// Two branches that fork off the upper half and die out
	for (int32 B = 0; B < 2; ++B)
	{
		FVector P = Points[Rand.RandRange(2, N / 2)];
		const FVector Dir = FVector(Rand.FRandRange(-1.f, 1.f), Rand.FRandRange(-1.f, 1.f), 0.f).GetSafeNormal();
		for (int32 K = 0; K < 6; ++K)
		{
			const FVector Next = P + Dir * Step * Rand.FRandRange(0.4f, 0.9f) + FVector(0, 0, -Step * Rand.FRandRange(0.5f, 1.f));
			Segments.Add(AerosStormSystemInternal::SegmentTransform(P, Next, 160.f - K * 20.f));
			P = Next;
		}
	}

	Bolt->ClearInstances();
	Bolt->AddInstances(Segments, false, true);
	Bolt->SetVisibility(true);
}

void AAerosLightningFlash::Fire(const FAerosLightningStrike& Strike, const FVector& ViewerLocation)
{
	const FVector Ground = AerosStormSystemInternal::GroundAt(GetWorld(), Strike.Location);
	SetActorLocation(Ground + FVector(0, 0, Strike.bCloudToGround ? 150000.0 : 600000.0));
	Peak = Strike.PeakCurrent * (Strike.bCloudToGround ? 4.0e5f : 1.5e5f);
	Age = 0.f;

	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();
	if (UNiagaraSystem* BoltSystem = Settings->LightningSystem.LoadSynchronous(); BoltSystem && Strike.bCloudToGround)
	{
		if (UNiagaraComponent* NC = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, BoltSystem, Ground))
		{
			NC->SetVariableVec3(TEXT("Start"), Ground + FVector(0, 0, 150000.0));
			NC->SetVariableVec3(TEXT("End"), Ground);
		}
	}
	else if (Settings->bUseFallbackVisuals && Strike.bCloudToGround)
	{
		if (!BoltMID)
		{
			if (UMaterialInterface* Mat = AerosStormSystemInternal::LoadEffectMaterial(TEXT("/AerosWeather/Materials/M_AerosEmissive.M_AerosEmissive")))
			{
				Bolt->SetMaterial(0, Mat);
			}
			BoltMID = Bolt->CreateDynamicMaterialInstance(0);
		}
		BuildBolt(Ground + FVector(0, 0, 150000.0), Ground, Strike.Sequence);
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
	if (Bolt->IsVisible())
	{
		// The channel glows with the return strokes, then disappears
		if (BoltMID)
		{
			BoltMID->SetScalarParameterValue(TEXT("Intensity"), 60.f * Pulse);
			BoltMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.8f, 0.85f, 1.f));
		}
		if (Pulse < 0.03f && Age > 0.25f) Bolt->SetVisibility(false);
	}

	if (!bThunderPlayed && Age >= ThunderDelay)
	{
		bThunderPlayed = true;
		if (USoundBase* Snd = UAerosWeatherSettings::Get()->ThunderSound.LoadSynchronous())
		{
			UGameplayStatics::PlaySoundAtLocation(this, Snd, GetActorLocation(), ThunderVolume);
		}
	}
}
