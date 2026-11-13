// Copyright Aeros. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AerosTypes.h"
#include "AerosWeatherSettings.generated.h"

class UNiagaraSystem;
class USoundBase;
class UMaterialParameterCollection;
class AAerosStormSystem;
class AAerosTornado;
class AAerosHurricane;

/** Project Settings > Plugins > Aeros Weather. */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Aeros Weather"))
class AEROSWEATHER_API UAerosWeatherSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UAerosWeatherSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	static const UAerosWeatherSettings* Get() { return GetDefault<UAerosWeatherSettings>(); }

	/** Start the simulation automatically in every game / PIE world. An AAerosWeatherController in the level overrides the settings. */
	UPROPERTY(Config, EditAnywhere, Category = "Simulation")
	bool bAutoStart = false;

	UPROPERTY(Config, EditAnywhere, Category = "Simulation")
	FAerosSimulationSettings DefaultSimulation;

	/** Use a background thread for the simulation (recommended). */
	UPROPERTY(Config, EditAnywhere, Category = "Simulation")
	bool bUseWorkerThread = true;

	// ---- Gameplay actors ---------------------------------------------------------

	/** Storm actors are spawned only for storms within this distance of a player (LOD). */
	UPROPERTY(Config, EditAnywhere, Category = "Gameplay", meta = (ClampMin = "5"))
	float StormActorRadiusKm = 120.f;

	/** Storm intensity (0-1) needed to get an actor. */
	UPROPERTY(Config, EditAnywhere, Category = "Gameplay", meta = (ClampMin = "0", ClampMax = "1"))
	float StormActorMinIntensity = 0.3f;

	/** Probability scale of tornado-genesis checks (1 = roadmap default: ~15% per check in critical conditions). */
	UPROPERTY(Config, EditAnywhere, Category = "Gameplay", meta = (ClampMin = "0"))
	float TornadoFrequency = 1.f;

	/** Simulated seconds between tornado-genesis checks per supercell. */
	UPROPERTY(Config, EditAnywhere, Category = "Gameplay", meta = (ClampMin = "60"))
	float TornadoCheckInterval = 600.f;

	/** Hail larger than this damages actors (Chaos + ApplyRadialDamage), mm. Roadmap: 30 mm. */
	UPROPERTY(Config, EditAnywhere, Category = "Gameplay")
	float HailDamageThreshold = 30.f;

	UPROPERTY(Config, EditAnywhere, Category = "Gameplay")
	bool bEnableChaosDestruction = true;

	UPROPERTY(Config, EditAnywhere, Category = "Gameplay")
	TSoftClassPtr<AAerosStormSystem> StormActorClass;

	UPROPERTY(Config, EditAnywhere, Category = "Gameplay")
	TSoftClassPtr<AAerosTornado> TornadoActorClass;

	UPROPERTY(Config, EditAnywhere, Category = "Gameplay")
	TSoftClassPtr<AAerosHurricane> HurricaneActorClass;

	// ---- Visuals -----------------------------------------------------------------

	/** Optional Material Parameter Collection fed every frame (see README for parameter names). */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UMaterialParameterCollection> WeatherParameterCollection;

	/** Niagara systems. User parameters: User.Intensity (float), User.Wind (vector m/s), User.HailSize (float mm). */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> RainSystem;

	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> SnowSystem;

	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> HailSystem;

	/** On storm actors. User.SpawnRate (float) = intensity * 1000 (roadmap OnRep_StormIntensity). */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> StormSystem;

	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> TornadoSystem;

	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> LightningSystem;

	/** Built-in instanced-mesh precipitation / debris when no Niagara asset is assigned. */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	bool bUseFallbackVisuals = true;

	/** Lightning flashes within this range spawn a light + thunder. */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals", meta = (ClampMin = "1"))
	float LightningVisualRangeKm = 40.f;

	// ---- Audio -------------------------------------------------------------------

	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	TSoftObjectPtr<USoundBase> RainLoopSound;

	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	TSoftObjectPtr<USoundBase> HailImpactSound;

	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	TSoftObjectPtr<USoundBase> ThunderSound;

	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	TSoftObjectPtr<USoundBase> TornadoLoopSound;

	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	TSoftObjectPtr<USoundBase> WindLoopSound;
};
