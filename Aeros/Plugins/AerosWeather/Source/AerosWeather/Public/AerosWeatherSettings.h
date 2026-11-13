// Copyright Aeros. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AerosTypes.h"
#include "AerosWeatherSettings.generated.h"

class UNiagaraSystem;
class USoundBase;
class UMaterialParameterCollection;
class UMaterialInterface;
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

	/** Airborne dust / sand (User.Intensity, User.Wind). */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> DustSystem;

	/** Falling volcanic ash (User.Intensity, User.Wind). */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> AshSystem;

	/** Eruption column at the vent (User.Intensity, User.PlumeHeight in m). */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> VolcanoPlumeSystem;

	/** Spawned along lava flows (User.Intensity). */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UNiagaraSystem> LavaSystem;

	/** Water surface for floods / storm surge / tsunami. Default: /AerosWeather/Materials/M_AerosWater (aeros_setup.py). */
	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TSoftObjectPtr<UMaterialInterface> FloodWaterMaterial;

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

	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	TSoftObjectPtr<USoundBase> EarthquakeSound;

	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	TSoftObjectPtr<USoundBase> VolcanoSound;

	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	TSoftObjectPtr<USoundBase> ExplosionSound;

	// ---- Disasters ---------------------------------------------------------------

	/** Spontaneous earthquakes from SeismicZones (server). */
	UPROPERTY(Config, EditAnywhere, Category = "Disasters")
	bool bSpontaneousEarthquakes = true;

	/** Camera shake strength multiplier for earthquakes / extreme wind. */
	UPROPERTY(Config, EditAnywhere, Category = "Disasters", meta = (ClampMin = "0"))
	float CameraShakeScale = 1.f;

	/** Radius (km) around each player where earthquakes / floods push physics bodies. */
	UPROPERTY(Config, EditAnywhere, Category = "Disasters", meta = (ClampMin = "0.1"))
	float PhysicsEffectRadiusKm = 2.f;

	// ---- Real-world weather ------------------------------------------------------

	/** Minutes between two refreshes of the real-world weather. */
	UPROPERTY(Config, EditAnywhere, Category = "Live Weather", meta = (ClampMin = "1"))
	float LiveRefreshMinutes = 15.f;

	/** Base URL of the Open-Meteo forecast API (self-hosted instances work too). */
	UPROPERTY(Config, EditAnywhere, Category = "Live Weather")
	FString OpenMeteoForecastURL = TEXT("https://api.open-meteo.com/v1/forecast");

	UPROPERTY(Config, EditAnywhere, Category = "Live Weather")
	FString OpenMeteoMarineURL = TEXT("https://marine-api.open-meteo.com/v1/marine");
};
