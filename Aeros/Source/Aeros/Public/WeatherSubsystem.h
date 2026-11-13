// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WeatherTypes.h"
#include "WeatherSimulationThread.h"
#include "WeatherBuffer.h"
#include "WeatherRadar.h"
#include "Engine/Texture2D.h"
#include "WeatherSubsystem.generated.h"

// ============================================================
//  DELEGATES
// ============================================================

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnWeatherPhenomenonChanged,
                                            EWeatherPhenomenon, NewPhenomenon);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStormCellDetected,
                                             FStormCell, StormCell, bool, bIsNew);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRadarScanComplete,
                                            const FRadarVolumeScan&, VolumeScan);

// ============================================================
//  CHUNK KEY — infinite world chunking
// ============================================================

USTRUCT()
struct FWeatherChunkKey
{
    GENERATED_BODY()

    int32 ChunkX = 0, ChunkY = 0;

    bool operator==(const FWeatherChunkKey& O) const
    {
        return ChunkX == O.ChunkX && ChunkY == O.ChunkY;
    }
};

FORCEINLINE uint32 GetTypeHash(const FWeatherChunkKey& K)
{
    return HashCombine(GetTypeHash(K.ChunkX), GetTypeHash(K.ChunkY));
}

// ============================================================
//  WEATHER SUBSYSTEM
// ============================================================

UCLASS()
class AEROS_API UWeatherSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    // ── USubsystem interface ──────────────────────────────
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    bool TickWrapper(float DeltaTime);
    virtual void Deinitialize() override;
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;

    // ── Tick via custom tickable ──────────────────────────
    void Tick(float DeltaTime);

    // ── Configuration ────────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Weather|Config")
    void SetConfiguration(const FWeatherSimConfig& NewConfig);

    UFUNCTION(BlueprintCallable, Category = "Weather|Config")
    FWeatherSimConfig GetConfiguration() const { return ActiveConfig; }

    UFUNCTION(BlueprintCallable, Category = "Weather|Config")
    void SetMicrophysicsScheme(EMicrophysicsScheme Scheme);

    UFUNCTION(BlueprintCallable, Category = "Weather|Config")
    void SetConvectionScheme(EConvectionScheme Scheme);

    // ── Queries (Blueprint-callable, thread-safe) ─────────
    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    bool GetAtmosphereAtLocation(const FVector& WorldPos,
                                 FAtmosphericCell& OutCell) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    bool GetColumnDiagnostics(const FVector& WorldPos,
                              FAtmosphericColumn& OutColumn) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    FVector GetWindAtLocation(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    float GetTemperatureAtLocation(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    float GetPressureAtLocation(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    float GetRelativeHumidityAtLocation(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    float GetPrecipitationRateAtLocation(const FVector& WorldPos) const; // mm/hr

    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    bool IsHailPossibleAtLocation(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Query")
    EWeatherPhenomenon GetCurrentPhenomenon(const FVector& WorldPos) const;

    // ── Storm queries ─────────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Weather|Storms")
    TArray<FStormCell> GetAllDetectedStorms() const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Storms")
    bool GetNearestStorm(const FVector& WorldPos, FStormCell& OutStorm,
                         float MaxRadius_cm = 500000.f) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Storms")
    int32 GetMesocycloneCount() const;

    // ── Interventions ────────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Weather|Inject")
    void InjectThermalBubble(const FVector& WorldPos, float Strength = 2.f);

    UFUNCTION(BlueprintCallable, Category = "Weather|Inject")
    void InjectMoistureAnomaly(const FVector& WorldPos, float DeltaQv = 0.005f);

    UFUNCTION(BlueprintCallable, Category = "Weather|Inject")
    void InjectVorticity(const FVector& WorldPos, float Strength = 5.f);

    UFUNCTION(BlueprintCallable, Category = "Weather|Inject")
    void ResetSimulation();

    // ── Radar access ─────────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Weather|Radar")
    UWeatherRadar* GetRadar() const { return Radar; }

    UFUNCTION(BlueprintCallable, Category = "Weather|Radar")
    void SetRadarLocation(const FVector& WorldPos);

    UFUNCTION(BlueprintCallable, Category = "Weather|Radar")
    FRadarVolumeScan GetLatestVolumeScan() const;

    // ── Infinite world integration ────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Weather|World")
    void UpdatePlayerLocation(const FVector& PlayerWorldPos);

    UFUNCTION(BlueprintCallable, Category = "Weather|World")
    FVector GetDomainOrigin() const { return DomainOrigin; }

    // ── Rendering helpers (polled by Weather Actors) ──────
    UFUNCTION(BlueprintCallable, Category = "Weather|Render")
    float GetCloudCoverFraction(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Render")
    float GetReflectivity_dBZ(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Render")
    float GetTKE(const FVector& WorldPos) const;

    // ── Events ───────────────────────────────────────────
    UPROPERTY(BlueprintAssignable, Category = "Weather|Events")
    FOnWeatherPhenomenonChanged OnPhenomenonChanged;

    UPROPERTY(BlueprintAssignable, Category = "Weather|Events")
    FOnStormCellDetected OnStormCellDetected;

    UPROPERTY(BlueprintAssignable, Category = "Weather|Events")
    FOnRadarScanComplete OnRadarScanComplete;

    // ── Simulation time ───────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Weather|Time")
    float GetSimulationTime() const;

    UFUNCTION(BlueprintCallable, Category = "Weather|Time")
    float GetSimulationSpeed() const { return SimulationSpeed; }

    UFUNCTION(BlueprintCallable, Category = "Weather|Time")
    void  SetSimulationSpeed(float Speed) { SimulationSpeed = FMath::Clamp(Speed, 0.f, 100.f); }

private:
    void CreateSimThread(const FWeatherSimConfig& Cfg);
    void DestroySimThread();
    void UpdateDomainOrigin(const FVector& PlayerPos);

    FWeatherSimConfig           ActiveConfig;
    TUniquePtr<FWeatherSimulationThread> SimThread;

    UPROPERTY()
    UWeatherRadar* Radar = nullptr;

    // Render / Snapshot optimizations
    FWeatherDoubleBuffer::FReadView  CachedRenderView;
    TArray<FAtmosphericColumn>       ColumnCache;
    mutable FCriticalSection         ColumnCacheLock;
    int32                            ColumnCacheNX = 0;
    int32                            ColumnCacheNY = 0;

    // Storm tracking
    mutable FCriticalSection    StormLock;
    TArray<FStormCell>          TrackedStorms;

    // Infinite world domain management
    FVector                     DomainOrigin = FVector::ZeroVector;
    FVector                     LastPlayerPos = FVector::ZeroVector;
    float                       DomainShiftThreshold_cm = 0.f;

    // Timing
    float                       SimulationSpeed = 1.f;
    float                       AccumTime = 0.f;
    float                       LastPhenomenonCheck = 0.f;

    EWeatherPhenomenon          LastBroadcastPhenomenon = EWeatherPhenomenon::Clear;

    FTSTicker::FDelegateHandle  TickHandle;
};