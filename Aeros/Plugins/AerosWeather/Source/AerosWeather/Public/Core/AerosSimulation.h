// Copyright Aeros. All Rights Reserved.
//
// FAerosSimulation — the thermodynamic engine (pure C++, no UObject, thread agnostic).
//
// 2.5D model (roadmap "Grille 2.5D"): a 2D grid of columns. Each column stores three
// atmospheric levels (surface/boundary layer, 850 hPa ~1.5 km, 500 hPa ~5.6 km) plus a
// ground layer. The vertical profile rebuilt from these levels gives CAPE/CIN/LCL/LFC/EL,
// freezing level, helicity and shear with a real parcel ascent.
//
// Per physics step:
//   1. Synoptic forcing (scenario or climate from latitude + date).
//   2. Dynamics: semi-Lagrangian advection, shallow-water pressure/wind adjustment,
//      Coriolis, friction, open boundaries fed by the synoptic state.
//   3. Column physics (parallel): sun + surface energy budget, boundary-layer fluxes,
//      parcel thermodynamics, convection trigger/growth/decay, bulk microphysics,
//      cold pools, hail growth/melt, lightning rate, precip type, ground accumulation.
//   4. Tropical cyclones (Holland vortex nudging), storm labelling + tracking, lightning.

#pragma once

#include "CoreMinimal.h"
#include "AerosTypes.h"

class FAerosSnapshot;

/** One grid column. Plain data, ~100 floats. */
struct FAerosCell
{
	// ---- Prognostic ------------------------------------------------------
	float T = 15.f;        // C   2 m / boundary layer temperature
	float Q = 0.008f;      // kg/kg boundary layer mixing ratio
	float T850 = 5.f;      // C
	float Q850 = 0.004f;   // kg/kg
	float T500 = -20.f;    // C
	float Us = 0.f, Vs = 0.f;       // m/s surface environmental wind (x east, y north)
	float Uo = 0.f, Vo = 0.f;       // m/s density-current wind (outflows, sea breezes); total = Us + Uo
	float U850 = 0.f, V850 = 0.f;   // m/s
	float U500 = 0.f, V500 = 0.f;   // m/s
	float P = 0.f;         // hPa MSLP anomaly from 1013
	float PForced = 0.f;   // hPa part of P imposed by cold pools / thermal lows
	float Cloud = 0.f;     // kg/m2 column condensate
	float Rain = 0.f;      // kg/m2 column precipitation-sized water/ice
	float Conv = 0.f;      // 0-1 convective activity
	float Meso = 0.f;      // 0-1 mesocyclone strength
	float ColdPool = 0.f;  // K (<=0) outflow temperature anomaly
	float Tg = 15.f;       // C ground skin
	float Wet = 0.f;       // 0-1
	float Snow = 0.f;      // cm
	float HailCov = 0.f;   // 0-1

	// ---- Static surface --------------------------------------------------
	float Water = 0.f;     // 0-1 open water fraction

	// ---- Diagnostics -----------------------------------------------------
	float Psfc = 1013.f;
	float Td = 5.f, RH = 0.5f;
	float CAPE = 0.f, CIN = 0.f, LCL = 0.f, LFC = 0.f, EL = 0.f, FZL = 3000.f, LI = 0.f;
	float SRH01 = 0.f, SRH03 = 0.f, Shear06 = 0.f, STP = 0.f, SCP = 0.f, SHIP = 0.f;
	float LapseRate75 = 6.5f;
	float W = 0.f;             // m/s updraft core
	float PrecipRate = 0.f;    // mm/h at ground
	float Hail = 0.f;          // mm at ground
	float DBZ = -32.f;
	float CloudCover = 0.f, CloudBase = 1500.f, CloudTop = 2000.f;
	float Div = 0.f, Div850 = 0.f, Vort = 0.f;
	float Lightning = 0.f;     // flashes/min
	float Visibility = 20000.f;
	float StormU = 0.f, StormV = 0.f; // m/s Bunkers right-mover / steering
	float SensibleFlux = 0.f;  // W/m2
	uint8 PType = 0, Phen = 0;
	uint8 CycloneZone = 0;     // 0 none, 1 tropical storm, 2 hurricane
};

struct FAerosVortex
{
	int32 Id = 0;
	float X = 0.f, Y = 0.f;    // m, domain frame (origin = SW corner)
	float VelX = 0.f, VelY = 0.f;
	float DeltaP = 10.f;       // hPa central deficit
	float Rmax = 40000.f;      // m
	float HollandB = 1.7f;
	float Age = 0.f;
	float MaxWind() const;
};

struct FAerosTrackedStorm
{
	FAerosStormData Data;
	float X = 0.f, Y = 0.f;    // m, domain frame
	bool bMatched = false;
};

class AEROSWEATHER_API FAerosSimulation
{
public:
	FAerosSimulation();
	~FAerosSimulation();

	void Initialize(const FAerosSimulationSettings& InSettings);
	bool IsInitialized() const { return NX > 0; }

	/** Advance one physics step of Dt simulated seconds. */
	void Step(float Dt);

	double GetSimTime() const { return SimTime; }
	int64 GetStepCount() const { return StepCount; }
	const FAerosSimulationSettings& GetSettings() const { return Settings; }
	int32 GetNX() const { return NX; }
	int32 GetNY() const { return NY; }
	const TArray<FAerosCell>& GetCells() const { return Cells; }
	const TArray<FAerosTrackedStorm>& GetStorms() const { return Storms; }
	const TArray<FAerosVortex>& GetVortices() const { return Vortices; }
	FAerosDateTime GetDateTime() const;
	const FAerosSynopticState& GetSynoptic() const { return Synoptic; }

	/** Copy into an immutable snapshot (reuses its memory). */
	void FillSnapshot(FAerosSnapshot& Out) const;

	// ---- Commands (call from the thread that owns the simulation) ---------
	void SetScenario(EAerosScenario Scenario, const FAerosSynopticState* Custom = nullptr);
	void SetTimeScaleHint(float InTimeScale) { Settings.TimeScale = InTimeScale; }
	void SetClock(int32 DayOfYear, float Hour);
	/** Warm/moist bubble: starts convection when the atmosphere allows it. */
	void InjectThermal(const FVector& World, float RadiusKm, float DeltaT, float DeltaQ);
	/** Spawn a tropical cyclone seed / mature storm at a world location. */
	int32 SpawnCyclone(const FVector& World, float CentralPressure, float RmaxKm);
	/** Shift the domain so that it is centred near World (whole cells). */
	void Recenter(const FVector& World);
	void SetWaterRegions(const TArray<FAerosWaterRegion>& Regions);

	// ---- Helpers ----------------------------------------------------------
	FVector DomainToWorld(float Xm, float Ym) const;
	void WorldToDomain(const FVector& World, float& OutXm, float& OutYm) const;
	static FAerosSynopticState MakeScenario(EAerosScenario Scenario, float LatitudeDeg, int32 DayOfYear);

private:
	FAerosCell MakeBackgroundCell(float Xm, float Ym) const;
	void BackgroundAt(float Xm, float Ym, float& OutT, float& OutQ, float& OutT850, float& OutQ850, float& OutT500) const;
	void UpdateSynoptic();
	void UpdateSun();
	void ApplyStaticSurface(int32 I, int32 J, FAerosCell& C) const;

	void StepDynamics(float Dt);
	void StepColumnPhysics(float Dt);
	void ColumnDiagnostics(FAerosCell& C) const;
	float MesoscaleLift(uint32 GI, uint32 GJ) const;
	void ColumnPhysics(int32 I, int32 J, FAerosCell& C, float Dt) const;
	void StepCyclones(float Dt);
	void ApplyCycloneForcing(int32 I, int32 J, FAerosCell& C, float Dt) const;
	void DetectAndTrackStorms(float Interval);
	void GenerateLightning(float Interval);
	void TryCycloneGenesis();

	FAerosSimulationSettings Settings;
	FAerosSynopticState Synoptic;
	int32 NX = 0, NY = 0;
	float DX = 2000.f;     // m
	float Coriolis = 1e-4f;

	TArray<FAerosCell> Cells;
	TArray<FAerosCell> Prev; // dynamics scratch

	TArray<FAerosVortex> Vortices;
	TArray<FAerosTrackedStorm> Storms;
	TArray<int32> Labels;     // storm labelling scratch
	TArray<int32> CompCells;  // cell indices of every storm component
	TArray<FIntPoint> CompOfStorm; // (start, count) into CompCells, per entry of Storms
	TArray<FAerosLightningStrike> Lightning;
	int32 NextStormId = 1;
	int32 NextVortexId = 1;
	int32 NextStrikeSeq = 1;

	FVector GradientOrigin = FVector::ZeroVector;
	int32 CellOffsetX = 0, CellOffsetY = 0; // global cell offset after recentering (net-safe hashing)

	double SimTime = 0.0;
	int64 StepCount = 0;
	double ClockOffsetHours = 0.0;
	float StormTimer = 0.f;
	float GenesisTimer = 0.f;
	float LastStepMs = 0.f;
	float ConvectiveFraction = 0.f; // domain fraction with active convection (previous step)

	// Sun (domain-wide)
	float SinSunElev = 0.f;
	float SunElevDeg = 0.f, SunAzimDeg = 0.f;
	FVector SunDirSim = FVector::UpVector;
};
