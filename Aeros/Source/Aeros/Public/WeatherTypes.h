// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "WeatherTypes.generated.h"

// ============================================================
//  CONSTANTS — Atmospheric physics
// ============================================================
namespace WeatherConst
{
    constexpr float Rd = 287.05f;   // J/(kg·K)  — dry-air gas constant
    constexpr float Rv = 461.5f;    // J/(kg·K)  — water-vapour gas constant
    constexpr float Cp = 1005.0f;   // J/(kg·K)  — specific heat at const. pressure
    constexpr float Cv = 718.0f;    // J/(kg·K)  — specific heat at const. volume
    constexpr float Lv = 2.501e6f;  // J/kg      — latent heat of vaporisation
    constexpr float Lf = 3.337e5f;  // J/kg      — latent heat of fusion
    constexpr float Ls = 2.834e6f;  // J/kg      — latent heat of sublimation
    constexpr float g = 9.80665f;  // m/s²
    constexpr float Omega = 7.2921e-5f;// rad/s     — Earth angular velocity
    constexpr float P0 = 100000.0f; // Pa        — reference pressure
    constexpr float T0 = 288.15f;   // K         — ISA sea-level temperature
    constexpr float RhoAir0 = 1.225f;    // kg/m³     — sea-level density
    constexpr float Kappa = Rd / Cp;   // ≈ 0.2857
    constexpr float EsConst_A = 611.2f;    // Pa        — Tetens const
    constexpr float EsConst_B = 17.67f;
    constexpr float EsConst_C = 243.5f;    // °C
}

// ============================================================
//  ENUMS
// ============================================================

UENUM(BlueprintType)
enum class EWeatherPhenomenon : uint8
{
    Clear           UMETA(DisplayName = "Clear"),
    PartlyCloudy    UMETA(DisplayName = "Partly Cloudy"),
    Overcast        UMETA(DisplayName = "Overcast"),
    Drizzle         UMETA(DisplayName = "Drizzle"),
    Rain            UMETA(DisplayName = "Rain"),
    HeavyRain       UMETA(DisplayName = "Heavy Rain"),
    Thunderstorm    UMETA(DisplayName = "Thunderstorm"),
    Supercell       UMETA(DisplayName = "Supercell"),
    Tornado         UMETA(DisplayName = "Tornado"),
    Blizzard        UMETA(DisplayName = "Blizzard"),
    Hail            UMETA(DisplayName = "Hail"),
    TropicalCyclone UMETA(DisplayName = "Tropical Cyclone"),
    Fog             UMETA(DisplayName = "Fog"),
};

UENUM(BlueprintType)
enum class EHydrometeorType : uint8
{
    CloudDroplet,
    RainDrop,
    CloudIce,
    Snow,
    Graupel,
    Hail,
};

UENUM(BlueprintType)
enum class EConvectionScheme : uint8
{
    Explicit       UMETA(DisplayName = "Explicit (dx<=3km)"),
    GrayZone       UMETA(DisplayName = "Gray Zone (3-10km)"),
    Parameterised  UMETA(DisplayName = "Parameterised (dx>=10km)"),
};

UENUM(BlueprintType)
enum class EMicrophysicsScheme : uint8
{
    Kessler,      // 1-moment, warm-rain only
    WSM6,         // 1-moment, 6-category ice
    Thompson,     // 2-moment, aerosol-aware
    Morrison,     // 2-moment, full ice
};

// ============================================================
//  HYDROMETEOR — single species moment state
// ============================================================
USTRUCT(BlueprintType)
struct AEROS_API FHydrometeorState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite) float MixingRatio = 0.f;   // kg/kg
    UPROPERTY(BlueprintReadWrite) float NumberConc = 0.f;   // #/m³  (2-moment)
    UPROPERTY(BlueprintReadWrite) float MeanDiameter = 0.f;   // m
    UPROPERTY(BlueprintReadWrite) float FallSpeed = 0.f;   // m/s (terminal)

    // Marshall-Palmer / Gamma distribution parameter
    UPROPERTY(BlueprintReadWrite) float Lambda = 0.f;
    UPROPERTY(BlueprintReadWrite) float N0 = 0.f;

    bool IsEmpty() const { return MixingRatio < 1e-10f; }
};

// ============================================================
//  ATMOSPHERIC CELL — 3D grid node, all prognostic variables
// ============================================================
USTRUCT(BlueprintType)
struct AEROS_API FAtmosphericCell
{
    GENERATED_BODY()

    // ── Prognostic dynamic variables ──────────────────────
    UPROPERTY(BlueprintReadWrite) float U = 0.f;   // m/s  (zonal)
    UPROPERTY(BlueprintReadWrite) float V = 0.f;   // m/s  (meridional)
    UPROPERTY(BlueprintReadWrite) float W = 0.f;   // m/s  (vertical)
    UPROPERTY(BlueprintReadWrite) float Rho = 1.225f;// kg/m³
    UPROPERTY(BlueprintReadWrite) float Theta = 288.f; // K    (potential temperature)
    UPROPERTY(BlueprintReadWrite) float Pres = 101325.f; // Pa

    // ── Moisture variables ────────────────────────────────
    UPROPERTY(BlueprintReadWrite) float Qv = 0.f;   // water vapour   kg/kg
    UPROPERTY(BlueprintReadWrite) float Ql = 0.f;   // cloud liquid   kg/kg
    UPROPERTY(BlueprintReadWrite) float Qi = 0.f;   // cloud ice      kg/kg
    UPROPERTY(BlueprintReadWrite) FHydrometeorState Rain;
    UPROPERTY(BlueprintReadWrite) FHydrometeorState Snow;
    UPROPERTY(BlueprintReadWrite) FHydrometeorState Graupel;
    UPROPERTY(BlueprintReadWrite) FHydrometeorState Hail;

    // ── Turbulence ────────────────────────────────────────
    UPROPERTY(BlueprintReadWrite) float TKE = 0.f;   // J/kg (TKE)
    UPROPERTY(BlueprintReadWrite) float Eps = 0.f;   // dissipation

    // ── Derived / diagnostic ──────────────────────────────
    UPROPERTY(BlueprintReadWrite) float Temperature = 0.f; // K (actual)
    UPROPERTY(BlueprintReadWrite) float VirtualTemp = 0.f; // K
    UPROPERTY(BlueprintReadWrite) float RelativeHumidity = 0.f; // 0–1
    UPROPERTY(BlueprintReadWrite) float VortZ = 0.f;  // vertical vorticity
    UPROPERTY(BlueprintReadWrite) float Divergence = 0.f;
    UPROPERTY(BlueprintReadWrite) float Reflectivity_dBZ = 0.f; // for radar

    // Compute temperature from potential temperature + pressure
    void UpdateDerivedVars()
    {
        Temperature = Theta * FMath::Pow(Pres / WeatherConst::P0, WeatherConst::Kappa);
        VirtualTemp = Temperature * (1.f + 0.61f * Qv);
        Rho = Pres / (WeatherConst::Rd * VirtualTemp);

        // Saturation vapour pressure (Tetens)
        float Tc = Temperature - 273.15f;
        float Es = WeatherConst::EsConst_A *
            FMath::Exp(WeatherConst::EsConst_B * Tc / (Tc + WeatherConst::EsConst_C));
        float Qs = 0.622f * Es / FMath::Max(Pres - Es, 1.f);
        RelativeHumidity = FMath::Clamp(Qv / FMath::Max(Qs, 1e-6f), 0.f, 1.f);
    }
};

// ============================================================
//  COLUMN DIAGNOSTIC — derived stability indices
// ============================================================
USTRUCT(BlueprintType)
struct AEROS_API FAtmosphericColumn
{
    GENERATED_BODY()

    // Thermodynamic stability indices
    UPROPERTY(BlueprintReadWrite) float CAPE = 0.f;   // J/kg
    UPROPERTY(BlueprintReadWrite) float CIN = 0.f;   // J/kg (negative)
    UPROPERTY(BlueprintReadWrite) float LCL_m = 0.f;   // m AGL — Lifting Condensation Level
    UPROPERTY(BlueprintReadWrite) float LFC_m = 0.f;   // m AGL — Level of Free Convection
    UPROPERTY(BlueprintReadWrite) float EL_m = 0.f;   // m AGL — Equilibrium Level

    // Wind kinematics
    UPROPERTY(BlueprintReadWrite) float SRH_0_3km = 0.f;   // m²/s² — Storm-Relative Helicity
    UPROPERTY(BlueprintReadWrite) float SRH_0_1km = 0.f;
    UPROPERTY(BlueprintReadWrite) float BulkShear_0_6km = 0.f; // m/s
    UPROPERTY(BlueprintReadWrite) float BulkShear_0_1km = 0.f;
    UPROPERTY(BlueprintReadWrite) FVector StormMotion = FVector::ZeroVector; // m/s

    // Composite parameters
    UPROPERTY(BlueprintReadWrite) float STP = 0.f;   // Significant Tornado Parameter
    UPROPERTY(BlueprintReadWrite) float SCP = 0.f;   // Supercell Composite Parameter
    UPROPERTY(BlueprintReadWrite) float SHIP = 0.f;   // Significant Hail Parameter

    // Surface
    UPROPERTY(BlueprintReadWrite) float SST = 0.f;   // K  — sea-surface temperature
    UPROPERTY(BlueprintReadWrite) float PrecipRate = 0.f;  // mm/hr

    EWeatherPhenomenon ClassifyPhenomenon() const
    {
        if (CAPE > 2500.f && SRH_0_3km > 300.f && BulkShear_0_6km > 20.f)
            return EWeatherPhenomenon::Supercell;
        if (CAPE > 1500.f && SRH_0_1km > 150.f)
            return EWeatherPhenomenon::Tornado;
        if (CAPE > 800.f && SHIP > 1.f)
            return EWeatherPhenomenon::Hail;
        if (CAPE > 500.f)
            return EWeatherPhenomenon::Thunderstorm;
        if (PrecipRate > 10.f)
            return EWeatherPhenomenon::HeavyRain;
        if (PrecipRate > 0.1f)
            return EWeatherPhenomenon::Rain;
        return EWeatherPhenomenon::Clear;
    }
};

// ============================================================
//  RADAR — return data per gate
// ============================================================
USTRUCT(BlueprintType)
struct AEROS_API FRadarGate
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite) float Reflectivity_dBZ = -32.f;  // dBZ
    UPROPERTY(BlueprintReadWrite) float RadialVelocity_ms = 0.f;    // m/s (+ away)
    UPROPERTY(BlueprintReadWrite) float SpectrumWidth = 0.f;    // m/s
    UPROPERTY(BlueprintReadWrite) float DifferentialRef = 0.f;    // dB (Zdr)
    UPROPERTY(BlueprintReadWrite) float CorrelationCoeff = 1.f;    // rhohv 0–1
    UPROPERTY(BlueprintReadWrite) float KDP = 0.f;    // °/km
    UPROPERTY(BlueprintReadWrite) float Range_m = 0.f;
    UPROPERTY(BlueprintReadWrite) float Azimuth_deg = 0.f;
    UPROPERTY(BlueprintReadWrite) float Elevation_deg = 0.f;
    UPROPERTY(BlueprintReadWrite) FVector WorldPosition = FVector::ZeroVector;
    UPROPERTY(BlueprintReadWrite) bool   bIsValid = false;
};

USTRUCT(BlueprintType)
struct AEROS_API FRadarSweep
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite) float Elevation_deg = 0.5f;
    UPROPERTY(BlueprintReadWrite) float Timestamp = 0.f;
    UPROPERTY(BlueprintReadWrite) TArray<FRadarGate> Gates;

    int32 NumAzimuths = 360;
    int32 NumRanges = 460; // gates (250m each → 115km)
};

USTRUCT(BlueprintType)
struct AEROS_API FRadarVolumeScan
{
    GENERATED_BODY()

    // VCP-21 like: 9 elevation angles
    UPROPERTY(BlueprintReadWrite) TArray<FRadarSweep> Sweeps;
    UPROPERTY(BlueprintReadWrite) float ScanStartTime = 0.f;
    UPROPERTY(BlueprintReadWrite) FVector RadarLocation = FVector::ZeroVector;
    UPROPERTY(BlueprintReadWrite) float MaxRange_km = 230.f;

    // CAPPI products derived from volume
    UPROPERTY(BlueprintReadWrite) TArray<float> CAPPI_1km;  // 2D max-refl at 1km
    UPROPERTY(BlueprintReadWrite) TArray<float> CAPPI_3km;
    UPROPERTY(BlueprintReadWrite) TArray<float> EchoTop_km; // 2D echo-top
};

// ============================================================
//  STORM CELL — tracked mesocyclone / cell
// ============================================================
USTRUCT(BlueprintType)
struct AEROS_API FStormCell
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite) FVector  Position = FVector::ZeroVector;
    UPROPERTY(BlueprintReadWrite) FVector  Motion_ms = FVector::ZeroVector; // m/s
    UPROPERTY(BlueprintReadWrite) float    MaxRefl_dBZ = 0.f;
    UPROPERTY(BlueprintReadWrite) float    EchoTop_km = 0.f;
    UPROPERTY(BlueprintReadWrite) float    MaxW_ms = 0.f; // updraft
    UPROPERTY(BlueprintReadWrite) float    MaxVortZ = 0.f; // vertical vorticity
    UPROPERTY(BlueprintReadWrite) float    MesocycloneDiameter_km = 0.f;
    UPROPERTY(BlueprintReadWrite) float    RotationalVelocity_ms = 0.f;
    UPROPERTY(BlueprintReadWrite) bool     bIsMesocyclone = false;
    UPROPERTY(BlueprintReadWrite) bool     bTVS = false; // Tornadic Vortex Signature
    UPROPERTY(BlueprintReadWrite) EWeatherPhenomenon Type = EWeatherPhenomenon::Thunderstorm;
    UPROPERTY(BlueprintReadWrite) int32    TrackID = -1;
    UPROPERTY(BlueprintReadWrite) float    Age_s = 0.f;
};

// ============================================================
//  SIMULATION CONFIG
// ============================================================
USTRUCT(BlueprintType)
struct AEROS_API FWeatherSimConfig
{
    GENERATED_BODY()

    // Grid
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32  GridNX = 128;  // cells horiz.
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32  GridNY = 128;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32  GridNZ = 50;   // vertical levels
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float  DX_km = 1.0f; // horiz. spacing km
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float  DZ_m = 200.f;// first-level thick

    // Time integration
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float  DT_s = 2.0f; // acoustic split dt
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float  BigDT_s = 12.f; // physics dt
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool   bUseRK4 = true;

    // Physics switches
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EMicrophysicsScheme MicroScheme = EMicrophysicsScheme::Morrison;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EConvectionScheme   ConvScheme = EConvectionScheme::Explicit;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool   bEnableTurbulence = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool   bEnableRadiation = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool   bEnableCoriolis = true;

    // Environment (synoptic forcing)
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float  Latitude_deg = 40.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float  SSTempK = 300.f; // sea-surface T
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector MeanWind = FVector(10.f, 0.f, 0.f); // m/s

    // World offset (for infinite world — chunk coords)
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector WorldOrigin = FVector::ZeroVector;

    float CoriolisF() const
    {
        return 2.f * WeatherConst::Omega * FMath::Sin(FMath::DegreesToRadians(Latitude_deg));
    }
};