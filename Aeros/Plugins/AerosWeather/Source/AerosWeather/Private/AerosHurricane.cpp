// Copyright Aeros. All Rights Reserved.

#include "AerosHurricane.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosStormManager.h"
#include "Components/AudioComponent.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"

AAerosHurricane::AAerosHurricane()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	SetNetUpdateFrequency(1.f);

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	WindAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("WindAudio"));
	WindAudio->SetupAttachment(RootComponent);
	WindAudio->SetAutoActivate(false);
}

void AAerosHurricane::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAerosHurricane, CycloneData);
}

void AAerosHurricane::BeginPlay()
{
	Super::BeginPlay();
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (Sub->GetStormManager()) Sub->GetStormManager()->RegisterHurricane(this);
	}
}

void AAerosHurricane::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (Sub->GetStormManager()) Sub->GetStormManager()->UnregisterHurricane(this);
	}
	Super::EndPlay(Reason);
}

void AAerosHurricane::ServerUpdateFromSim(const FAerosCycloneData& Data)
{
	CycloneData = Data;
	OnRep_Cyclone();
}

void AAerosHurricane::OnRep_Cyclone()
{
	if (!bHasLocation)
	{
		Smoothed = CycloneData.Location;
		bHasLocation = true;
	}
	if (CycloneData.Category != LastCategory)
	{
		OnCategoryChanged(LastCategory, CycloneData.Category);
		LastCategory = CycloneData.Category;
	}
}

void AAerosHurricane::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	float TimeScale = 60.f;
	const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (Sub) TimeScale = Sub->GetTimeScale();
	Smoothed += CycloneData.Velocity * 100.0 * DeltaSeconds * TimeScale;
	Smoothed = FMath::VInterpTo(Smoothed, CycloneData.Location, DeltaSeconds, 0.5f);
	SetActorLocation(Smoothed);

	// Wind roar grows as the viewer gets close to the eyewall
	if (Sub && WindAudio)
	{
		if (!WindAudio->Sound)
		{
			if (USoundBase* Snd = UAerosWeatherSettings::Get()->WindLoopSound.LoadSynchronous()) WindAudio->SetSound(Snd);
		}
		const float DistKm = float(FVector::Dist2D(Sub->GetViewerLocation(), Smoothed)) * 1e-5f;
		const float Near = FMath::Clamp(1.f - (DistKm - CycloneData.RadiusMaxWindKm) / (CycloneData.RadiusMaxWindKm * 5.f), 0.f, 1.f);
		if (WindAudio->Sound && Near > 0.01f && !WindAudio->IsPlaying()) WindAudio->Play();
		if (WindAudio->IsPlaying())
		{
			WindAudio->SetVolumeMultiplier(FMath::Max(Near, 0.01f));
			WindAudio->SetWorldLocation(Sub->GetViewerLocation());
		}
	}
}
