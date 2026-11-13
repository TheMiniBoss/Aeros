// Copyright Aeros. All Rights Reserved.
//
// UAerosWeatherSubsystem - roadmap "UWeatherSubsystem": the brain.
// Owns the simulation thread, the storm manager and the cloud manager.
// Global access point for gameplay code and Blueprints.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AerosTypes.h"
#include "Core/AerosSnapshot.h"
#include "AerosWeatherSubsystem.generated.h"

class FAerosSimulationThread;
class FAerosSimulation;
class UAerosStormManager;
class UAerosCloudManager;
class AAerosWeatherNetState;
class AAerosTornado;
class AAerosEarthquake;
class AAerosVolcano;
class UAerosHazardManager;
class UAerosLiveWeather;
class UTexture2D;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAerosOnPhenomenonChanged, EAerosPhenomenon, OldPhenomenon, EAerosPhenomenon, NewPhenomenon);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnStormEvent, const FAerosStormData&, Storm);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnStormDissipated, int32, StormId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnLightning, const FAerosLightningStrike&, Strike);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnTornado, AAerosTornado*, Tornado);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnCyclone, const FAerosCycloneData&, Cyclone);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnEarthquake, const FAerosEarthquakeData&, Earthquake);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAerosOnTsunami, FVector, Source, float, Magnitude);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAerosOnEruption, AAerosVolcano*, Volcano, int32, VEI);

UCLASS()
class AEROSWEATHER_API UAerosWeatherSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UAerosWeatherSubsystem();
	virtual ~UAerosWeatherSubsystem() override;

	static UAerosWeatherSubsystem* Get(const UObject* WorldContext);

	// USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableWhenPaused() const override { return false; }

	// ---------------------------------------------------------------- Lifecycle
	UFUNCTION(BlueprintCallable, Category = "Aeros|Simulation")
	void StartSimulation(const FAerosSimulationSettings& Settings);

	UFUNCTION(BlueprintCallable, Category = "Aeros|Simulation")
	void StopSimulation();

	UFUNCTION(BlueprintPure, Category = "Aeros|Simulation")
	bool IsSimulationRunning() const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Simulation")
	FAerosSimulationSettings GetSimulationSettings() const { return ActiveSettings; }

	// ---------------------------------------------------------------- Time
	UFUNCTION(BlueprintCallable, Category = "Aeros|Time")
	void SetTimeScale(float NewTimeScale);

	UFUNCTION(BlueprintPure, Category = "Aeros|Time")
	float GetTimeScale() const { return ActiveSettings.TimeScale; }

	UFUNCTION(BlueprintCallable, Category = "Aeros|Time")
	void SetPaused(bool bInPaused) { bPaused = bInPaused; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Time")
	bool IsPaused() const { return bPaused; }

	/** Simulated seconds since start (interpolated display time). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Time")
	float GetSimulationTime() const { return float(DisplayTime); }

	UFUNCTION(BlueprintPure, Category = "Aeros|Time")
	FAerosDateTime GetDateTime() const;

	/** Jump the clock (season + time of day). Weather keeps evolving from the current state. */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Time")
	void SetDateTime(int32 DayOfYear, float Hour);

	// ---------------------------------------------------------------- Weather control
	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	void SetScenario(EAerosScenario Scenario);

	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	void SetCustomSynoptic(const FAerosSynopticState& State);

	UFUNCTION(BlueprintPure, Category = "Aeros|Control")
	EAerosScenario GetScenario() const { return ActiveSettings.Scenario; }

	/** Warm, moist bubble (thermal). Starts a storm if the atmosphere is unstable enough. */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	void InjectThermalBubble(FVector WorldLocation, float RadiusKm = 6.f, float DeltaTemperature = 3.f, float DeltaMoisture_gkg = 2.f);

	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	void SpawnHurricane(FVector WorldLocation, float CentralPressure = 975.f, float RadiusMaxWindKm = 35.f);

	/** Server: force a tornado under the given storm (or the strongest storm if -1). */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	AAerosTornado* ForceTornado(int32 StormId = -1);

	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	void SetWaterRegions(const TArray<FAerosWaterRegion>& Regions);

	/** Move the simulated domain so it is centred near this location. */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	void RecenterDomain(FVector WorldLocation);

	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	void SetDesertRegions(const TArray<FAerosDesertRegion>& Regions);

	/** Move the simulated world to another latitude (sun path, Coriolis, climate). */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Control")
	void SetLatitude(float LatitudeDeg);

	// ---------------------------------------------------------------- Disasters
	/** Server: earthquake (Mw). Offshore, shallow and M >= 7 also raises a tsunami. */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Disasters")
	AAerosEarthquake* TriggerEarthquake(FVector Epicenter, float Magnitude = 6.5f, float DepthKm = 10.f);

	/** Sea-floor uplift without the shaking (tests, scripted events). */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Disasters")
	void TriggerTsunami(FVector Source, float Magnitude = 9.f);

	/** Torrential rain over an area (mm): flash floods, rivers out of their banks. */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Disasters")
	void InjectRain(FVector WorldLocation, float RadiusKm = 15.f, float Millimetres = 200.f);

	/** Cold downburst / gust front. Over dry ground it raises a haboob (wall of sand). */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Disasters")
	void InjectOutflow(FVector WorldLocation, float RadiusKm = 20.f, float DeltaTemperature = -10.f);

	/** Volcanic ash column (used by AAerosVolcano). Mass eruption rate in kg/s, duration in simulated seconds. */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Disasters")
	void StartVolcanicEmission(FVector Vent, float MassRateKgPerSecond, float DurationSeconds);

	/** Ground acceleration (g) felt at a location from all active earthquakes. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Disasters")
	float GetGroundShakingAt(FVector WorldLocation) const;

	/** Closest simulated ocean: a coast point and a point OffshoreKm out to sea. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Disasters")
	bool FindNearestOcean(FVector From, FVector& OutCoast, FVector& OutOffshore, float OffshoreKm = 60.f) const;

	/** Re-trace the terrain under the simulated domain (after loading / editing the landscape). */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Disasters")
	void ResampleTerrain();

	UFUNCTION(BlueprintPure, Category = "Aeros|Disasters")
	TArray<AAerosVolcano*> GetVolcanoes() const;

	// ---------------------------------------------------------------- Real-world weather
	/**
	 * Fetch the current weather of a real place (Open-Meteo, no API key) and project it into the
	 * simulation: synoptic state, latitude, local date and hour. Server only (replicated).
	 */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Live")
	void RequestRealWeather(float Latitude, float Longitude, const FString& PlaceName, bool bApplyLocalTime = true, bool bKeepUpdating = true);

	UFUNCTION(BlueprintCallable, Category = "Aeros|Live")
	void StopRealWeather();

	UFUNCTION(BlueprintPure, Category = "Aeros|Live")
	UAerosLiveWeather* GetLiveWeather() const { return LiveWeather; }

	// ---------------------------------------------------------------- Queries
	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	FAerosWeatherSample GetWeatherAtLocation(FVector WorldLocation) const;

	/** Surface wind (world, m/s) including active tornado / hurricane vortices. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	FVector GetWindAtLocation(FVector WorldLocation) const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	EAerosPhenomenon GetPhenomenonAtLocation(FVector WorldLocation) const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	TArray<FAerosStormData> GetStorms() const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	TArray<FAerosCycloneData> GetCyclones() const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	bool GetNearestStorm(FVector WorldLocation, FAerosStormData& OutStorm, float MaxDistanceKm = 200.f) const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	TArray<AAerosTornado*> GetActiveTornadoes() const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	FAerosSynopticState GetSynopticState() const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	FAerosSimStats GetStats() const;

	/** Location used for LOD, local effects and domain following (local player camera). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Query")
	FVector GetViewerLocation() const { return ViewerLocation; }

	// ---------------------------------------------------------------- Rendering
	/** RGBA8: R cloud cover, G cloud base (0-5 km), B cloud top (0-16 km), A precipitation/darkness. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Render")
	UTexture2D* GetWeatherMapTexture() const;

	/** RGBA8: R wetness, G snow depth (0-50 cm), B hail cover, A temperature (-40..50 C). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Render")
	UTexture2D* GetSurfaceMapTexture() const;

	/** RGBA8: R flood depth, G dust, B ash on ground, A sea level anomaly (see UAerosCloudManager). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Render")
	UTexture2D* GetHazardMapTexture() const;

	/** (CenterX, CenterY, SizeX, SizeY) in cm, to map world XY to weather-map UV. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Render")
	FVector4 GetDomainRect() const;

	// ---------------------------------------------------------------- Events
	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnPhenomenonChanged OnPhenomenonChanged;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnStormEvent OnStormFormed;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnStormDissipated OnStormDissipated;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnLightning OnLightningStrike;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnTornado OnTornadoTouchdown;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnCyclone OnCycloneFormed;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnEarthquake OnEarthquake;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnTsunami OnTsunami;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Events")
	FAerosOnEruption OnVolcanicEruption;

	// ---------------------------------------------------------------- C++ access
	/** Latest immutable snapshot (thread safe, never blocks). */
	FAerosSnapshotPtr GetSnapshot() const { return Current; }
	/** Snapshot before Current, for temporal interpolation. */
	FAerosSnapshotPtr GetPreviousSnapshot() const { return Previous; }
	/** 0..1 blend between Previous and Current for the displayed time. */
	float GetSnapshotAlpha() const { return SnapshotAlpha; }

	UAerosStormManager* GetStormManager() const { return StormManager; }
	UAerosCloudManager* GetCloudManager() const { return CloudManager; }
	UAerosHazardManager* GetHazardManager() const { return HazardManager; }

	/** Push a command to the sim thread. Network-aware (server replicates it). */
	void EnqueueCommand(TUniqueFunction<void(FAerosSimulation&)>&& Command);

	// Net sync (called by AAerosWeatherNetState)
	void RegisterNetState(AAerosWeatherNetState* InNetState);
	void ApplyNetState(const AAerosWeatherNetState& State);
	void ApplyNetCommand(uint8 Type, const FVector& Location, const FVector4& Params, double ApplyAt);
	bool HasAuthority() const;

private:
	void UpdateViewer();
	void UpdateClock(float DeltaTime);
	void PullSnapshot();
	void BroadcastEvents();
	void MaybeFollowViewer();
	void IssueCommand(uint8 Type, const FVector& Location, const FVector4& Params);
	void UpdateTerrainSampling();

	FAerosSimulationSettings ActiveSettings;
	TUniquePtr<FAerosSimulationThread> SimThread;

	UPROPERTY(Transient)
	TObjectPtr<UAerosStormManager> StormManager;

	UPROPERTY(Transient)
	TObjectPtr<UAerosCloudManager> CloudManager;

	UPROPERTY(Transient)
	TObjectPtr<AAerosWeatherNetState> NetState;

	UPROPERTY(Transient)
	TObjectPtr<UAerosHazardManager> HazardManager;

	UPROPERTY(Transient)
	TObjectPtr<UAerosLiveWeather> LiveWeather;

	// Terrain sampling (time-sliced line traces)
	TArray<float> TerrainHeights;
	FVector TerrainCenter = FVector(1e30, 0, 0);
	int32 TerrainCursor = -1;
	int32 TerrainNX = 0, TerrainNY = 0;
	float TerrainCellCm = 0.f;

	FAerosSnapshotPtr Current;
	FAerosSnapshotPtr Previous;
	float SnapshotAlpha = 1.f;

	double TargetTime = 0.0;
	double DisplayTime = 0.0;
	bool bPaused = false;

	FVector ViewerLocation = FVector::ZeroVector;
	EAerosPhenomenon ViewerPhenomenon = EAerosPhenomenon::Clear;
	float PhenomenonCheckTimer = 0.f;
	int32 LastLightningSeq = 0;
	TArray<FAerosLightningStrike> PendingStrikes; // waiting for the displayed clock to reach their SimTime
	TSet<int32> KnownStorms;
	TSet<int32> KnownCyclones;
	float RecenterCooldown = 0.f;
	int32 AppliedEpoch = -1;
};
