// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "WeatherTypes.h"
#include "WeatherSimulationThread.h"
#include "WeatherRadar.generated.h"

class UWeatherSubsystem;

// ============================================================
//  RADAR PRODUCT TYPES
// ============================================================

UENUM(BlueprintType)
enum class ERadarProductType : uint8
{
    BaseReflectivity_PPI,    // Z at one elevation
    CompositeReflectivity,   // Max Z all elevations
    BaseVelocity_PPI,        // Radial velocity
    SpectrumWidth_PPI,       // Turbulence indicator
    StormRelativeVelocity,   // V after storm-motion removal
    CAPPI_1km,               // Constant Altitude PPI 1km
    CAPPI_3km,
    EchoTop,                 // 18 dBZ echo top
    VIL,                     // Vertically Integrated Liquid
    DualPol_ZDR,             // Differential reflectivity
    DualPol_KDP,             // Specific differential phase
    DualPol_CC,              // Correlation coefficient ρhv
    Velocity_Azimuth_Display,// VAD wind profile
};

UENUM(BlueprintType)
enum class ERadarScanMode : uint8
{
    VCP21,   // 9 elevations, 6-min scan (precipitation)
    VCP212,  // 14 elevations, 4.5-min scan (severe)
    VCP31,   // 8 elevations, 10-min scan (clear air)
    VCP35,   // 11 elevations, 6-min scan (clear air enhanced)
    Continuous, // Always-on streaming
};

// ============================================================
//  MESOCYCLONE DETECTION
// ============================================================

USTRUCT(BlueprintType)
struct FMesocycloneDetection
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite) FVector  Position = FVector::ZeroVector;
    UPROPERTY(BlueprintReadWrite) float    Range_km = 0.f;
    UPROPERTY(BlueprintReadWrite) float    Azimuth_deg = 0.f;
    UPROPERTY(BlueprintReadWrite) float    Depth_km = 0.f;
    UPROPERTY(BlueprintReadWrite) float    Diameter_km = 0.f;
    UPROPERTY(BlueprintReadWrite) float    RotVelocity_ms = 0.f;
    UPROPERTY(BlueprintReadWrite) bool     bTVS = false;  // Tornadic Vortex Sig
    UPROPERTY(BlueprintReadWrite) bool     bMDAAlert = false;  // Meso Detection Algo
    UPROPERTY(BlueprintReadWrite) int32    Rank = 0;      // 1 (weak) to 5 (strong)
};

// ============================================================
//  VELOCITY AZIMUTH DISPLAY (VAD) wind profile
// ============================================================

USTRUCT(BlueprintType)
struct FVADWindProfile
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite) TArray<float>   Altitude_m;
    UPROPERTY(BlueprintReadWrite) TArray<float>   WindSpeed_ms;
    UPROPERTY(BlueprintReadWrite) TArray<float>   WindDir_deg;
    UPROPERTY(BlueprintReadWrite) TArray<float>   DivergenceLevel;  // 10⁻³/s
};

// ============================================================
//  WEATHER RADAR — Main class
// ============================================================

UCLASS(BlueprintType)
class AEROS_API UWeatherRadar : public UObject
{
    GENERATED_BODY()

public:
    void Initialise(UWeatherSubsystem* InSubsystem);
    void Tick(float DeltaTime);
    void SetLocation(const FVector& WorldPos);
    void OnNewSimulationData(const FWeatherDoubleBuffer::FReadView& InView, const FWeatherSimConfig& Config);

    // ── Scan control ────────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Radar|Control")
    void StartScan(ERadarScanMode Mode = ERadarScanMode::VCP21);

    UFUNCTION(BlueprintCallable, Category = "Radar|Control")
    void StopScan();

    UFUNCTION(BlueprintCallable, Category = "Radar|Control")
    void SetScanMode(ERadarScanMode Mode);

    UFUNCTION(BlueprintCallable, Category = "Radar|Control")
    void SetMaxRange(float MaxRange_km);

    UFUNCTION(BlueprintCallable, Category = "Radar|Control")
    void SetPRF(float PRF_Hz); // Pulse repetition frequency → affects max vel / range

    // ── Data access ──────────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Radar|Data")
    FRadarVolumeScan GetLatestVolumeScan() const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Data")
    TArray<FRadarGate> GetPPISweep(float Elevation_deg) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Data")
    TArray<FRadarGate> GetRHISweep(float Azimuth_deg) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Data")
    TArray<float> GetCAPPI(float Altitude_km, int32& OutGridX, int32& OutGridY) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Data")
    TArray<float> GetCompositeReflectivity(int32& OutGridX, int32& OutGridY) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Data")
    TArray<float> GetEchoTop_km(int32& OutGridX, int32& OutGridY) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Data")
    TArray<float> GetVIL(int32& OutGridX, int32& OutGridY) const; // kg/m²

    // ── Products derived at a point ───────────────────────
    UFUNCTION(BlueprintCallable, Category = "Radar|Products")
    float GetReflectivityAtPoint(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Products")
    float GetRadialVelocityAtPoint(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Products")
    float GetZDR_AtPoint(const FVector& WorldPos) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Products")
    float GetCC_AtPoint(const FVector& WorldPos) const; // rhohv

    UFUNCTION(BlueprintCallable, Category = "Radar|Products")
    float GetKDP_AtPoint(const FVector& WorldPos) const;

    // ── Storm algorithms ─────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Radar|Algorithms")
    TArray<FMesocycloneDetection> DetectMesocyclones() const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Algorithms")
    TArray<FStormCell> STI_StormTracking() const; // Storm Tracking and Intensity

    UFUNCTION(BlueprintCallable, Category = "Radar|Algorithms")
    FVADWindProfile ComputeVADWindProfile(float Elevation_deg = 0.5f) const;

    UFUNCTION(BlueprintCallable, Category = "Radar|Algorithms")
    float EstimateRainfallRate_QPE(const FVector& WorldPos) const; // mm/hr

    // ── Status ────────────────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Radar|Status")
    bool  IsScanning()         const { return bIsScanning; }

    UFUNCTION(BlueprintCallable, Category = "Radar|Status")
    float GetCurrentElevation() const { return CurrentElevation_deg; }

    UFUNCTION(BlueprintCallable, Category = "Radar|Status")
    float GetCurrentAzimuth()   const { return CurrentAzimuth_deg; }

    UFUNCTION(BlueprintCallable, Category = "Radar|Status")
    FVector GetLocation()       const { return RadarLocation; }

    UFUNCTION(BlueprintCallable, Category = "Radar|Status")
    float GetNyquistVelocity()  const; // m/s

    UFUNCTION(BlueprintCallable, Category = "Radar|Status")
    float GetMaxUnambiguousRange_km() const;

    // ── UI Helpers ────────────────────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Radar|UI")
    UTexture2D* GenerateRadarTexture(const TArray<float>& ReflectivityData, int32 GridX, int32 GridY);

    UFUNCTION(BlueprintCallable, Category = "Radar|Control")
    void SetAntennaSpeed(float DegreesPerSecond) { AntennaSweepRate = DegreesPerSecond; }

private:
    // Beam propagation
    void  ComputeBeam(float Elevation_deg, float Azimuth_deg, FRadarSweep& Sweep);
    float BeamHeight_m(float Range_m, float Elevation_rad) const;
    float BeamAtmosphericAttenuation(float Range_m, float Z_dBZ) const;
    float BeamBlockage(float Elevation_deg, float Azimuth_deg, float Range_m) const;
    FVector GateWorldPosition(float Range_m, float Azimuth_deg, float Elevation_deg) const;

    // Gate computation
    FRadarGate ComputeGate(float Range_m, float Az_deg, float El_deg,
                           const FWeatherDoubleBuffer::FReadView& View,
                           const FWeatherSimConfig& Cfg) const;

    // Dual-pol parameters
    float Compute_ZDR(const FWeatherRenderCell& Cell) const;
    float Compute_KDP(const FWeatherRenderCell& Cell) const;
    float Compute_CC(const FWeatherRenderCell& Cell) const;

    // Products
    void BuildCAPPI(FRadarVolumeScan& Scan) const;
    void BuildEchoTop(FRadarVolumeScan& Scan) const;
    void BuildVIL(FRadarVolumeScan& Scan) const;

    // SCAN VCP schedules (elevation angles)
    TArray<float> GetVCPElevations(ERadarScanMode Mode) const;

    // Data members
    UPROPERTY()
    UWeatherSubsystem* Subsystem = nullptr;

    FVector    RadarLocation = FVector::ZeroVector;
    float      MaxRange_km = 230.f;
    float      PRF_Hz = 1000.f;   // Hz
    float      Wavelength_m = 0.10f;    // 10 cm (S-band)
    float      AntennaSweepRate = 18.f;     // deg/s (RPM)
    float      GateSpacing_m = 250.f;
    int32      NumAzimuths = 360;

    ERadarScanMode ScanMode = ERadarScanMode::VCP21;
    bool       bIsScanning = false;
    float      CurrentElevation_deg = 0.5f;
    float      CurrentAzimuth_deg = 0.f;
    int32      CurrentElevIdx = 0;
    float      ScanTimer = 0.f;

    // Latest full volume scan
    mutable FCriticalSection ScanLock;
    FRadarVolumeScan         CurrentVolumeScan;
    FRadarVolumeScan         PreviousVolumeScan;
    bool                     bVolumeScanReady = false;

    // Last known sim state (for on-demand queries)
    FCriticalSection         Statelock;
    FWeatherDoubleBuffer::FReadView LatestView;
    FWeatherSimConfig        LatestConfig;
    bool                     bHasState = false;
};