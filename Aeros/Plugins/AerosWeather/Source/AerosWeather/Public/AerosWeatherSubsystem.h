// Copyright Aeros. All Rights Reserved.
//
// UAerosWeatherSubsystem — roadmap "UWeatherSubsystem": the brain.
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
class UTexture2D;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAerosOnPhenomenonChanged, EAerosPhenomenon, OldPhenomenon, EAerosPhenomenon, NewPhenomenon);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnStormEvent, const FAerosStormData&, Storm);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnStormDissipated, int32, StormId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnLightning, const FAerosLightningStrike&, Strike);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnTornado, AAerosTornado*, Tornado);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnCyclone, const FAerosCycloneData&, Cyclone);

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

	// ---------------------------------------------------------------- C++ access
	/** Latest immutable snapshot (thread safe, never blocks). */
	FAerosSnapshotPtr GetSnapshot() const { return Current; }
	/** Snapshot before Current, for temporal interpolation. */
	FAerosSnapshotPtr GetPreviousSnapshot() const { return Previous; }
	/** 0..1 blend between Previous and Current for the displayed time. */
	float GetSnapshotAlpha() const { return SnapshotAlpha; }

	UAerosStormManager* GetStormManager() const { return StormManager; }
	UAerosCloudManager* GetCloudManager() const { return CloudManager; }

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

	FAerosSimulationSettings ActiveSettings;
	TUniquePtr<FAerosSimulationThread> SimThread;

	UPROPERTY(Transient)
	TObjectPtr<UAerosStormManager> StormManager;

	UPROPERTY(Transient)
	TObjectPtr<UAerosCloudManager> CloudManager;

	UPROPERTY(Transient)
	TObjectPtr<AAerosWeatherNetState> NetState;

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
	TSet<int32> KnownStorms;
	TSet<int32> KnownCyclones;
	float RecenterCooldown = 0.f;
	int32 AppliedEpoch = -1;
};
