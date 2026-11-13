// Copyright Aeros. All Rights Reserved.
// Public data types shared by the simulation core, the game layer and Blueprints.
//
// Coordinate convention:
//   Simulation frame: x = east, y = north, z = up (metres).
//   Unreal world:     X = east, -Y = north, Z = up (centimetres).
//   Every world-facing API already converts. Wind vectors returned to Unreal are world vectors in m/s.

#pragma once

#include "CoreMinimal.h"
#include "AerosTypes.generated.h"

UENUM(BlueprintType)
enum class EAerosPrecipType : uint8
{
	None          UMETA(DisplayName = "None"),
	Rain          UMETA(DisplayName = "Rain"),
	Snow          UMETA(DisplayName = "Snow"),
	Sleet         UMETA(DisplayName = "Sleet"),
	FreezingRain  UMETA(DisplayName = "Freezing Rain"),
	Hail          UMETA(DisplayName = "Hail"),
};

UENUM(BlueprintType)
enum class EAerosPhenomenon : uint8
{
	Clear               UMETA(DisplayName = "Clear"),
	PartlyCloudy        UMETA(DisplayName = "Partly Cloudy"),
	Overcast            UMETA(DisplayName = "Overcast"),
	Fog                 UMETA(DisplayName = "Fog"),
	Drizzle             UMETA(DisplayName = "Drizzle"),
	Rain                UMETA(DisplayName = "Rain"),
	HeavyRain           UMETA(DisplayName = "Heavy Rain"),
	Snow                UMETA(DisplayName = "Snow"),
	Blizzard            UMETA(DisplayName = "Blizzard"),
	FreezingRain        UMETA(DisplayName = "Freezing Rain"),
	Thunderstorm        UMETA(DisplayName = "Thunderstorm"),
	SevereThunderstorm  UMETA(DisplayName = "Severe Thunderstorm"),
	Hail                UMETA(DisplayName = "Hail"),
	Supercell           UMETA(DisplayName = "Supercell"),
	Tornado             UMETA(DisplayName = "Tornado"),
	TropicalStorm       UMETA(DisplayName = "Tropical Storm"),
	Hurricane           UMETA(DisplayName = "Hurricane"),
};

UENUM(BlueprintType)
enum class EAerosStormType : uint8
{
	SingleCell  UMETA(DisplayName = "Single Cell"),
	Multicell   UMETA(DisplayName = "Multicell"),
	Supercell   UMETA(DisplayName = "Supercell"),
};

/** Large-scale weather regime. Climate = derived from latitude + date (seasons). */
UENUM(BlueprintType)
enum class EAerosScenario : uint8
{
	Climate              UMETA(DisplayName = "Climate (seasons)"),
	FairWeather          UMETA(DisplayName = "Fair Weather"),
	SummerThunderstorms  UMETA(DisplayName = "Summer Thunderstorms"),
	SupercellOutbreak    UMETA(DisplayName = "Supercell Outbreak"),
	WinterStorm          UMETA(DisplayName = "Winter Storm"),
	TropicalSeason       UMETA(DisplayName = "Tropical Season"),
	FrontalRain          UMETA(DisplayName = "Frontal Rain"),
	Custom               UMETA(DisplayName = "Custom"),
};

UENUM(BlueprintType)
enum class EAerosSeason : uint8
{
	Winter, Spring, Summer, Autumn
};

/**
 * Synoptic (large-scale) state. Acts as boundary condition and relaxation target.
 * Winds are in the simulation frame (x = east, y = north), m/s.
 */
USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosSynopticState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") float SurfaceTemperature = 20.f;   // C (daily mean)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") float SurfaceDewPoint = 10.f;      // C
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") float Temperature850 = 8.f;        // C (~1.5 km)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") float DewPoint850 = 0.f;           // C
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") float Temperature500 = -18.f;      // C (~5.6 km)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") FVector2D WindSurface = FVector2D(4.0, 0.0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") FVector2D Wind850 = FVector2D(8.0, 0.0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") FVector2D Wind500 = FVector2D(16.0, 0.0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") float SeaSurfaceTemperature = 18.f;// C
	/** Large-scale vertical motion at mid levels, m/s. > 0 fronts / lows (stratiform rain, snow), < 0 subsidence (clear skies). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") float LargeScaleAscent = 0.f;
	/** Horizontal temperature gradient, K per 100 km (x = east, y = north). Fronts / baroclinic zones. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") FVector2D TemperatureGradient = FVector2D::ZeroVector;
	/** Horizontal dew point gradient, K per 100 km. Drylines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Synoptic") FVector2D DewPointGradient = FVector2D::ZeroVector;
};

/** Circular region of open water (oceans, lakes). Feeds moisture, slows heating, fuels hurricanes. */
USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosWaterRegion
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Surface") FVector Center = FVector::ZeroVector; // world cm
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Surface") float RadiusKm = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Surface") float ShoreWidthKm = 4.f;
};

USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosSimulationSettings
{
	GENERATED_BODY()

	/** Grid cells along X. Cost scales with X*Y. 128 = cheap, 256 = high. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Grid", meta = (ClampMin = "16", ClampMax = "1024")) int32 GridSizeX = 128;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Grid", meta = (ClampMin = "16", ClampMax = "1024")) int32 GridSizeY = 128;
	/** Horizontal size of one cell. 1-3 km resolves thunderstorms; 5-10 km for hurricanes / continents. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Grid", meta = (ClampMin = "0.25", ClampMax = "50")) float CellSizeKm = 2.f;
	/** Physics time step in simulated seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Time", meta = (ClampMin = "5", ClampMax = "600")) float PhysicsStepSeconds = 30.f;
	/** Simulated seconds per real second. 60 = one minute per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Time", meta = (ClampMin = "0", ClampMax = "100000")) float TimeScale = 60.f;
	/** Max simulated seconds the sim may lag behind before the clock is held back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Time") float MaxLagSeconds = 600.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Time", meta = (ClampMin = "1", ClampMax = "365")) int32 StartDayOfYear = 172;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Time", meta = (ClampMin = "0", ClampMax = "24")) float StartHour = 10.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|World", meta = (ClampMin = "-89", ClampMax = "89")) float LatitudeDeg = 38.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Weather") EAerosScenario Scenario = EAerosScenario::Climate;
	/** Used when Scenario = Custom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Weather") FAerosSynopticState CustomSynoptic;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Physics") bool bEnableConvection = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Physics") bool bEnableDiurnalCycle = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Physics") bool bEnableTropicalCyclones = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Physics") bool bEnableSurfaceAccumulation = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Surface") TArray<FAerosWaterRegion> WaterRegions;

	/** World location of the domain centre (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|World") FVector DomainCenter = FVector::ZeroVector;
	/** Recentre the domain on the local player when they move far (infinite worlds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|World") bool bFollowPlayer = true;

	/** Seed. Same seed + same inputs = same weather on every machine (multiplayer). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Network") int32 Seed = 1337;

	float CellSizeM() const { return CellSizeKm * 1000.f; }
	float DomainSizeXM() const { return GridSizeX * CellSizeM(); }
	float DomainSizeYM() const { return GridSizeY * CellSizeM(); }
};

/** Weather at one location. Returned by the subsystem queries. */
USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosWeatherSample
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Aeros") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") EAerosPhenomenon Phenomenon = EAerosPhenomenon::Clear;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") EAerosPrecipType PrecipitationType = EAerosPrecipType::None;

	// Surface
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") float Temperature = 15.f;      // C (2 m)
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") float DewPoint = 5.f;          // C
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") float RelativeHumidity = 0.5f; // 0-1
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") float Pressure = 1013.f;       // hPa MSL
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") float GroundTemperature = 15.f;// C
	/** Surface wind, Unreal world vector, m/s. */
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") FVector Wind = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") float WindSpeed = 0.f;         // m/s
	/** Wind near 1.5 km (850 hPa), world vector, m/s. Drives low clouds. */
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") FVector Wind850 = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Surface") float Visibility = 20000.f;    // m

	// Clouds and precipitation
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Clouds") float CloudCover = 0.f;         // 0-1
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Clouds") float CloudBase = 1500.f;       // m AGL
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Clouds") float CloudTop = 2000.f;        // m AGL
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Clouds") float CloudWater = 0.f;         // kg/m2
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Precip") float PrecipitationRate = 0.f;  // mm/h (liquid eq.)
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Precip") float HailDiameter = 0.f;       // mm at ground
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Precip") float Reflectivity = -32.f;     // dBZ
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Precip") float LightningRate = 0.f;      // flashes/min

	// Convection / severe indices
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float ConvectiveActivity = 0.f; // 0-1
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float Updraft = 0.f;      // m/s
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float Mesocyclone = 0.f;  // 0-1
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float CAPE = 0.f;         // J/kg
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float CIN = 0.f;          // J/kg (<=0)
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float LCL = 0.f;          // m
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float LFC = 0.f;          // m
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float EquilibriumLevel = 0.f; // m
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float FreezingLevel = 3000.f; // m
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float LiftedIndex = 0.f;  // K
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float SRH01 = 0.f;        // m2/s2
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float SRH03 = 0.f;        // m2/s2
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float BulkShear06 = 0.f;  // m/s
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float STP = 0.f;          // Significant Tornado Parameter
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float SCP = 0.f;          // Supercell Composite Parameter
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Convection") float SHIP = 0.f;         // Significant Hail Parameter

	// Ground accumulation
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Ground") float Wetness = 0.f;          // 0-1 (puddles > 0.6)
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Ground") float SnowDepth = 0.f;        // cm
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Ground") float HailCover = 0.f;        // 0-1
};

/** A thunderstorm tracked by the simulation (connected convective cells). */
USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosStormData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 Id = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") EAerosStormType Type = EAerosStormType::SingleCell;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") FVector Location = FVector::ZeroVector;   // world cm (ground)
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") FVector Velocity = FVector::ZeroVector;   // world m/s
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float RadiusKm = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 NumCells = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float Intensity = 0.f;      // 0-1
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float MaxReflectivity = 0.f;// dBZ
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float MaxUpdraft = 0.f;     // m/s
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float MaxHail = 0.f;        // mm
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float MaxRainRate = 0.f;    // mm/h
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float CloudTop = 0.f;       // m
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float LightningRate = 0.f;  // flashes/min
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float CAPE = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float SRH01 = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float SRH03 = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float BulkShear06 = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float LCL = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float STP = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float SCP = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float Mesocyclone = 0.f;    // 0-1
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") FVector HailCoreLocation = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") FVector UpdraftLocation = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float Age = 0.f;            // sim seconds
};

/** Tropical cyclone (hurricane / typhoon) vortex. */
USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosCycloneData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 Id = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") FVector Location = FVector::ZeroVector;  // world cm
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") FVector Velocity = FVector::ZeroVector;  // world m/s
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float CentralPressure = 1000.f;          // hPa
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float MaxWind = 0.f;                     // m/s
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float RadiusMaxWindKm = 40.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 Category = 0;                      // Saffir-Simpson
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float Age = 0.f;
};

USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosLightningStrike
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 Sequence = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") FVector Location = FVector::ZeroVector;   // world cm (ground for CG)
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float SimTime = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float PeakCurrent = 30.f;                // kA
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") bool bCloudToGround = true;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 StormId = -1;
};

USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosDateTime
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 DayOfYear = 172;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 Month = 6;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 DayOfMonth = 21;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float Hour = 12.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") EAerosSeason Season = EAerosSeason::Summer;
	/** Sun elevation above horizon, degrees. */
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float SunElevation = 45.f;
	/** Sun azimuth from north, clockwise, degrees. */
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float SunAzimuth = 180.f;
	/** Unit vector pointing to the sun, Unreal world frame. */
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") FVector SunDirection = FVector::UpVector;
};

/** Runtime stats for profiling (stat Aeros / HUD). */
USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosSimStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float SimTime = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float StepMs = 0.f;        // last physics step cost
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") float LagSeconds = 0.f;    // simulated seconds behind target
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 StepsTotal = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 NumCells = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 NumStorms = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros") int32 NumCyclones = 0;
};
