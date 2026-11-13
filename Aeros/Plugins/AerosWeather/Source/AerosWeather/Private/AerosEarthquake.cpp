// Copyright Aeros. All Rights Reserved.

#include "AerosEarthquake.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosHazardManager.h"
#include "AerosAssetUtils.h"
#include "AerosWeatherModule.h"
#include "Core/AerosPhysics.h"
#include "Components/AudioComponent.h"
#include "Field/FieldSystemComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"

namespace AerosEarthquakeInternal
{
	constexpr float PWaveKmS = 6.f;    // km/s P waves
	constexpr float SWaveKmS = 3.5f;   // km/s S waves
}
using AerosEarthquakeInternal::PWaveKmS;
using AerosEarthquakeInternal::SWaveKmS;

AAerosEarthquake::AAerosEarthquake()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	SetNetUpdateFrequency(2.f);

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	FieldSystem = CreateDefaultSubobject<UFieldSystemComponent>(TEXT("FieldSystem"));
	FieldSystem->SetupAttachment(RootComponent);
	RumbleAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("Rumble"));
	RumbleAudio->SetupAttachment(RootComponent);
	RumbleAudio->SetAutoActivate(false);
}

void AAerosEarthquake::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAerosEarthquake, Magnitude);
	DOREPLIFETIME(AAerosEarthquake, DepthKm);
	DOREPLIFETIME(AAerosEarthquake, StartServerTime);
	DOREPLIFETIME(AAerosEarthquake, Duration);
	DOREPLIFETIME(AAerosEarthquake, bTsunamigenic);
	DOREPLIFETIME(AAerosEarthquake, bAftershock);
}

double AAerosEarthquake::Now() const
{
	const UWorld* World = GetWorld();
	if (!World) return 0.0;
	if (const AGameStateBase* GS = World->GetGameState()) return GS->GetServerWorldTimeSeconds();
	return World->GetTimeSeconds();
}

void AAerosEarthquake::ServerInitialize(float InMagnitude, float InDepthKm, bool bInAftershock)
{
	Magnitude = FMath::Clamp(InMagnitude, 1.f, 9.6f);
	DepthKm = FMath::Clamp(InDepthKm, 1.f, 700.f);
	bAftershock = bInAftershock;
	// Strong-motion duration grows with rupture length (~ 10^(0.5 M))
	Duration = FMath::Clamp(2.f * FMath::Pow(10.f, 0.5f * Magnitude - 2.3f), 4.f, 240.f);
	StartServerTime = Now() + 0.1;

	// Submarine, shallow and large: the sea floor moves -> tsunami
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		const FAerosWeatherSample Here = Sub->GetWeatherAtLocation(GetActorLocation());
		bTsunamigenic = Here.bOcean && Magnitude >= 7.f && DepthKm < 70.f;
		if (bTsunamigenic) Sub->TriggerTsunami(GetActorLocation(), Magnitude);
	}
	if (!bAftershock) ScheduleAftershocks();
}

void AAerosEarthquake::ScheduleAftershocks()
{
	// Bath's law: largest aftershock ~1.2 below the main shock; Omori: rate decays ~1/t
	const int32 Count = FMath::Clamp(int32(FMath::Pow(10.f, Magnitude - 5.5f)), 0, 6);
	FRandomStream Rand(int32(Magnitude * 1000.f) ^ int32(GetActorLocation().X));
	for (int32 K = 0; K < Count; ++K)
	{
		AftershockTimes.Add(Duration + 20.f * FMath::Pow(Rand.FRandRange(1.f, 12.f), 1.6f));
		AftershockMagnitudes.Add(Magnitude - 1.2f - Rand.FRandRange(0.f, 1.5f) * (K > 0 ? 1.f : 0.3f));
	}
}

void AAerosEarthquake::BeginPlay()
{
	Super::BeginPlay();
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (UAerosHazardManager* HM = Sub->GetHazardManager()) HM->RegisterEarthquake(this);
	}
	if (USoundBase* Snd = Aeros::LoadOptional(UAerosWeatherSettings::Get()->EarthquakeSound))
	{
		RumbleAudio->SetSound(Snd);
	}
	// Lives until the slowest waves have crossed the simulated region
	SetLifeSpan(Duration * 2.f + 300.f / SWaveKmS + 30.f + (AftershockTimes.Num() > 0 ? AftershockTimes.Last() : 0.f));
}

void AAerosEarthquake::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (UAerosHazardManager* HM = Sub->GetHazardManager()) HM->UnregisterEarthquake(this);
	}
	Super::EndPlay(Reason);
}

FAerosEarthquakeData AAerosEarthquake::GetData() const
{
	FAerosEarthquakeData D;
	D.Epicenter = GetActorLocation();
	D.Magnitude = Magnitude;
	D.DepthKm = DepthKm;
	D.DurationSeconds = Duration;
	D.bTsunamigenic = bTsunamigenic;
	return D;
}

float AAerosEarthquake::GetPeakAccelerationAt(const FVector& Location) const
{
	const float EpiKm = float(FVector::Dist2D(Location, GetActorLocation())) * 1e-5f;
	const float R = FMath::Sqrt(EpiKm * EpiKm + DepthKm * DepthKm);
	// Attenuation (simplified Campbell-type): log10 PGA[g] = 0.5 M - 1.3 log10(R + 10) - 1.8
	return FMath::Min(FMath::Pow(10.f, 0.5f * Magnitude - 1.3f * FMath::LogX(10.f, R + 10.f) - 1.8f), 2.f);
}

float AAerosEarthquake::Envelope(float DistanceKm) const
{
	const float T = float(Now() - StartServerTime);
	const float TP = DistanceKm / PWaveKmS, TS = DistanceKm / SWaveKmS;
	if (T < TP) return 0.f;
	if (T < TS) return 0.25f * Aeros::Phys::SmoothStep01((T - TP) / FMath::Max(TS - TP, 0.1f)); // P waves: lighter, vertical
	const float After = T - TS;
	if (After < Duration) return Aeros::Phys::SmoothStep01(After / 1.5f);                    // S + surface waves
	return FMath::Exp(-(After - Duration) / FMath::Max(Duration * 0.4f, 2.f));                  // coda
}

float AAerosEarthquake::GetShakingAt(const FVector& Location) const
{
	const float EpiKm = float(FVector::Dist2D(Location, GetActorLocation())) * 1e-5f;
	const float R = FMath::Sqrt(EpiKm * EpiKm + DepthKm * DepthKm);
	return GetPeakAccelerationAt(Location) * Envelope(R);
}

int32 AAerosEarthquake::MercalliFromPGA(float PGA)
{
	// Wald et al. (1999): MMI = 3.66 log10(PGA cm/s2) - 1.66
	const float MMI = 3.66f * FMath::LogX(10.f, FMath::Max(PGA * 981.f, 0.01f)) - 1.66f;
	return FMath::Clamp(FMath::RoundToInt(MMI), 1, 12);
}

void AAerosEarthquake::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bAnnounced && Now() >= StartServerTime)
	{
		bAnnounced = true;
		if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) Sub->OnEarthquake.Broadcast(GetData());
		UE_LOG(LogAeros, Log, TEXT("Aeros: earthquake M%.1f depth %.0f km%s"), Magnitude, DepthKm, bTsunamigenic ? TEXT(" (tsunamigenic)") : TEXT(""));
	}

	// Rumble follows the local shaking
	if (RumbleAudio->Sound)
	{
		if (const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
		{
			const float Shake = GetShakingAt(Sub->GetViewerLocation());
			if (Shake > 0.01f && !RumbleAudio->IsPlaying()) RumbleAudio->Play();
			if (RumbleAudio->IsPlaying())
			{
				RumbleAudio->SetWorldLocation(Sub->GetViewerLocation());
				RumbleAudio->SetVolumeMultiplier(FMath::Clamp(Shake * 3.f, 0.001f, 1.f));
			}
		}
	}

	if (!HasAuthority()) return;
	ShakePhysics(DeltaSeconds);

	// Aftershocks (server spawns them as new events)
	const float Elapsed = float(Now() - StartServerTime);
	for (int32 K = AftershockTimes.Num() - 1; K >= 0; --K)
	{
		if (Elapsed >= AftershockTimes[K])
		{
			if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
			{
				const FVector Offset(FMath::FRandRange(-1.f, 1.f), FMath::FRandRange(-1.f, 1.f), 0.f);
				if (UAerosHazardManager* HM = Sub->GetHazardManager())
				{
					HM->SpawnEarthquake(GetActorLocation() + Offset * 1500000.0, AftershockMagnitudes[K], DepthKm, true);
				}
			}
			AftershockTimes.RemoveAt(K);
			AftershockMagnitudes.RemoveAt(K);
		}
	}
}

void AAerosEarthquake::ShakePhysics(float DeltaSeconds)
{
	PhysicsTimer += DeltaSeconds;
	StrainTimer += DeltaSeconds;
	if (PhysicsTimer < 0.05f) return;
	PhysicsTimer = 0.f;

	UWorld* World = GetWorld();
	if (!World) return;
	const float Radius = UAerosWeatherSettings::Get()->PhysicsEffectRadiusKm * 100000.f;
	const float T = float(Now() - StartServerTime);

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr;
		if (!Pawn) continue;
		const FVector Center = Pawn->GetActorLocation();
		const float PGA = GetShakingAt(Center);
		if (PGA < 0.02f) continue;

		TArray<FOverlapResult> Overlaps;
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_PhysicsBody);
		Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
		Obj.AddObjectTypesToQuery(ECC_Destructible);
		World->OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity, Obj, FCollisionShape::MakeSphere(Radius));
		for (const FOverlapResult& O : Overlaps)
		{
			UPrimitiveComponent* Prim = O.GetComponent();
			if (!Prim || !Prim->IsSimulatingPhysics()) continue;
			// Ground motion: ~1-3 Hz oscillation, each object with its own phase / direction
			const float Seed = float(Prim->GetUniqueID() % 997);
			const float Freq = 1.2f + FMath::Fmod(Seed, 7.f) * 0.25f;
			const FVector Dir(FMath::Cos(Seed), FMath::Sin(Seed), 0.f);
			const float A = PGA * 981.f * FMath::Sin(2.f * PI * Freq * T + Seed);
			Prim->AddForce(Dir * A + FVector(0, 0, 0.4f * A * FMath::Cos(2.f * PI * Freq * 1.3f * T)), NAME_None, true);
		}

		// Unreinforced structures (Geometry Collections) crack above ~0.15 g
		if (StrainTimer > 0.5f && PGA > 0.1f && FieldSystem && UAerosWeatherSettings::Get()->bEnableChaosDestruction)
		{
			FieldSystem->ApplyStrainField(true, Center, Radius, PGA * 800000.f, 1);
		}

		// People lose their balance in strong shaking
		if (PGA > 0.35f && FMath::FRand() < 0.05f)
		{
			if (ACharacter* Char = Cast<ACharacter>(It->Get()->GetPawn()))
			{
				Char->LaunchCharacter(FVector(FMath::FRandRange(-1.f, 1.f), FMath::FRandRange(-1.f, 1.f), 0.6f) * PGA * 250.f, false, false);
			}
		}
	}
	if (StrainTimer > 0.5f) StrainTimer = 0.f;
}

// =====================================================================
//  Camera modifier
// =====================================================================

bool UAerosShakeCameraModifier::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	Super::ModifyCamera(DeltaTime, InOutPOV);
	Time += DeltaTime;
	const UAerosWeatherSubsystem* Sub = CameraOwner ? UAerosWeatherSubsystem::Get(CameraOwner) : nullptr;
	if (!Sub) return false;

	float Target = Sub->GetGroundShakingAt(InOutPOV.Location);
	// Violent wind (tornado, hurricane eyewall) buffets the camera too
	const float Wind = Sub->GetWeatherAtLocation(InOutPOV.Location).WindSpeed;
	Target += FMath::Max(0.f, Wind - 25.f) / 120.f;
	Target *= UAerosWeatherSettings::Get()->CameraShakeScale;
	Smoothed = FMath::FInterpTo(Smoothed, Target, DeltaTime, 6.f);
	if (Smoothed < 0.002f) return false;

	const float A = FMath::Min(Smoothed, 1.5f);
	auto N = [this](float Speed, float Offset) { return FMath::PerlinNoise1D(Time * Speed + Offset); };
	InOutPOV.Location += FVector(N(9.f, 0.f), N(9.f, 37.f), N(11.f, 71.f) * 0.6f) * A * 35.f;
	InOutPOV.Rotation += FRotator(N(7.f, 13.f) * A * 1.8f, N(7.f, 29.f) * A * 1.2f, N(5.f, 53.f) * A * 2.2f);
	return false;
}
