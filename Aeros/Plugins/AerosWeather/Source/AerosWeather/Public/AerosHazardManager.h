// Copyright Aeros. All Rights Reserved.
//
// UAerosHazardManager - geological and hydrological disasters on the game side:
//   * earthquakes (spawn, spontaneous seismicity, shaking queries, camera shake),
//   * physics in flood water (buoyancy, current) around players,
//   * volcano registry.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AerosTypes.h"
#include "AerosHazardManager.generated.h"

class UAerosWeatherSubsystem;
class AAerosEarthquake;
class AAerosVolcano;

UCLASS()
class AEROSWEATHER_API UAerosHazardManager : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UAerosWeatherSubsystem* InOwner);
	void Shutdown();
	void Tick(float DeltaTime);

	/** Server: spawn an earthquake actor (replicated). */
	AAerosEarthquake* SpawnEarthquake(const FVector& Epicenter, float Magnitude, float DepthKm, bool bAftershock = false);

	/** Peak ground acceleration (g) right now at a location, all quakes combined. */
	float GetShakingAt(const FVector& Location) const;

	TArray<AAerosVolcano*> GetVolcanoes() const;
	TArray<AAerosEarthquake*> GetEarthquakes() const;

	void RegisterEarthquake(AAerosEarthquake* Quake);
	void UnregisterEarthquake(AAerosEarthquake* Quake);
	void RegisterVolcano(AAerosVolcano* Volcano);
	void UnregisterVolcano(AAerosVolcano* Volcano);

	UAerosWeatherSubsystem* GetOwnerSubsystem() const { return Owner; }

private:
	void UpdateSpontaneousSeismicity();
	void UpdateFloodPhysics(float DeltaTime);
	void EnsureCameraModifiers();

	UPROPERTY(Transient) TObjectPtr<UAerosWeatherSubsystem> Owner;
	UPROPERTY(Transient) TArray<TObjectPtr<AAerosEarthquake>> Earthquakes;
	UPROPERTY(Transient) TArray<TObjectPtr<AAerosVolcano>> Volcanoes;

	int64 LastSeismicMinute = -1;
	float FloodTimer = 0.f;
	float ModifierTimer = 0.f;
};
