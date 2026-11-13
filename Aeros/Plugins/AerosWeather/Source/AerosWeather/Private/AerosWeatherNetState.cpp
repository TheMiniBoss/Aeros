// Copyright Aeros. All Rights Reserved.

#include "AerosWeatherNetState.h"
#include "AerosWeatherSubsystem.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"

AAerosWeatherNetState::AAerosWeatherNetState()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
	SetNetUpdateFrequency(2.f);
}

void AAerosWeatherNetState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAerosWeatherNetState, Settings);
	DOREPLIFETIME(AAerosWeatherNetState, ServerSimTime);
	DOREPLIFETIME(AAerosWeatherNetState, bServerPaused);
	DOREPLIFETIME(AAerosWeatherNetState, Commands);
	DOREPLIFETIME(AAerosWeatherNetState, Epoch);
}

void AAerosWeatherNetState::BeginPlay()
{
	Super::BeginPlay();
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		Sub->RegisterNetState(this);
	}
}

void AAerosWeatherNetState::ServerInitialize(const FAerosSimulationSettings& InSettings)
{
	Settings = InSettings;
	Commands.Reset();
	LastAppliedSequence = 0;
	ServerSimTime = 0.0;
	++Epoch;
	ForceNetUpdate();
}

void AAerosWeatherNetState::ServerSetSettings(const FAerosSimulationSettings& InSettings)
{
	Settings = InSettings;
	ForceNetUpdate();
}

void AAerosWeatherNetState::ServerAddCommand(uint8 Type, const FVector& Location, const FVector4& Params, double ApplyAt)
{
	FAerosNetCommandData& C = Commands.AddDefaulted_GetRef();
	C.Sequence = NextSequence++;
	C.Type = Type;
	C.Location = Location;
	C.Params = Params;
	C.ApplyAt = ApplyAt;
	LastAppliedSequence = C.Sequence; // the server applies it directly
	if (Commands.Num() > 256)
	{
		// Late joiners replay the kept history; very old commands are dropped.
		Commands.RemoveAt(0, Commands.Num() - 256);
	}
	ForceNetUpdate();
}

void AAerosWeatherNetState::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority()) return;

	ClockPushTimer += DeltaSeconds;
	if (ClockPushTimer >= 1.f)
	{
		ClockPushTimer = 0.f;
		if (const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
		{
			ServerSimTime = Sub->GetSimulationTime();
			bServerPaused = Sub->IsPaused();
			Settings.TimeScale = Sub->GetTimeScale();
		}
	}
}

void AAerosWeatherNetState::OnRep_Settings()
{
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		Sub->ApplyNetState(*this);
	}
}

void AAerosWeatherNetState::OnRep_Clock()
{
	ClockReceivedWallTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		Sub->ApplyNetState(*this);
	}
}

void AAerosWeatherNetState::OnRep_Commands()
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub || !Sub->IsSimulationRunning()) return;
	for (const FAerosNetCommandData& C : Commands)
	{
		if (C.Sequence > LastAppliedSequence)
		{
			Sub->ApplyNetCommand(C.Type, C.Location, C.Params, C.ApplyAt);
			LastAppliedSequence = C.Sequence;
		}
	}
}
