// Copyright Aeros. All Rights Reserved.
//
// UAerosStormManager - roadmap "UStormManager". Reads the storms detected by the simulation
// and turns the important ones into actors (LOD: only near players), rolls tornado genesis
// with the shared seed, follows hurricanes and shows lightning.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AerosTypes.h"
#include "AerosStormManager.generated.h"

class UAerosWeatherSubsystem;
class AAerosStormSystem;
class AAerosTornado;
class AAerosHurricane;

UCLASS()
class AEROSWEATHER_API UAerosStormManager : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UAerosWeatherSubsystem* InOwner);
	void Shutdown();
	void ClearActors();
	void Tick(float DeltaTime);

	void HandleLightning(const FAerosLightningStrike& Strike);
	AAerosTornado* ForceTornado(int32 StormId);

	FVector GetVortexWindAt(const FVector& WorldLocation) const;
	bool IsInsideTornado(const FVector& WorldLocation, float RadiusMultiplier) const;
	TArray<AAerosTornado*> GetTornadoes() const;
	AAerosStormSystem* FindStormActor(int32 StormId) const;

	// Actors self-register so clients see replicated ones too
	void RegisterStormActor(AAerosStormSystem* Actor);
	void UnregisterStormActor(AAerosStormSystem* Actor);
	void RegisterTornado(AAerosTornado* Tornado);
	void UnregisterTornado(AAerosTornado* Tornado);
	void RegisterHurricane(AAerosHurricane* Hurricane);
	void UnregisterHurricane(AAerosHurricane* Hurricane);

private:
	void UpdateStormActors();
	void UpdateTornadoGenesis();
	void UpdateHurricanes();
	AAerosTornado* SpawnTornado(const FAerosStormData& Storm, float RandA, float RandB);
	bool IsNearAnyPlayer(const FVector& Location, float RadiusKm) const;

	UPROPERTY(Transient)
	TObjectPtr<UAerosWeatherSubsystem> Owner;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AAerosStormSystem>> StormActors;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AAerosTornado>> Tornadoes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AAerosHurricane>> Hurricanes;

	TMap<int32, int64> LastTornadoCheck;
	float UpdateTimer = 0.f;
};
