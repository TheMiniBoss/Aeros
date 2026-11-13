// Copyright Aeros. All Rights Reserved.
//
// Network strategy (roadmap risk #1 "Net Saturation"):
//   The grid is NEVER replicated. Server and clients run the same deterministic simulation
//   (same seed, same settings, same timed commands). Only this small actor is replicated:
//   settings + seed, the server clock, and the list of timed commands. Storm / tornado /
//   hurricane actors replicate their own macro state.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "AerosTypes.h"
#include "AerosWeatherNetState.generated.h"

UENUM()
enum class EAerosNetCommand : uint8
{
	SetScenario,
	ThermalBubble,
	Hurricane,
	Recenter,
	SetClock,
	CustomSynoptic,
	WaterRegions,
	InjectRain,
	InjectOutflow,
	Tsunami,
	VolcanicEmission,
	SetLatitude,
	DesertRegions,
};

USTRUCT()
struct FAerosNetCommandData
{
	GENERATED_BODY()

	UPROPERTY() int32 Sequence = 0;
	UPROPERTY() uint8 Type = 0;
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FVector4 Params = FVector4(0, 0, 0, 0);
	UPROPERTY() double ApplyAt = 0.0;
};

UCLASS(NotPlaceable, Transient)
class AEROSWEATHER_API AAerosWeatherNetState : public AInfo
{
	GENERATED_BODY()

public:
	AAerosWeatherNetState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Server only. */
	void ServerInitialize(const FAerosSimulationSettings& InSettings);
	void ServerAddCommand(uint8 Type, const FVector& Location, const FVector4& Params, double ApplyAt);
	void ServerSetSettings(const FAerosSimulationSettings& InSettings);

	UPROPERTY(ReplicatedUsing = OnRep_Settings)
	FAerosSimulationSettings Settings;

	UPROPERTY(ReplicatedUsing = OnRep_Clock)
	double ServerSimTime = 0.0;

	UPROPERTY(Replicated)
	bool bServerPaused = false;

	UPROPERTY(ReplicatedUsing = OnRep_Commands)
	TArray<FAerosNetCommandData> Commands;

	/** Increments every time the simulation is (re)started on the server. */
	UPROPERTY(ReplicatedUsing = OnRep_Settings)
	int32 Epoch = 0;

	int32 LastAppliedSequence = 0;
	double ClockReceivedWallTime = 0.0;

protected:
	UFUNCTION() void OnRep_Settings();
	UFUNCTION() void OnRep_Clock();
	UFUNCTION() void OnRep_Commands();

private:
	int32 NextSequence = 1;
	float ClockPushTimer = 0.f;
};
