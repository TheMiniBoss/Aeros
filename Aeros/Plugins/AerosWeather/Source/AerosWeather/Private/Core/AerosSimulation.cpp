// Copyright Aeros. All Rights Reserved.

#include "Core/AerosSimulation.h"
#include "Core/AerosPhysics.h"
#include "Core/AerosSnapshot.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"


namespace AerosSimInternal
{
	// Tunables (kept here so the physics reads like the roadmap).
	constexpr float GravityWaveSpeed = 50.f;    // m/s, shallow-water equivalent depth ~250 m
	constexpr float EquivalentDepth  = 250.f;   // m
	constexpr float FrictionSfc      = 3.f * 3600.f;
	constexpr float Friction850      = 8.f * 3600.f;
	constexpr float Relax500         = 3.f * 3600.f;
	constexpr float LandHeatCapacity = 2.0e5f;  // J/(m2 K) skin + top soil
	constexpr float WaterHeatCapacity= 4.0e7f;  // J/(m2 K) 10 m mixed layer
	constexpr float UpdraftArea      = 0.03f;   // updraft area fraction at full activity
	constexpr float UpperMass        = 4000.f;  // kg/m2 heated by convective latent release
	constexpr float Layer850Mass     = 2000.f;  // kg/m2 of the 850 layer
	constexpr float DaySeconds       = 86400.f;
	constexpr float SoilCapacity     = 150.f;   // mm plant-available water
	constexpr float ElevPressure     = 0.12f;   // hPa per m of terrain

	const int32 MonthStart[13] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 };

	struct FBilin
	{
		int32 I0, J0, I1, J1;
		float W00, W10, W01, W11;
	};

	FORCEINLINE float Lerp(float A, float B, float T) { return A + (B - A) * T; }
}

float FAerosVortex::MaxWind() const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	// Holland (1980) gradient max wind, reduced to surface (0.8).
	return 0.85f * FMath::Sqrt(HollandB * DeltaP * 100.f / (RhoAir * 2.71828f));
}

FAerosSimulation::FAerosSimulation() = default;
FAerosSimulation::~FAerosSimulation() = default;

// =====================================================================
//  Scenarios / climate
// =====================================================================

FAerosSynopticState FAerosSimulation::MakeScenario(EAerosScenario Scenario, float LatitudeDeg, int32 DayOfYear)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	FAerosSynopticState S;
	auto Set = [&S](float T, float Td, float T850, float Td850, float T500, FVector2D Ws, FVector2D W850, FVector2D W500, float SST)
	{
		S.SurfaceTemperature = T; S.SurfaceDewPoint = Td; S.Temperature850 = T850; S.DewPoint850 = Td850;
		S.Temperature500 = T500; S.WindSurface = Ws; S.Wind850 = W850; S.Wind500 = W500; S.SeaSurfaceTemperature = SST;
	};

	switch (Scenario)
	{
	case EAerosScenario::Blizzard:
		// Arctic outbreak behind a deep low: very cold, strong pressure gradient, snow + blowing snow
		Set(-12.f, -14.f, -15.f, -16.f, -38.f, FVector2D(16, -6), FVector2D(22, -6), FVector2D(30, 2), 2.f);
		S.LargeScaleAscent = 0.06f;
		S.TemperatureGradient = FVector2D(0.0, -3.0);
		S.Aridity = 0.05f;
		break;
	case EAerosScenario::DesertDust:
		// Hot desert with some monsoon moisture aloft: dry microbursts and haboobs, strong winds lift sand
		Set(41.f, 10.f, 28.f, 4.f, -8.f, FVector2D(11, 3), FVector2D(14, 4), FVector2D(20, 2), 28.f);
		S.Aridity = 1.f;
		S.LargeScaleAscent = -0.005f;
		break;
	case EAerosScenario::Monsoon:
		// Saturated tropical air, persistent ascent: training storms, torrential rain, rivers burst
		Set(28.f, 25.f, 19.f, 17.f, -5.f, FVector2D(4, 6), FVector2D(8, 12), FVector2D(4, 3), 30.f);
		S.LargeScaleAscent = 0.07f;
		S.Aridity = 0.f;
		break;
	case EAerosScenario::FairWeather:
		Set(21.f, 7.f, 11.f, -6.f, -13.f, FVector2D(3, 1), FVector2D(6, 1), FVector2D(12, 0), 20.f);
		S.LargeScaleAscent = -0.02f;
		S.Aridity = 0.25f;
		break;
	case EAerosScenario::SummerThunderstorms:
		// Moist, unstable, weak shear: pulse storms and multicells in the afternoon.
		Set(29.f, 19.f, 17.f, 9.f, -11.f, FVector2D(2, 2), FVector2D(4, 3), FVector2D(9, 1), 26.f);
		break;
	case EAerosScenario::SupercellOutbreak:
		// "Loaded gun": rich moisture under an elevated mixed layer, veering winds, strong shear.
		Set(29.f, 21.f, 21.f, 6.f, -14.f, FVector2D(-3, 8), FVector2D(3, 18), FVector2D(26, 9), 26.f);
		S.DewPointGradient = FVector2D(4.0, 0.0); // dryline: drier to the west
		S.Aridity = 0.35f;
		break;
	case EAerosScenario::WinterStorm:
		Set(-4.f, -6.f, -7.f, -9.f, -33.f, FVector2D(-7, 7), FVector2D(-4, 20), FVector2D(25, 14), 8.f);
		S.TemperatureGradient = FVector2D(0.0, -2.0);
		S.LargeScaleAscent = 0.06f;
		break;
	case EAerosScenario::TropicalSeason:
		Set(28.f, 24.f, 17.f, 14.f, -6.f, FVector2D(-5, 0.5), FVector2D(-6, 1), FVector2D(-3, 1), 30.f);
		S.LargeScaleAscent = 0.01f;
		S.Aridity = 0.05f;
		break;
	case EAerosScenario::FrontalRain:
		Set(13.f, 11.f, 7.f, 6.f, -21.f, FVector2D(4, 6), FVector2D(14, 12), FVector2D(30, 6), 14.f);
		S.TemperatureGradient = FVector2D(0.0, -3.0);
		S.LargeScaleAscent = 0.05f;
		S.Aridity = 0.05f;
		break;
	case EAerosScenario::Climate:
	case EAerosScenario::Custom:
	default:
	{
		const float Lat = FMath::Abs(LatitudeDeg);
		const float HemiShift = LatitudeDeg < 0.f ? 182.f : 0.f;
		const float Season = 0.5f + 0.5f * FMath::Cos(2.f * PI * (DayOfYear - 200.f - HemiShift) / 365.f); // 1 = mid summer
		const float SeasonLag = 0.5f + 0.5f * FMath::Cos(2.f * PI * (DayOfYear - 230.f - HemiShift) / 365.f);
		const float Mean = 30.f - 0.4f * Lat;
		const float Amp = 1.5f + 0.28f * Lat;
		const float T = Mean + Amp * (2.f * Season - 1.f);
		const float Tropic = FMath::Clamp((25.f - Lat) / 10.f, 0.f, 1.f); // 1 = tropics
		const float Td = T - Lerp(Lerp(9.f, 6.f, Season), 4.f, Tropic);
		const float T850 = T - Lerp(7.f, 10.f, Season);
		const float Td850 = T850 - Lerp(10.f, 7.f, Season);
		const float T500 = T - Lerp(Lerp(28.f, 35.f, Season), 34.f, Tropic);
		const FVector2D West500(Lerp(32.f, 16.f, Season), 3.f), West850(Lerp(14.f, 7.f, Season), 4.f), WestS(Lerp(6.f, 3.f, Season), 3.f);
		const FVector2D East500(-5, 0), East850(-7, 0.5), EastS(-6, 0.5);
		Set(T, Td, T850, Td850, T500,
			FMath::Lerp(WestS, EastS, Tropic), FMath::Lerp(West850, East850, Tropic), FMath::Lerp(West500, East500, Tropic),
			Mean + Amp * 0.6f * (2.f * SeasonLag - 1.f) + 1.f);
		S.LargeScaleAscent = Lerp(0.015f, -0.005f, Season) * (1.f - Tropic);
		S.Aridity = Lerp(0.1f, 0.3f, Season);
		break;
	}
	}
	return S;
}

// =====================================================================
//  Init
// =====================================================================

void FAerosSimulation::Initialize(const FAerosSimulationSettings& InSettings)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	Settings = InSettings;
	NX = FMath::Clamp(Settings.GridSizeX, 16, 1024);
	NY = FMath::Clamp(Settings.GridSizeY, 16, 1024);
	DX = Settings.CellSizeM();
	Coriolis = CoriolisF(Settings.LatitudeDeg);
	if (FMath::Abs(Coriolis) < 1e-5f) Coriolis = (Settings.LatitudeDeg < 0.f ? -1.f : 1.f) * 1e-5f;

	SimTime = 0.0;
	StepCount = 0;
	ClockOffsetHours = 0.0;
	StormTimer = 0.f;
	GenesisTimer = 0.f;
	Vortices.Reset();
	Storms.Reset();
	Lightning.Reset();
	NextStormId = NextVortexId = NextStrikeSeq = NextEmitterId = 1;
	Emitters.Reset();
	bOceanActive = false;
	MaxFlood = 0.f;
	GradientOrigin = Settings.DomainCenter;
	CellOffsetX = CellOffsetY = 0;

	UpdateSynoptic();
	UpdateSun();

	Cells.SetNum(NX * NY);
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			FAerosCell& C = Cells[I + J * NX];
			C = MakeBackgroundCell((I + 0.5f) * DX, (J + 0.5f) * DX);
			ApplyStaticSurface(I, J, C);
			// Tiny deterministic noise breaks symmetry (convection needs it).
			C.T += (Hash01(Settings.Seed, I, J, 7) - 0.5f) * 0.4f;
			C.Tg = C.Water > 0.5f ? Synoptic.SeaSurfaceTemperature : C.T;
			C.Soil = SoilCapacity * Lerp(0.55f, 0.1f, FMath::Max(Synoptic.Aridity, C.Arid));
			if (C.Water < 0.5f && C.T < -2.f) C.Snow = FMath::Clamp((-C.T - 1.f) * 2.f, 0.f, 25.f); // existing snowpack in cold climates
			ColumnDiagnostics(C);
		}
	}
	Prev = Cells;
	FloodOut.SetNumZeroed(NX * NY);
	Labels.SetNumUninitialized(NX * NY);
}

void FAerosSimulation::BackgroundAt(float Xm, float Ym, float& OutT, float& OutQ, float& OutT850, float& OutQ850, float& OutT500) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	// Offset from the gradient origin, km, clamped so far travel does not explode values.
	const FVector World = DomainToWorld(Xm, Ym);
	const float Ekm = FMath::Clamp(float(World.X - GradientOrigin.X) * 1e-5f, -400.f, 400.f);
	const float Nkm = FMath::Clamp(-float(World.Y - GradientOrigin.Y) * 1e-5f, -400.f, 400.f);
	const float DT = float(Synoptic.TemperatureGradient.X) * Ekm * 0.01f + float(Synoptic.TemperatureGradient.Y) * Nkm * 0.01f;
	const float DTd = float(Synoptic.DewPointGradient.X) * Ekm * 0.01f + float(Synoptic.DewPointGradient.Y) * Nkm * 0.01f;

	OutT = Synoptic.SurfaceTemperature + DT;
	const float Td = FMath::Min(Synoptic.SurfaceDewPoint + DT + DTd, OutT);
	OutQ = MixingRatioFromDewPoint(Td, StdPressure);
	OutT850 = Synoptic.Temperature850 + DT * 0.8f;
	OutQ850 = MixingRatioFromDewPoint(FMath::Min(Synoptic.DewPoint850 + DT * 0.8f + DTd * 0.5f, OutT850), 850.f);
	OutT500 = Synoptic.Temperature500 + DT * 0.6f;
}

FAerosCell FAerosSimulation::MakeBackgroundCell(float Xm, float Ym) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	FAerosCell C;
	BackgroundAt(Xm, Ym, C.T, C.Q, C.T850, C.Q850, C.T500);
	C.Us = Synoptic.WindSurface.X;  C.Vs = Synoptic.WindSurface.Y;
	C.U850 = Synoptic.Wind850.X;    C.V850 = Synoptic.Wind850.Y;
	C.U500 = Synoptic.Wind500.X;    C.V500 = Synoptic.Wind500.Y;
	C.Tg = C.T;
	C.StormU = (C.Us + C.U850 + C.U500) / 3.f;
	C.StormV = (C.Vs + C.V850 + C.V500) / 3.f;
	return C;
}

void FAerosSimulation::ApplyStaticSurface(int32 I, int32 J, FAerosCell& C) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	const FVector World = DomainToWorld((I + 0.5f) * DX, (J + 0.5f) * DX);
	C.Water = 0.f;
	C.Bathy = 0.f;
	for (const FAerosWaterRegion& R : Settings.WaterRegions)
	{
		const float Dkm = float(FVector::Dist2D(World, R.Center)) * 1e-5f;
		const float Shore = FMath::Max(R.ShoreWidthKm, 0.1f);
		C.Water = FMath::Max(C.Water, FMath::Clamp((R.RadiusKm - Dkm) / Shore + 0.5f, 0.f, 1.f));
		// Continental shelf (~3 m/km for 50 km) then slope to the abyss
		const float Inside = R.RadiusKm - Dkm;
		if (Inside > 0.f)
		{
			const float Depth = Inside < 50.f ? 20.f + 3.f * Inside : 170.f + 40.f * (Inside - 50.f);
			C.Bathy = FMath::Max(C.Bathy, FMath::Min(Depth, 4000.f));
		}
	}
	// Terrain sampled below sea level is ocean too
	if (C.Elev < -0.5f)
	{
		C.Water = 1.f;
		C.Bathy = FMath::Max(C.Bathy, FMath::Min(-C.Elev, 6000.f));
	}
	if (C.Water >= 0.5f) C.Bathy = FMath::Max(C.Bathy, 10.f);

	C.Arid = 0.f;
	for (const FAerosDesertRegion& R : Settings.DesertRegions)
	{
		const float Dkm = float(FVector::Dist2D(World, R.Center)) * 1e-5f;
		C.Arid = FMath::Max(C.Arid, FMath::Clamp((R.RadiusKm - Dkm) / FMath::Max(R.EdgeWidthKm, 0.1f) + 0.5f, 0.f, 1.f));
	}
}

float FAerosSimulation::ShoreDistanceKm(const FVector& World) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	float Best = -1e6f;
	for (const FAerosWaterRegion& R : Settings.WaterRegions)
	{
		Best = FMath::Max(Best, R.RadiusKm - float(FVector::Dist2D(World, R.Center)) * 1e-5f);
	}
	return Best;
}

void FAerosSimulation::RecomputeSlopes()
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	const float Inv = 1.f / (2.f * DX);
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			auto E = [this](int32 X, int32 Y) { return FMath::Max(Cells[FMath::Clamp(X, 0, NX - 1) + FMath::Clamp(Y, 0, NY - 1) * NX].Elev, 0.f); };
			FAerosCell& C = Cells[I + J * NX];
			C.ElevDX = FMath::Clamp((E(I + 1, J) - E(I - 1, J)) * Inv, -0.3f, 0.3f);
			C.ElevDY = FMath::Clamp((E(I, J + 1) - E(I, J - 1)) * Inv, -0.3f, 0.3f);
		}
	}
}

void FAerosSimulation::SetElevation(const TArray<float>& HeightsM, const FVector& SampledCenter)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	if (HeightsM.Num() != NX * NY) return;
	if (FVector::Dist2D(SampledCenter, Settings.DomainCenter) > DX * 50.0) return; // domain moved meanwhile
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			FAerosCell& C = Cells[I + J * NX];
			C.Elev = HeightsM[I + J * NX];
			ApplyStaticSurface(I, J, C);
		}
	}
	RecomputeSlopes();
}

void FAerosSimulation::SetDesertRegions(const TArray<FAerosDesertRegion>& Regions)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	Settings.DesertRegions = Regions;
	for (int32 J = 0; J < NY; ++J)
		for (int32 I = 0; I < NX; ++I)
			ApplyStaticSurface(I, J, Cells[I + J * NX]);
}

void FAerosSimulation::SetLatitude(float LatitudeDeg)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	Settings.LatitudeDeg = FMath::Clamp(LatitudeDeg, -89.f, 89.f);
	Coriolis = CoriolisF(Settings.LatitudeDeg);
	if (FMath::Abs(Coriolis) < 1e-5f) Coriolis = (Settings.LatitudeDeg < 0.f ? -1.f : 1.f) * 1e-5f;
	UpdateSynoptic();
	UpdateSun();
}

void FAerosSimulation::SetWaterRegions(const TArray<FAerosWaterRegion>& Regions)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	Settings.WaterRegions = Regions;
	for (int32 J = 0; J < NY; ++J)
		for (int32 I = 0; I < NX; ++I)
			ApplyStaticSurface(I, J, Cells[I + J * NX]);
}

// =====================================================================
//  Clock / sun
// =====================================================================

FAerosDateTime FAerosSimulation::GetDateTime() const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	FAerosDateTime D;
	const double TotalHours = (Settings.StartDayOfYear - 1) * 24.0 + Settings.StartHour + ClockOffsetHours + SimTime / 3600.0;
	const int64 Days = FMath::FloorToInt64(TotalHours / 24.0);
	D.DayOfYear = int32(((Days % 365) + 365) % 365) + 1;
	D.Hour = float(TotalHours - Days * 24.0);
	int32 M = 0;
	while (M < 11 && D.DayOfYear > MonthStart[M + 1]) ++M;
	D.Month = M + 1;
	D.DayOfMonth = D.DayOfYear - MonthStart[M];

	int32 Doy = D.DayOfYear;
	if (Settings.LatitudeDeg < 0.f) Doy = (Doy + 182) % 365 + 1;
	if (Doy >= 80 && Doy < 172) D.Season = EAerosSeason::Spring;
	else if (Doy >= 172 && Doy < 265) D.Season = EAerosSeason::Summer;
	else if (Doy >= 265 && Doy < 355) D.Season = EAerosSeason::Autumn;
	else D.Season = EAerosSeason::Winter;

	D.SunElevation = SunElevDeg;
	D.SunAzimuth = SunAzimDeg;
	D.SunDirection = FAerosSnapshot::SimToWorldDir(SunDirSim.X, SunDirSim.Y, SunDirSim.Z);
	return D;
}

void FAerosSimulation::SetClock(int32 DayOfYear, float Hour)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	const double Current = (Settings.StartDayOfYear - 1) * 24.0 + Settings.StartHour + ClockOffsetHours + SimTime / 3600.0;
	const double Desired = (FMath::Clamp(DayOfYear, 1, 365) - 1) * 24.0 + FMath::Clamp(Hour, 0.f, 24.f);
	const double CurrentInYear = FMath::Fmod(Current, 365.0 * 24.0);
	ClockOffsetHours += Desired - CurrentInYear;
	UpdateSynoptic();
	UpdateSun();
}

void FAerosSimulation::UpdateSun()
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	const FAerosDateTime D = [this]()
	{
		FAerosDateTime T;
		const double TotalHours = (Settings.StartDayOfYear - 1) * 24.0 + Settings.StartHour + ClockOffsetHours + SimTime / 3600.0;
		const int64 Days = FMath::FloorToInt64(TotalHours / 24.0);
		T.DayOfYear = int32(((Days % 365) + 365) % 365) + 1;
		T.Hour = float(TotalHours - Days * 24.0);
		return T;
	}();

	const float Lat = FMath::DegreesToRadians(Settings.LatitudeDeg);
	const float Decl = FMath::DegreesToRadians(-23.44f) * FMath::Cos(2.f * PI * (D.DayOfYear + 10.f) / 365.f);
	const float HourAngle = FMath::DegreesToRadians(15.f * (D.Hour - 12.f));
	const float SinH = FMath::Sin(Lat) * FMath::Sin(Decl) + FMath::Cos(Lat) * FMath::Cos(Decl) * FMath::Cos(HourAngle);
	const float H = FMath::Asin(FMath::Clamp(SinH, -1.f, 1.f));
	const float CosH = FMath::Max(FMath::Cos(H), 1e-4f);
	const float SinA = -FMath::Sin(HourAngle) * FMath::Cos(Decl) / CosH;
	const float CosA = (FMath::Sin(Decl) - SinH * FMath::Sin(Lat)) / (CosH * FMath::Max(FMath::Cos(Lat), 1e-4f));
	const float A = FMath::Atan2(SinA, CosA);

	SinSunElev = FMath::Max(SinH, 0.f);
	SunElevDeg = FMath::RadiansToDegrees(H);
	SunAzimDeg = FMath::Fmod(FMath::RadiansToDegrees(A) + 360.f, 360.f);
	SunDirSim = FVector(FMath::Sin(A) * CosH, FMath::Cos(A) * CosH, SinH);
}

void FAerosSimulation::UpdateSynoptic()
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	const FAerosDateTime D = GetDateTime();
	if (Settings.Scenario == EAerosScenario::Custom)
		Synoptic = Settings.CustomSynoptic;
	else
		Synoptic = MakeScenario(Settings.Scenario, Settings.LatitudeDeg, D.DayOfYear);
}

void FAerosSimulation::SetScenario(EAerosScenario Scenario, const FAerosSynopticState* Custom)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	Settings.Scenario = Scenario;
	if (Custom) Settings.CustomSynoptic = *Custom;
	UpdateSynoptic();
}

// =====================================================================
//  Coordinates
// =====================================================================

FVector FAerosSimulation::DomainToWorld(float Xm, float Ym) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	return FVector(Settings.DomainCenter.X + (Xm - NX * DX * 0.5f) * 100.0,
	               Settings.DomainCenter.Y - (Ym - NY * DX * 0.5f) * 100.0, 0.0);
}

void FAerosSimulation::WorldToDomain(const FVector& World, float& OutXm, float& OutYm) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	OutXm = float(World.X - Settings.DomainCenter.X) * 0.01f + NX * DX * 0.5f;
	OutYm = -float(World.Y - Settings.DomainCenter.Y) * 0.01f + NY * DX * 0.5f;
}

// =====================================================================
//  Step
// =====================================================================

void FAerosSimulation::Step(float Dt)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	if (!IsInitialized() || Dt <= 0.f) return;
	TRACE_CPUPROFILER_EVENT_SCOPE(Aeros_Step);
	const double T0 = FPlatformTime::Seconds();

	UpdateSynoptic();
	UpdateSun();
	StepDynamics(Dt);
	StepEmitters(Dt);
	StepColumnPhysics(Dt);
	if (bOceanActive || Vortices.Num() > 0) StepOcean(Dt);
	if (Settings.bEnableHydrology) StepHydrology(Dt);
	if (Settings.bEnableTropicalCyclones) StepCyclones(Dt);

	StormTimer += Dt;
	if (StormTimer >= 60.f)
	{
		DetectAndTrackStorms(StormTimer);
		GenerateLightning(StormTimer);
		StormTimer = 0.f;
	}

	GenesisTimer += Dt;
	if (GenesisTimer >= 6.f * 3600.f)
	{
		GenesisTimer = 0.f;
		if (Settings.bEnableTropicalCyclones) TryCycloneGenesis();
	}

	SimTime += Dt;
	++StepCount;
	LastStepMs = float((FPlatformTime::Seconds() - T0) * 1000.0);
}

// =====================================================================
//  Dynamics: semi-Lagrangian advection + shallow-water adjustment
// =====================================================================

void FAerosSimulation::StepDynamics(float Dt)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	TRACE_CPUPROFILER_EVENT_SCOPE(Aeros_Dynamics);
	const float MaxSub = 0.5f * DX / GravityWaveSpeed;
	const int32 NumSub = FMath::Max(1, FMath::CeilToInt(Dt / MaxSub));
	const float H = Dt / NumSub;
	const float InvDX = 1.f / DX;
	const float PressureCoef = RhoAir * G * EquivalentDepth / 100.f; // hPa per unit divergence*time
	const float CosF = FMath::Cos(Coriolis * H), SinF = FMath::Sin(Coriolis * H);
	const float SynUs = Synoptic.WindSurface.X, SynVs = Synoptic.WindSurface.Y;
	const float Syn8U = Synoptic.Wind850.X, Syn8V = Synoptic.Wind850.Y;
	const float Syn5U = Synoptic.Wind500.X, Syn5V = Synoptic.Wind500.Y;

	for (int32 Sub = 0; Sub < NumSub; ++Sub)
	{
		Prev = Cells;
		const FAerosCell* P = Prev.GetData();

		auto MakeBilin = [this](float FX, float FY, FBilin& B) -> bool
		{
			// FX/FY in cell-centre coords (cell i centre = i)
			if (FX < -0.5f || FY < -0.5f || FX > NX - 0.5f || FY > NY - 0.5f) return false;
			FX = FMath::Clamp(FX, 0.f, float(NX - 1));
			FY = FMath::Clamp(FY, 0.f, float(NY - 1));
			B.I0 = FMath::Min(int32(FX), NX - 1); B.J0 = FMath::Min(int32(FY), NY - 1);
			B.I1 = FMath::Min(B.I0 + 1, NX - 1);  B.J1 = FMath::Min(B.J0 + 1, NY - 1);
			const float TX = FX - B.I0, TY = FY - B.J0;
			B.W00 = (1 - TX) * (1 - TY); B.W10 = TX * (1 - TY); B.W01 = (1 - TX) * TY; B.W11 = TX * TY;
			return true;
		};
		auto Get = [this, P](const FBilin& B, float FAerosCell::* M)
		{
			return P[B.I0 + B.J0 * NX].*M * B.W00 + P[B.I1 + B.J0 * NX].*M * B.W10
			     + P[B.I0 + B.J1 * NX].*M * B.W01 + P[B.I1 + B.J1 * NX].*M * B.W11;
		};
		auto At = [this, P](int32 I, int32 J) -> const FAerosCell&
		{
			return P[FMath::Clamp(I, 0, NX - 1) + FMath::Clamp(J, 0, NY - 1) * NX];
		};

		auto Cubic = [this, P](float FX, float FY, float FAerosCell::* M)
		{
			FX = FMath::Clamp(FX, 0.f, float(NX - 1));
			FY = FMath::Clamp(FY, 0.f, float(NY - 1));
			const int32 IX = FMath::Min(int32(FX), NX - 2 < 0 ? 0 : NX - 2), IY = FMath::Min(int32(FY), NY - 2 < 0 ? 0 : NY - 2);
			const float TX = FX - IX, TY = FY - IY;
			auto V = [&](int32 X, int32 Y) { return P[FMath::Clamp(X, 0, NX - 1) + FMath::Clamp(Y, 0, NY - 1) * NX].*M; };
			auto CR = [](float A, float B, float C, float D, float T)
			{
				return B + 0.5f * T * (C - A + T * (2.f * A - 5.f * B + 4.f * C - D + T * (3.f * (B - C) + D - A)));
			};
			float Row[4];
			for (int32 K = 0; K < 4; ++K)
			{
				const int32 Y = IY - 1 + K;
				Row[K] = CR(V(IX - 1, Y), V(IX, Y), V(IX + 1, Y), V(IX + 2, Y), TX);
			}
			const float R = CR(Row[0], Row[1], Row[2], Row[3], TY);
			const float A = V(IX, IY), B = V(IX + 1, IY), C = V(IX, IY + 1), D = V(IX + 1, IY + 1);
			return FMath::Clamp(R, FMath::Min(FMath::Min(A, B), FMath::Min(C, D)), FMath::Max(FMath::Max(A, B), FMath::Max(C, D)));
		};

		// ---- Pass 1: advection + momentum ------------------------------------
		ParallelFor(NY, [&](int32 J)
		{
			for (int32 I = 0; I < NX; ++I)
			{
				const int32 Idx = I + J * NX;
				const FAerosCell& Src = P[Idx];
				FAerosCell& C = Cells[Idx];
				const float Xm = (I + 0.5f) * DX, Ym = (J + 0.5f) * DX;
				const float S = H * InvDX;
				FBilin B;
				bool bBgDone = false;
				FAerosCell Bg;
				auto Background = [&]() -> const FAerosCell&
				{
					if (!bBgDone) { Bg = MakeBackgroundCell(Xm, Ym); bBgDone = true; }
					return Bg;
				};

				// Surface group (moves with the total surface wind)
				if (MakeBilin(I - (Src.Us + Src.Uo) * S, J - (Src.Vs + Src.Vo) * S, B))
				{
					C.T = Get(B, &FAerosCell::T);   C.Q = Get(B, &FAerosCell::Q);
					C.Us = Get(B, &FAerosCell::Us); C.Vs = Get(B, &FAerosCell::Vs);
					C.Uo = Get(B, &FAerosCell::Uo); C.Vo = Get(B, &FAerosCell::Vo);
					C.P = Get(B, &FAerosCell::P);   C.ColdPool = Get(B, &FAerosCell::ColdPool);
					C.Dust = Get(B, &FAerosCell::Dust);
				}
				else
				{
					const FAerosCell& G0 = Background();
					C.T = G0.T; C.Q = G0.Q; C.Us = G0.Us; C.Vs = G0.Vs; C.Uo = C.Vo = 0.f; C.P = 0.f; C.ColdPool = 0.f; C.Dust = 0.f;
				}
				// 850 group
				if (MakeBilin(I - Src.U850 * S, J - Src.V850 * S, B))
				{
					C.T850 = Get(B, &FAerosCell::T850); C.Q850 = Get(B, &FAerosCell::Q850);
					C.U850 = Get(B, &FAerosCell::U850); C.V850 = Get(B, &FAerosCell::V850);
				}
				else
				{
					const FAerosCell& G0 = Background();
					C.T850 = G0.T850; C.Q850 = G0.Q850; C.U850 = G0.U850; C.V850 = G0.V850;
				}
				// 500 group
				if (MakeBilin(I - Src.U500 * S, J - Src.V500 * S, B))
				{
					C.T500 = Get(B, &FAerosCell::T500);
					C.U500 = Get(B, &FAerosCell::U500); C.V500 = Get(B, &FAerosCell::V500);
					C.Ash = Get(B, &FAerosCell::Ash);
				}
				else
				{
					const FAerosCell& G0 = Background();
					C.T500 = G0.T500; C.U500 = G0.U500; C.V500 = G0.V500; C.Ash = 0.f;
				}
				// Storm group: clouds, rain and convection move with the storm motion.
				// Monotone cubic keeps compact storm cells from smearing out (bilinear is too diffusive).
				const float SFX = I - Src.StormU * S, SFY = J - Src.StormV * S;
				if (MakeBilin(SFX, SFY, B))
				{
					C.Cloud = Cubic(SFX, SFY, &FAerosCell::Cloud); C.Rain = Cubic(SFX, SFY, &FAerosCell::Rain);
					C.Conv = Cubic(SFX, SFY, &FAerosCell::Conv);   C.Meso = Cubic(SFX, SFY, &FAerosCell::Meso);
				}
				else
				{
					C.Cloud = C.Rain = C.Conv = C.Meso = 0.f;
				}

				// Pressure gradient (Pa/m) from previous pressure
				const float DPdx = (At(I + 1, J).P - At(I - 1, J).P) * 100.f * 0.5f * InvDX;
				const float DPdy = (At(I, J + 1).P - At(I, J - 1).P) * 100.f * 0.5f * InvDX;
				const float DFdx = (At(I + 1, J).PForced - At(I - 1, J).PForced) * 100.f * 0.5f * InvDX;
				const float DFdy = (At(I, J + 1).PForced - At(I, J - 1).PForced) * 100.f * 0.5f * InvDX;

				// Light Laplacian smoothing kills the collocated-grid checkerboard mode
				auto Lap = [&](float FAerosCell::* M)
				{
					return At(I + 1, J).*M + At(I - 1, J).*M + At(I, J + 1).*M + At(I, J - 1).*M - 4.f * Src.*M;
				};

				// Surface wind: perturbation from synoptic gets PGF, Coriolis rotation, friction
				{
					float DU = C.Us - SynUs - H * DPdx / RhoAir + 0.04f * Lap(&FAerosCell::Us);
					float DV = C.Vs - SynVs - H * DPdy / RhoAir + 0.04f * Lap(&FAerosCell::Vs);
					const float RU = DU * CosF + DV * SinF;
					const float RV = -DU * SinF + DV * CosF;
					const float Fr = 1.f / (1.f + H / FrictionSfc);
					C.Us = SynUs + RU * Fr;
					C.Vs = SynVs + RV * Fr;
				}
				// Density currents: pushed by the hydrostatic (cold pool / thermal) pressure,
				// they do not feed the gravity-wave continuity, so gust fronts keep running.
				{
					const float Fr = 1.f / (1.f + H / 1500.f);
					C.Uo = FMath::Clamp((C.Uo - H * DFdx / RhoAir + 0.04f * Lap(&FAerosCell::Uo)) * Fr, -35.f, 35.f);
					C.Vo = FMath::Clamp((C.Vo - H * DFdy / RhoAir + 0.04f * Lap(&FAerosCell::Vo)) * Fr, -35.f, 35.f);
				}
				// 850 wind: weaker pressure coupling (shallow cold pools), less friction
				{
					float DU = C.U850 - Syn8U - 0.3f * H * DPdx / RhoAir;
					float DV = C.V850 - Syn8V - 0.3f * H * DPdy / RhoAir;
					const float RU = DU * CosF + DV * SinF;
					const float RV = -DU * SinF + DV * CosF;
					const float Fr = 1.f / (1.f + H / Friction850);
					C.U850 = Syn8U + RU * Fr;
					C.V850 = Syn8V + RV * Fr;
				}
				// 500 wind: relax to synoptic
				{
					const float A = H / Relax500;
					C.U500 += (Syn5U - C.U500) * A;
					C.V500 += (Syn5V - C.V500) * A;
				}
				C.P += 0.08f * Lap(&FAerosCell::P);
			}
		});

		// ---- Pass 2: continuity (pressure from divergence), diagnostics ------
		const FAerosCell* N = Cells.GetData();
		ParallelFor(NY, [&](int32 J)
		{
			for (int32 I = 0; I < NX; ++I)
			{
				auto WindS = [&](int32 X, int32 Y, float& U, float& V)
				{
					if (X < 0 || Y < 0 || X >= NX || Y >= NY) { U = SynUs; V = SynVs; return; }
					U = N[X + Y * NX].Us; V = N[X + Y * NX].Vs;
				};
				auto Wind8 = [&](int32 X, int32 Y, float& U, float& V)
				{
					if (X < 0 || Y < 0 || X >= NX || Y >= NY) { U = Syn8U; V = Syn8V; return; }
					U = N[X + Y * NX].U850; V = N[X + Y * NX].V850;
				};
				auto WindO = [&](int32 X, int32 Y, float& U, float& V)
				{
					if (X < 0 || Y < 0 || X >= NX || Y >= NY) { U = V = 0.f; return; }
					U = N[X + Y * NX].Uo; V = N[X + Y * NX].Vo;
				};
				float UE, VE, UW, VW, UN, VN, US, VS;
				WindS(I + 1, J, UE, VE); WindS(I - 1, J, UW, VW); WindS(I, J + 1, UN, VN); WindS(I, J - 1, US, VS);
				const float Div = (UE - UW + VN - VS) * 0.5f * InvDX;
				float Vort = (VE - VW - (UN - US)) * 0.5f * InvDX;
				WindO(I + 1, J, UE, VE); WindO(I - 1, J, UW, VW); WindO(I, J + 1, UN, VN); WindO(I, J - 1, US, VS);
				const float DivO = (UE - UW + VN - VS) * 0.5f * InvDX;
				Vort += (VE - VW - (UN - US)) * 0.5f * InvDX;
				Wind8(I + 1, J, UE, VE); Wind8(I - 1, J, UW, VW); Wind8(I, J + 1, UN, VN); Wind8(I, J - 1, US, VS);
				const float Div8 = (UE - UW + VN - VS) * 0.5f * InvDX;

				FAerosCell& C = Cells[I + J * NX];
				C.Div = Div + DivO;           // total: gust-front convergence triggers new cells
				C.Div850 = Div8;
				C.ColdPool *= 1.f / (1.f + H * FMath::Max(DivO, 0.f)); // spreading thins the pool
				C.Vort = Vort;
				C.P = FMath::Clamp(C.P - H * PressureCoef * Div, -120.f, 60.f);
				C.P *= 1.f / (1.f + H / (2.f * 3600.f)); // drift damping (mass returns aloft)
			}
		});
	}
}

// =====================================================================
//  Column physics
// =====================================================================

void FAerosSimulation::StepColumnPhysics(float Dt)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	TRACE_CPUPROFILER_EVENT_SCOPE(Aeros_ColumnPhysics);
	ParallelFor(NY, [&](int32 J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			ColumnPhysics(I, J, Cells[I + J * NX], Dt);
		}
	});

	int32 Active = 0;
	for (const FAerosCell& C : Cells) Active += C.Conv > 0.1f ? 1 : 0;
	ConvectiveFraction = float(Active) / float(NX * NY);
}

void FAerosSimulation::ColumnDiagnostics(FAerosCell& C) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	C.Psfc = StdPressure + C.P + C.PForced - FMath::Max(C.Elev, 0.f) * ElevPressure;
	C.Td = FMath::Min(DewPoint(C.Q, C.Psfc), C.T);
	C.RH = RelativeHumidity(C.T, C.Q, C.Psfc);

	// ---- Vertical profile ---------------------------------------------------
	constexpr int32 NL = 7;
	float Lp[NL] = { C.Psfc, 850.f, 700.f, 500.f, 300.f, 200.f, 150.f };
	const float T700 = C.T850 + 0.364f * (C.T500 - C.T850);
	const float T300 = C.T500 - 23.f;
	const float T200 = FMath::Max(T300 - 12.f, -62.f);
	float LT[NL] = { C.T, C.T850, T700, C.T500, T300, T200, T200 };
	float Lq[NL] = { C.Q, C.Q850, 0.55f * C.Q850, 0.25f * C.Q850, 0.05f * C.Q850, 0.01f * C.Q850, 0.005f * C.Q850 };
	for (int32 K = 1; K < NL; ++K) Lq[K] = FMath::Min(Lq[K], SatMixingRatio(LT[K], Lp[K]));
	if (Lp[0] < 860.f) Lp[0] = 860.f;
	float LZ[NL];
	LZ[0] = 0.f;
	for (int32 K = 1; K < NL; ++K)
	{
		const float TvMean = 0.5f * (VirtualTemp(LT[K - 1] + K0, Lq[K - 1]) + VirtualTemp(LT[K] + K0, Lq[K]));
		LZ[K] = LZ[K - 1] + Rd * TvMean / G * FMath::Loge(Lp[K - 1] / Lp[K]);
	}

	// Freezing level
	C.FZL = 0.f;
	if (LT[0] > 0.f)
	{
		C.FZL = LZ[NL - 1];
		for (int32 K = 1; K < NL; ++K)
		{
			if (LT[K] <= 0.f)
			{
				const float F = LT[K - 1] / (LT[K - 1] - LT[K]);
				C.FZL = LZ[K - 1] + F * (LZ[K] - LZ[K - 1]);
				break;
			}
		}
	}
	C.LapseRate75 = (T700 - C.T500) / FMath::Max(LZ[3] - LZ[2], 1.f) * 1000.f;

	// ---- Surface-based parcel ascent (CAPE, CIN, LCL, LFC, EL, LI) ----------
	const float T0K = C.T + K0;
	C.LCL = LCLHeight(C.T, C.Td);
	const float TlclK = T0K - 0.0098f * C.LCL;
	const float Plcl = C.Psfc * FMath::Pow(TlclK / T0K, 1.f / Kappa);

	float CAPE = 0.f, CIN = 0.f, LFC = 0.f, EL = 0.f, Tp500 = T0K;
	bool bLFC = false;
	float Pc = C.Psfc, Tp = T0K, DiffPrev = 0.f;
	int32 K = 0;
	constexpr float DP = 25.f;
	while (Pc - DP > 150.f)
	{
		const float Pn = Pc - DP;
		float Tpn;
		if (Pn >= Plcl)
		{
			Tpn = T0K * FMath::Pow(Pn / C.Psfc, Kappa);
		}
		else
		{
			float PStart = Pc, TStart = Tp;
			if (Pc > Plcl) { PStart = Plcl; TStart = TlclK; }
			const float H = Pn - PStart; // negative
			const float K1 = MoistAdiabatDTdP(TStart, PStart);
			const float K2 = MoistAdiabatDTdP(TStart + K1 * H * 0.5f, PStart + H * 0.5f);
			Tpn = TStart + K2 * H;
		}
		const float Qpn = Pn >= Plcl ? C.Q : SatMixingRatio(Tpn - K0, Pn);

		while (K < NL - 2 && Lp[K + 1] > Pn) ++K;
		const float F = FMath::Clamp(FMath::Loge(Lp[K] / Pn) / FMath::Loge(Lp[K] / Lp[K + 1]), 0.f, 1.f);
		const float Te = LT[K] + F * (LT[K + 1] - LT[K]);
		const float Qe = Lq[K] + F * (Lq[K + 1] - Lq[K]);
		const float Ze = LZ[K] + F * (LZ[K + 1] - LZ[K]);

		const float Diff = VirtualTemp(Tpn, Qpn) - VirtualTemp(Te + K0, Qe);
		const float DE = Rd * 0.5f * (Diff + DiffPrev) * FMath::Loge(Pc / Pn);

		if (!bLFC)
		{
			if (Pn < Plcl && Diff > 0.f) { bLFC = true; LFC = Ze; }
			else CIN += FMath::Min(DE, 0.f);
		}
		else
		{
			if (DE > 0.f) CAPE += DE;
			if (Diff > 0.f) EL = Ze;
		}
		if (Pc > 500.f && Pn <= 500.f) Tp500 = Tp + (Tpn - Tp) * (Pc - 500.f) / (Pc - Pn);

		DiffPrev = Diff;
		Tp = Tpn;
		Pc = Pn;
	}
	C.CAPE = bLFC ? CAPE : 0.f;
	C.CIN = FMath::Clamp(CIN, -600.f, 0.f);
	C.LFC = bLFC ? LFC : 0.f;
	C.EL = bLFC ? FMath::Max(EL, LFC) : 0.f;
	C.LI = C.T500 - (Tp500 - K0);

	// ---- Hodograph: helicity, shear, storm motion ---------------------------
	const float Z850 = LZ[1], Z500 = LZ[3];
	auto WindAt = [&](float Z, float& U, float& V)
	{
		if (Z <= Z850)
		{
			const float A = Z / FMath::Max(Z850, 1.f);
			U = Lerp(C.Us, C.U850, A); V = Lerp(C.Vs, C.V850, A);
		}
		else
		{
			const float A = FMath::Min((Z - Z850) / FMath::Max(Z500 - Z850, 1.f), 1.15f);
			U = Lerp(C.U850, C.U500, A); V = Lerp(C.V850, C.V500, A);
		}
	};
	float MeanU = 0.f, MeanV = 0.f;
	for (int32 S = 0; S <= 12; ++S)
	{
		float U, V; WindAt(S * 500.f, U, V);
		MeanU += U; MeanV += V;
	}
	MeanU /= 13.f; MeanV /= 13.f;
	float U6, V6; WindAt(6000.f, U6, V6);
	const float ShU = U6 - C.Us, ShV = V6 - C.Vs;
	C.Shear06 = FMath::Sqrt(ShU * ShU + ShV * ShV);
	const float Hemi = Settings.LatitudeDeg < 0.f ? -1.f : 1.f;
	float Cx = MeanU, Cy = MeanV;
	if (C.Shear06 > 1.f)
	{
		// Bunkers: right mover in the northern hemisphere, left mover in the southern
		Cx += Hemi * 7.5f * ShV / C.Shear06;
		Cy -= Hemi * 7.5f * ShU / C.Shear06;
	}
	auto SRH = [&](float Top)
	{
		float Sum = 0.f, U0, V0;
		WindAt(0.f, U0, V0);
		const int32 Steps = FMath::Max(2, int32(Top / 250.f));
		for (int32 S = 1; S <= Steps; ++S)
		{
			float U1, V1; WindAt(Top * S / Steps, U1, V1);
			Sum += (U1 - Cx) * (V0 - Cy) - (U0 - Cx) * (V1 - Cy);
			U0 = U1; V0 = V1;
		}
		return Sum * Hemi; // positive = favourable in both hemispheres
	};
	C.SRH01 = SRH(1000.f);
	C.SRH03 = SRH(3000.f);
	C.StormU = Lerp(MeanU, Cx, FMath::Clamp(C.Meso * 2.f, 0.f, 1.f));
	C.StormV = Lerp(MeanV, Cy, FMath::Clamp(C.Meso * 2.f, 0.f, 1.f));

	// ---- Composite parameters (SPC definitions, simplified layers) ----------
	const float ShearTermSTP = C.Shear06 < 12.5f ? 0.f : FMath::Min(C.Shear06 / 20.f, 1.5f);
	const float LCLTerm = C.LCL < 1000.f ? 1.f : FMath::Clamp((2000.f - C.LCL) / 1000.f, 0.f, 1.f);
	const float CINTerm = C.CIN > -50.f ? 1.f : FMath::Clamp((200.f + C.CIN) / 150.f, 0.f, 1.f);
	C.STP = FMath::Max(0.f, (C.CAPE / 1500.f) * LCLTerm * (C.SRH01 / 150.f) * ShearTermSTP * CINTerm);

	const float ShearTermSCP = C.Shear06 < 10.f ? 0.f : FMath::Min(C.Shear06 / 20.f, 1.f);
	C.SCP = FMath::Max(0.f, (C.CAPE / 1000.f) * (C.SRH03 / 50.f) * ShearTermSCP);

	const float MR = FMath::Clamp(C.Q * 1000.f, 11.f, 13.6f);
	const float T5 = FMath::Min(C.T500, -5.5f);
	const float Sh = FMath::Clamp(C.Shear06, 7.f, 27.f);
	float Ship = (C.CAPE * MR * C.LapseRate75 * -T5 * Sh) / 42000000.f;
	if (C.CAPE < 1300.f) Ship *= C.CAPE / 1300.f;
	if (C.LapseRate75 < 5.8f) Ship *= C.LapseRate75 / 5.8f;
	if (C.FZL < 2400.f) Ship *= C.FZL / 2400.f;
	C.SHIP = FMath::Max(0.f, Ship);
}

float FAerosSimulation::MesoscaleLift(uint32 GI, uint32 GJ) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	// Unresolved mesoscale ascent (boundaries, terrain, gravity waves): smooth value noise,
	// ~25 cells wide, evolving over ~90 minutes. Deterministic -> identical on every client.
	constexpr float Scale = 1.f / 24.f;
	const float FX = GI * Scale, FY = GJ * Scale, FT = float(SimTime / 5400.0);
	const int32 X0 = FMath::FloorToInt(FX), Y0 = FMath::FloorToInt(FY), T0 = FMath::FloorToInt(FT);
	const float TX = SmoothStep01(FX - X0), TY = SmoothStep01(FY - Y0), TT = SmoothStep01(FT - T0);
	auto N = [this](int32 X, int32 Y, int32 T) { return Hash01(Settings.Seed ^ 0x77, uint32(X), uint32(Y), uint32(T)); };
	auto Plane = [&](int32 T)
	{
		return Lerp(Lerp(N(X0, Y0, T), N(X0 + 1, Y0, T), TX), Lerp(N(X0, Y0 + 1, T), N(X0 + 1, Y0 + 1, T), TX), TY);
	};
	const float V = Lerp(Plane(T0), Plane(T0 + 1), TT);
	return SmoothStep01((V - 0.35f) / 0.65f); // most areas ~0, a few zones of ascent
}

void FAerosSimulation::ColumnPhysics(int32 I, int32 J, FAerosCell& C, float Dt) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	const float Xm = (I + 0.5f) * DX, Ym = (J + 0.5f) * DX;
	float BgT, BgQ, BgT850, BgQ850, BgT500;
	BackgroundAt(Xm, Ym, BgT, BgQ, BgT850, BgQ850, BgT500);
	const float ElevPos = FMath::Max(C.Elev, 0.f);
	BgT -= 0.0065f * ElevPos; // standard lapse rate: mountains are colder (snow on peaks)
	const uint32 GI = uint32(I + CellOffsetX), GJ = uint32(J + CellOffsetY);
	const uint32 StepSalt = uint32(StepCount);
	const float Aridity = FMath::Max(Synoptic.Aridity, C.Arid);
	float MeltCm = 0.f; // snow melted this step (cm of snow ~ mm of water)

	C.CycloneZone = 0;
	if (Vortices.Num() > 0) ApplyCycloneForcing(I, J, C, Dt);

	C.Psfc = StdPressure + C.P + C.PForced - ElevPos * ElevPressure;
	const float Wind = FMath::Sqrt(FMath::Square(C.Us + C.Uo) + FMath::Square(C.Vs + C.Vo));
	const bool bWater = C.Water >= 0.5f;

	// ---- 1. Surface energy budget (sun -> ground -> air) ----------------------
	const float CC = C.CloudCover;
	float Albedo = bWater ? 0.07f : 0.20f;
	if (!bWater && C.Snow > 1.f) Albedo = Lerp(0.2f, 0.8f, FMath::Clamp(C.Snow / 10.f, 0.f, 1.f));
	Albedo = Lerp(Albedo, 0.55f, C.HailCov * 0.7f);
	const float SinElev = Settings.bEnableDiurnalCycle ? SinSunElev : 0.32f;
	const float Aerosol = FMath::Exp(-C.Dust / 30.f - C.Ash / 40.f); // dust storms and ash plumes dim the sun
	const float SW = Solar * SinElev * 0.75f * (1.f - Albedo) * (1.f - 0.72f * FMath::Pow(CC, 1.5f)) * Aerosol;
	const float TgK = C.Tg + K0;
	const float LWnet = Sigma * TgK * TgK * TgK * TgK * (0.24f - 0.17f * CC);
	const float Ch = 0.003f * (Wind + 2.f);
	const float Hs = RhoAir * Cp * Ch * (C.Tg - C.T);
	const float Avail = Lerp(FMath::Clamp(0.05f + 0.5f * C.Soil / SoilCapacity + 0.6f * C.Wet + (C.Snow > 0.5f ? 0.4f : 0.f), 0.f, 1.f), 1.f, C.Water);
	float Evap = RhoAir * Ch * (SatMixingRatio(C.Tg, C.Psfc) - C.Q) * Avail; // kg/m2/s
	if (Evap < 0.f) Evap *= 0.3f; // dew
	const float LE = Lv * Evap;
	C.SensibleFlux = Hs;

	if (bWater)
	{
		C.Tg += (SW - LWnet - Hs - LE) * Dt / WaterHeatCapacity;
		C.Tg += (Synoptic.SeaSurfaceTemperature - C.Tg) * Dt / (3.f * DaySeconds);
	}
	else
	{
		const float GroundFlux = 4.f * (C.Tg - BgT); // to deep soil
		C.Tg += (SW - LWnet - Hs - LE - GroundFlux) * Dt / LandHeatCapacity;
		if (C.Snow > 0.f && C.Tg > 0.f)
		{
			const float Melt = FMath::Min(C.Snow, C.Tg * 0.75f);
			C.Snow -= Melt;
			MeltCm += Melt;
			C.Tg -= Melt / 0.75f;
		}
	}
	C.Tg = FMath::Clamp(C.Tg, -60.f, 70.f);

	// ---- 2. Boundary layer --------------------------------------------------------
	const float HBL = FMath::Clamp(400.f + 6.f * FMath::Max(Hs, 0.f), 300.f, 2200.f);
	C.T += Hs / (RhoAir * Cp * HBL) * Dt;
	C.Q += Evap / (RhoAir * HBL) * Dt;

	// Large-scale relaxation toward the synoptic state
	C.T += (BgT - C.T) * Dt / DaySeconds;
	C.Q += (BgQ - C.Q) * Dt / DaySeconds;
	C.T850 += (BgT850 - C.T850) * Dt / (DaySeconds * 0.25f);
	C.Q850 += (BgQ850 - C.Q850) * Dt / (DaySeconds * 0.5f);
	C.T500 += (BgT500 - C.T500) * Dt / (DaySeconds * 0.25f);

	// Deep afternoon mixing: moisture up, cumulus fields
	if (HBL > 1200.f)
	{
		const float Mix = FMath::Clamp((HBL - 1200.f) / 1000.f, 0.f, 1.f) * Dt / 7200.f;
		const float DQ = (C.Q - C.Q850) * Mix;
		C.Q -= DQ * 0.4f;
		C.Q850 += DQ * 0.6f;
	}

	// Large-scale lift from low-level convergence (fronts, outflows, cyclones)
	// Hydrostatic pressure response: cold pools build meso-highs, hot ground builds thermal lows
	// (diagnostic, not dispersed by gravity waves: the cold air itself is the "mass")
	C.PForced = FMath::Clamp(-0.6f * C.ColdPool - 0.12f * (C.T - BgT), -8.f, 10.f);
	C.Psfc = StdPressure + C.P + C.PForced;

	// Orographic lift: wind blowing up a slope
	const float WOrog = (C.Us + C.Uo) * C.ElevDX + (C.Vs + C.Vo) * C.ElevDY;
	const float W850 = FMath::Clamp(-(0.5f * C.Div + 0.5f * C.Div850) * 1500.f + 0.5f * WOrog, -0.5f, 0.5f) + Synoptic.LargeScaleAscent;
	C.T850 -= 0.0098f * W850 * Dt * 0.5f;
	C.T500 -= 0.0098f * Synoptic.LargeScaleAscent * Dt * 0.4f;
	C.ColdPool *= FMath::Exp(-Dt / 5400.f);

	// ---- 3. Column diagnostics ------------------------------------------------------
	C.Q = FMath::Clamp(C.Q, 1e-5f, SatMixingRatio(C.T, C.Psfc));
	// Full parcel ascent on half the cells per step (checkerboard): halves the cost, 1 step latency.
	if (((I + J + int32(StepCount)) & 1) == 0 || C.Conv > 0.05f)
	{
		ColumnDiagnostics(C);
	}
	else
	{
		C.Td = FMath::Min(DewPoint(C.Q, C.Psfc), C.T);
		C.RH = RelativeHumidity(C.T, C.Q, C.Psfc);
		C.LCL = LCLHeight(C.T, C.Td);
	}

	// ---- 4. Convection ---------------------------------------------------------------
	float CondRate = 0.f;
	if (Settings.bEnableConvection)
	{
		const float WTherm = Hs > 0.f ? FMath::Pow(G / (C.T + K0) * Hs / (RhoAir * Cp) * HBL, 1.f / 3.f) : 0.f;
		const float WConv = FMath::Max(0.f, -C.Div) * 1000.f;
		const float WTrig = 0.5f * WTherm + WConv + FMath::Max(WOrog, 0.f) + 0.8f * MesoscaleLift(GI, GJ) + 0.4f * Hash01(Settings.Seed, GI, GJ, StepSalt);
		const float ETrig = 40.f * WTrig * WTrig;
		const bool bEnvOK = C.CAPE > 100.f && C.Q > 0.006f;
#ifdef AEROS_SIM_DEBUG
		AerosDbg(C.CAPE, C.CIN, ETrig, WTherm, WConv, bEnvOK);
#endif

		if (bEnvOK && C.Conv < 0.05f && -C.CIN < ETrig)
		{
			// Compensating subsidence: the more of the domain is convecting, the harder new cells start
			const float Suppress = FMath::Clamp(1.f - ConvectiveFraction / 0.12f, 0.f, 1.f);
			const float Chance = FMath::Clamp((ETrig + C.CIN) / FMath::Max(ETrig, 1.f), 0.f, 1.f) * Suppress * Dt / (6.f * 3600.f);
			// Decided per 3x3 block so a new storm starts ~6 km wide (resolvable), not as one pixel
			if (Hash01(Settings.Seed ^ 0x5bd1e995, GI / 3, GJ / 3, StepSalt) < Chance) C.Conv = 0.2f;
		}

		if (C.Conv > 0.f)
		{
			// Mature updrafts sustain themselves; weak (advected/diffused) values need local lift
			const bool bCanGrow = C.Conv >= 0.15f || -C.CIN < ETrig;
			if (C.CAPE > 100.f && bCanGrow)
				C.Conv += Dt / 900.f * C.Conv * (1.f - C.Conv) * FMath::Clamp(C.CAPE / 1000.f, 0.3f, 1.5f);

			float Decay = bCanGrow ? 1.f / 7200.f : 1.f / 900.f;
			if (C.CAPE < 100.f) Decay += 1.f / 900.f;
			if (C.Q < 0.008f) Decay += 1.f / 1800.f;
			Decay += (-C.ColdPool / 5.f) * (1.f - FMath::Clamp(C.Shear06 / 25.f, 0.f, 0.9f)) / 1800.f;
			Decay *= (1.f - 0.7f * C.Meso);
			C.Conv = FMath::Clamp(C.Conv - C.Conv * Decay * Dt, 0.f, 1.f);
			if (C.Conv < 0.005f) C.Conv = 0.f;
		}

		const float MesoTarget = (C.Conv > 0.3f && C.SRH03 > 100.f && C.Shear06 > 15.f && C.CAPE > 500.f)
			? FMath::Clamp(C.SCP / 6.f, 0.f, 1.f) : 0.f;
		C.Meso += (MesoTarget - C.Meso) * FMath::Min(Dt / 1200.f, 1.f);

		C.W = C.Conv > 0.02f ? FMath::Sqrt(2.f * C.CAPE) * (0.45f + 0.15f * C.Meso) * SmoothStep01(C.Conv / 0.6f) : 0.f;

		if (C.W > 0.f)
		{
			const float Tp500 = C.T500 - C.LI;
			const float QOut = SatMixingRatio(Tp500, 500.f);
			const float M = C.Conv * UpdraftArea * RhoAir * C.W; // kg/m2/s
			CondRate = M * FMath::Max(0.f, C.Q - QOut);
			C.Cloud += CondRate * Dt;
			C.Q -= M * FMath::Max(0.f, C.Q - C.Q850) * Dt / (RhoAir * 1500.f);
			const float Heat = Lv * CondRate * Dt / (Cp * UpperMass);
			C.T500 += Heat * 0.6f;
			C.T850 += Heat * 0.08f;
			C.Q850 += CondRate * Dt * 0.05f / Layer850Mass;
		}
	}
	else
	{
		C.Conv = 0.f; C.W = 0.f; C.Meso = 0.f;
	}

	// ---- 5. Stratiform condensation at 850 -------------------------------------------
	{
		const float Qs = SatMixingRatio(C.T850, 850.f);
		const float Tk = C.T850 + K0;
		if (C.Q850 > Qs)
		{
			const float Cn = (C.Q850 - Qs) / (1.f + Lv * Lv * Qs / (Rv * Cp * Tk * Tk));
			C.Q850 -= Cn;
			C.T850 += Lv / Cp * Cn;
			C.Cloud += Cn * Layer850Mass;
		}
		else if (C.Cloud > 0.f)
		{
			const float RH8 = C.Q850 / FMath::Max(Qs, 1e-6f);
			const float E = FMath::Min(C.Cloud, C.Cloud * (1.f - RH8) * Dt / 2400.f * (1.f - C.Conv));
			C.Cloud -= E;
			C.Q850 += E / Layer850Mass;
			C.T850 -= Lv / Cp * E / Layer850Mass;
		}
	}

	// ---- 6. Bulk microphysics: autoconversion, fall-out, evaporation, cold pool --------
	{
		const float Thr = 0.15f * (1.f - C.Conv);
		const float K = 1.f / 1500.f + C.Conv / 240.f;
		const float Auto = FMath::Min(FMath::Max(0.f, C.Cloud - Thr) * K * Dt, FMath::Max(0.f, C.Cloud - Thr));
		C.Cloud -= Auto;
		C.Rain += Auto;

		const float Tau = Lerp(800.f, 400.f, C.Conv);
		const float Fall = C.Rain * (1.f - FMath::Exp(-Dt / Tau)); // kg/m2 this step
		C.Rain -= Fall;
		// Sub-cloud evaporation + downdrafts entraining dry mid-level air build the cold pool
		const float RH850 = RelativeHumidity(C.T850, C.Q850, 850.f);
		const float EvapFrac = FMath::Clamp((1.f - C.RH) * FMath::Min(C.LCL, 3000.f) / 2000.f + 0.3f * (1.f - RH850) * C.Conv, 0.f, 0.7f);
		// Evaporation cannot exceed what the sub-cloud air can still hold (it saturates)
		const float Deficit = FMath::Max(0.f, SatMixingRatio(C.T, C.Psfc) - C.Q) * RhoAir * 1000.f; // kg/m2
		const float E = FMath::Min(Fall * EvapFrac, 0.5f * Deficit);
		const float Cool = Lv * E / (Cp * RhoAir * 1000.f);
		C.T -= Cool;
		C.ColdPool = FMath::Max(C.ColdPool - Cool, -12.f);
		C.Q += E / (RhoAir * 1000.f);
		C.PrecipRate = (Fall - E) / Dt * 3600.f;
		if (C.PrecipRate < 0.01f) C.PrecipRate = 0.f;
	}

	// ---- 7. Precipitation type, hail, reflectivity, lightning ------------------------
	C.Hail = 0.f;
	if (C.Conv > 0.2f && C.W > 12.f && C.FZL < C.EL - 1000.f)
	{
		const float DMax = MaxHailForUpdraft(C.W) * FMath::Clamp(C.Q * 1000.f / 13.f, 0.4f, 1.2f) * SmoothStep01(C.Conv);
		const float Melt = 3.f * C.FZL * 0.001f; // mm lost per km of fall through warm air
		const float D = DMax - Melt;
		if (D >= 5.f && C.PrecipRate > 0.5f) C.Hail = D;
	}

	EAerosPrecipType PT = EAerosPrecipType::None;
	if (C.PrecipRate > 0.f)
	{
		if (C.Hail >= 5.f) PT = EAerosPrecipType::Hail;
		else if (C.T850 > 1.f)
		{
			if (C.T < -4.f) PT = EAerosPrecipType::Sleet;
			else if (C.T < 0.5f) PT = EAerosPrecipType::FreezingRain;
			else PT = EAerosPrecipType::Rain;
		}
		else
		{
			if (C.T < 1.f) PT = EAerosPrecipType::Snow;
			else if (C.T < 3.f && C.FZL < 400.f) PT = EAerosPrecipType::Sleet;
			else PT = EAerosPrecipType::Rain;
		}
	}
	C.PType = uint8(PT);

	if (C.PrecipRate > 0.f)
	{
		C.DBZ = PT == EAerosPrecipType::Snow ? DBZFromSnow(C.PrecipRate) : DBZFromRain(C.PrecipRate);
		if (C.Hail > 0.f) C.DBZ = FMath::Max(C.DBZ, DBZFromHail(C.Hail));
	}
	else
	{
		C.DBZ = C.Cloud > 0.05f ? FMath::Max(-32.f, -20.f + 10.f * FMath::LogX(10.f, C.Cloud * 100.f)) : -32.f;
	}

	C.Lightning = (C.Conv > 0.25f && C.W > 8.f && C.EL > 6000.f)
		? FMath::Min(FMath::Max(FlashRate(C.EL * 0.001f), FlashRateUpdraft(C.W)) * SmoothStep01((C.Conv - 0.25f) / 0.5f), 150.f) : 0.f;

	// ---- 8. Ground accumulation ------------------------------------------------------
	if (Settings.bEnableSurfaceAccumulation)
	{
		const float RmmStep = C.PrecipRate * Dt / 3600.f;
		if (PT == EAerosPrecipType::Rain || PT == EAerosPrecipType::FreezingRain || PT == EAerosPrecipType::Hail)
		{
			C.Wet += RmmStep / 4.f;
			C.Snow = FMath::Max(0.f, C.Snow - RmmStep * 0.1f);
		}
		else if (PT == EAerosPrecipType::Snow || PT == EAerosPrecipType::Sleet)
		{
			C.Snow += RmmStep * (C.Tg <= 0.5f ? 1.f : 0.4f) * (PT == EAerosPrecipType::Sleet ? 0.5f : 1.f);
		}
		if (C.Hail > 0.f) C.HailCov += RmmStep / 2.f;

		const float Dry = (0.1f + FMath::Max(SW, 0.f) / 500.f + Wind / 15.f) * Dt / (3.f * 3600.f) * (C.T > 0.f ? 1.f : 0.3f);
		C.Wet = FMath::Clamp(bWater ? 1.f : C.Wet - Dry, 0.f, 1.f);
		C.HailCov = FMath::Clamp(C.HailCov - C.HailCov * Dt * (1.f + FMath::Max(C.T, 0.f) / 4.f) / 2400.f, 0.f, 1.f);
		if (C.T > 0.f)
		{
			const float Melt = FMath::Min(C.Snow, 0.3f * C.T * Dt / 3600.f);
			C.Snow -= Melt;
			MeltCm += Melt;
		}
		if (bWater) C.Snow = 0.f;
		if (C.Snow > 0.5f) C.Tg = FMath::Min(C.Tg, 0.f);
	}

	// ---- 8b. Hydrology: infiltration, runoff, ponding (lateral routing in StepHydrology) ---
	if (Settings.bEnableHydrology && !bWater)
	{
		const float LiquidFrac = PT == EAerosPrecipType::Snow ? 0.f : (PT == EAerosPrecipType::Sleet ? 0.5f : 1.f);
		const float RainMM = C.PrecipRate * Dt / 3600.f * LiquidFrac;
		// Saturation-excess runoff from wet areas (variable source area), the rest tries to infiltrate
		const float SatFrac = FMath::Pow(C.Soil / SoilCapacity, 4.f);
		const float DirectRunoff = (RainMM + MeltCm) * (0.03f + 0.9f * SatFrac);
		const float WaterIn = (RainMM + MeltCm - DirectRunoff) + C.Flood * 1000.f;
		// Infiltration: fast in dry soil, slow when saturated, very slow on frozen or crusted desert soil
		const float InfRate = (C.Tg < -1.f ? 0.5f : 2.f + 18.f * (1.f - C.Soil / SoilCapacity)) * (1.f - 0.8f * Aridity); // mm/h
		const float Inf = FMath::Clamp(InfRate * Dt / 3600.f, 0.f, FMath::Min(WaterIn, SoilCapacity - C.Soil));
		C.Soil += Inf;
		float Ponded = WaterIn - Inf + DirectRunoff;
		Ponded = FMath::Max(0.f, Ponded - (1.f + 6.f * SinElev) * Dt / DaySeconds * (C.T > 0.f ? 1.f : 0.2f)); // evaporation (~5 mm/day)
		C.Soil = FMath::Max(0.f, C.Soil - (0.5f + 4.f * SinElev) * Dt / DaySeconds * (C.Soil / SoilCapacity)); // evapotranspiration + drainage
		const float NewFlood = Ponded * 0.001f;
		C.FloodRise = (NewFlood - C.Flood) / Dt * 3600.f;
		C.Flood = NewFlood;
		C.Wet = FMath::Max(C.Wet, FMath::Clamp(C.Flood * 20.f + (C.Soil / SoilCapacity - 0.8f) * 2.f, 0.f, 1.f));
	}
	else if (bWater)
	{
		C.Flood = 0.f;
		C.Soil = SoilCapacity;
	}

	// ---- 8c. Mineral dust (sand storms, haboobs) ---------------------------------------
	if (Settings.bEnableDust)
	{
		if (!bWater && C.Snow < 0.5f && C.Flood < 0.01f && C.Tg > 0.f && Aridity > 0.05f)
		{
			// Saltation threshold rises with soil moisture; flux ~ (u - ut) u^2 above it (Marticorena-like).
			// Calibrated: 11 m/s over desert -> ~5 km visibility, a 25 m/s haboob front -> a few hundred metres.
			const float Threshold = 8.f + 10.f * C.Wet + 8.f * C.Soil / SoilCapacity;
			if (Wind > Threshold) C.Dust += 2e-6f * Aridity * Aridity * FMath::Square(Wind - Threshold) * Wind * Dt;
		}
		C.Dust *= FMath::Exp(-Dt / (3.f * 3600.f)) * FMath::Exp(-Dt * C.PrecipRate / 3600.f); // settling + wash-out
		C.Dust = FMath::Clamp(C.Dust, 0.f, 300.f);
	}

	// ---- 8d. Volcanic ash fall ------------------------------------------------------
	C.AshFall = 0.f;
	if (C.Ash > 0.001f)
	{
		const float Fall = C.Ash * (1.f - FMath::Exp(-Dt / 5400.f * (1.f + C.PrecipRate))); // g/m2, rain scavenges
		C.Ash -= Fall;
		if (!bWater) C.AshDepth += Fall * 0.001f; // 1 kg/m2 = 1 mm (bulk density ~1000 kg/m3)
		C.AshFall = Fall * 0.001f / Dt * 3600.f;
	}

	// ---- 8e. Blowing snow, comfort ----------------------------------------------------
	C.Blow = (C.Snow > 2.f && C.T < -1.f && C.Wet < 0.2f && PT != EAerosPrecipType::Rain) ? SmoothStep01((Wind - 9.f) / 8.f) : 0.f;
	if (C.Blow > 0.f) C.Snow = FMath::Max(0.f, C.Snow - C.Blow * 0.2f * Dt / 3600.f); // sublimation / drifting away
	{
		const float Kmh = Wind * 3.6f;
		if (C.T <= 10.f && Kmh > 4.8f)
		{
			const float V16 = FMath::Pow(Kmh, 0.16f);
			C.FeelsLike = 13.12f + 0.6215f * C.T - 11.37f * V16 + 0.3965f * C.T * V16; // wind chill (Environment Canada)
		}
		else if (C.T >= 27.f && C.RH > 0.4f)
		{
			const float F = C.T * 1.8f + 32.f, R = C.RH * 100.f; // NOAA Rothfusz heat index (F)
			const float HI = -42.379f + 2.04901523f * F + 10.14333127f * R - 0.22475541f * F * R - 6.83783e-3f * F * F
				- 5.481717e-2f * R * R + 1.22874e-3f * F * F * R + 8.5282e-4f * F * R * R - 1.99e-6f * F * F * R * R;
			C.FeelsLike = (HI - 32.f) / 1.8f;
		}
		else
		{
			C.FeelsLike = C.T;
		}
	}

	// ---- 9. Clouds, visibility, phenomenon -------------------------------------------
	const float RH850 = RelativeHumidity(C.T850, C.Q850, 850.f);
	float Cover = FMath::Max(SmoothStep01((RH850 - 0.65f) / 0.35f) * 0.9f, 1.f - FMath::Exp(-C.Cloud / 0.15f));
	if (HBL > 1000.f && C.LCL < HBL + 300.f && C.RH > 0.45f) Cover = FMath::Max(Cover, 0.3f); // fair-weather cumulus
	Cover = FMath::Max(Cover, SmoothStep01(C.Conv / 0.3f));
	C.CloudCover = FMath::Clamp(Cover, 0.f, 1.f);
	const bool bStratiform = RH850 > 0.9f && C.Conv < 0.2f;
	C.CloudBase = FMath::Clamp(bStratiform ? FMath::Min(C.LCL, 1200.f) : C.LCL, 100.f, 4000.f);
	C.CloudTop = C.Conv > 0.05f ? Lerp(C.CloudBase + 2000.f, FMath::Max(C.EL, C.CloudBase + 2000.f), SmoothStep01(C.Conv / 0.5f))
	                            : (bStratiform ? 4500.f : C.CloudBase + 800.f + 1200.f * C.CloudCover);

	float Vis = 30000.f;
	const float Spread = C.T - C.Td;
	if (Spread < 1.f && Wind < 4.f && SinSunElev < 0.25f) Vis = 150.f + 900.f * Spread;
	if (C.PrecipRate > 0.f)
	{
		Vis = FMath::Min(Vis, PT == EAerosPrecipType::Snow ? 6000.f / (1.f + C.PrecipRate * 3.f) : 20000.f / (1.f + C.PrecipRate * 0.5f));
		if (PT == EAerosPrecipType::Snow && Wind > 12.f) Vis *= 0.3f;
	}
	if (C.Dust > 0.05f) Vis = FMath::Min(Vis, 6500.f / C.Dust);            // extinction ~0.6 m2/g over a 1 km layer
	if (C.AshFall > 0.01f) Vis = FMath::Min(Vis, 3000.f / (1.f + C.AshFall * 20.f));
	if (C.Blow > 0.f) Vis = Lerp(Vis, 80.f, C.Blow);                       // whiteout
	C.Visibility = FMath::Max(Vis, 20.f);
	const float FlowSpeed = FMath::Sqrt(C.FlowU * C.FlowU + C.FlowV * C.FlowV);

	EAerosPhenomenon Ph;
	const bool bBlizzard = (C.Blow > 0.3f || (PT == EAerosPrecipType::Snow && C.PrecipRate > 0.1f)) && Wind >= 15.f && C.Visibility < 400.f;
	if (C.TsunamiZone && (C.Flood > 0.2f || bWater)) Ph = Vortices.Num() > 0 ? EAerosPhenomenon::StormSurge : EAerosPhenomenon::Tsunami;
	else if (C.CycloneZone == 2) Ph = EAerosPhenomenon::Hurricane;
	else if (C.CycloneZone == 1) Ph = EAerosPhenomenon::TropicalStorm;
	else if (C.Flood > 0.1f && (FlowSpeed > 0.5f || C.FloodRise > 0.05f)) Ph = EAerosPhenomenon::FlashFlood;
	else if (C.Meso > 0.5f && C.Conv > 0.4f) Ph = EAerosPhenomenon::Supercell;
	else if (C.Hail >= 20.f) Ph = EAerosPhenomenon::Hail;
	else if (C.Conv > 0.4f && (C.W > 25.f || C.Hail >= 25.f || Wind > 25.f)) Ph = EAerosPhenomenon::SevereThunderstorm;
	else if (C.Conv > 0.15f && C.Lightning > 0.2f) Ph = EAerosPhenomenon::Thunderstorm;
	else if (bBlizzard) Ph = EAerosPhenomenon::Blizzard;
	else if (C.Flood > 0.15f) Ph = EAerosPhenomenon::Flood; // cell average: low ground is much deeper
	else if (C.AshFall > 0.05f) Ph = EAerosPhenomenon::VolcanicAshfall;
	else if (C.Dust > 6.5f) Ph = EAerosPhenomenon::Sandstorm; // visibility < 1 km
	else if (PT == EAerosPrecipType::Snow && C.PrecipRate > 0.05f) Ph = EAerosPhenomenon::Snow;
	else if (PT == EAerosPrecipType::FreezingRain && C.PrecipRate > 0.05f) Ph = EAerosPhenomenon::FreezingRain;
	else if (C.PrecipRate > 8.f) Ph = EAerosPhenomenon::HeavyRain;
	else if (C.PrecipRate > 0.8f) Ph = EAerosPhenomenon::Rain;
	else if (C.PrecipRate > 0.05f) Ph = EAerosPhenomenon::Drizzle;
	else if (Vis < 1000.f) Ph = EAerosPhenomenon::Fog;
	else if (C.CloudCover > 0.85f) Ph = EAerosPhenomenon::Overcast;
	else if (C.CloudCover > 0.25f) Ph = EAerosPhenomenon::PartlyCloudy;
	else Ph = EAerosPhenomenon::Clear;
	C.Phen = uint8(Ph);

	// Sanity clamps
	C.T = FMath::Clamp(C.T, -60.f, 60.f);
	C.T850 = FMath::Clamp(C.T850, -60.f, 40.f);
	C.T500 = FMath::Clamp(C.T500, -70.f, 10.f);
	C.Q = FMath::Clamp(C.Q, 1e-5f, 0.035f);
	C.Q850 = FMath::Clamp(C.Q850, 1e-6f, 0.025f);
	C.Cloud = FMath::Clamp(C.Cloud, 0.f, 20.f);
	C.Rain = FMath::Clamp(C.Rain, 0.f, 600.f);
}

// =====================================================================
//  Tropical cyclones
// =====================================================================

void FAerosSimulation::ApplyCycloneForcing(int32 I, int32 J, FAerosCell& C, float Dt) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	const float Xm = (I + 0.5f) * DX, Ym = (J + 0.5f) * DX;
	const float Hemi = Coriolis < 0.f ? -1.f : 1.f;
	for (const FAerosVortex& V : Vortices)
	{
		const float Dx = Xm - V.X, Dy = Ym - V.Y;
		const float R = FMath::Max(FMath::Sqrt(Dx * Dx + Dy * Dy), 500.f);
		const float RR = R / V.Rmax;
		if (RR > 10.f) continue;

		const float X = FMath::Pow(V.Rmax / R, V.HollandB);
		const float Fh = FMath::Abs(Coriolis) * R * 0.5f;
		const float Vg = FMath::Sqrt(V.HollandB * V.DeltaP * 100.f / RhoAir * X * FMath::Exp(-X) + Fh * Fh) - Fh;
		const float Weight = RR < 4.f ? 1.f : FMath::Clamp(1.f - (RR - 4.f) / 6.f, 0.f, 1.f);
		const float Alpha = FMath::Min(Weight * Dt / 900.f, 1.f);

		const float TX = -Dy / R * Hemi, TY = Dx / R * Hemi;   // cyclonic tangent
		const float RX = Dx / R, RY = Dy / R;                  // outward radial
		const float Ci = 0.94f, Si = 0.34f;                    // 20 deg inflow at surface
		const float SurfU = Synoptic.WindSurface.X * 0.5f + V.VelX * 0.5f + 0.85f * Vg * (Ci * TX - Si * RX);
		const float SurfV = Synoptic.WindSurface.Y * 0.5f + V.VelY * 0.5f + 0.85f * Vg * (Ci * TY - Si * RY);
		C.Us += (SurfU - C.Us) * Alpha;
		C.Vs += (SurfV - C.Vs) * Alpha;
		C.U850 += (Synoptic.Wind850.X * 0.5f + Vg * (0.98f * TX - 0.17f * RX) - C.U850) * Alpha;
		C.V850 += (Synoptic.Wind850.Y * 0.5f + Vg * (0.98f * TY - 0.17f * RY) - C.V850) * Alpha;
		C.P += (-V.DeltaP * (1.f - FMath::Exp(-X)) - C.P) * Alpha;

		const float Intensity = FMath::Clamp(V.DeltaP / 30.f, 0.3f, 1.f);
		float ConvTarget = 0.f;
		if (RR < 0.5f)
		{
			C.Conv *= 1.f - Alpha;
			C.Cloud *= 1.f - Alpha * 0.5f;
		}
		else if (RR < 1.8f) ConvTarget = Intensity;
		else if (RR < 7.f)
		{
			const float Theta = FMath::Atan2(Dy, Dx);
			const float Arm = 0.5f + 0.5f * FMath::Cos(2.f * (Theta - Hemi * 1.6f * FMath::Loge(RR)));
			ConvTarget = 0.7f * Arm * Arm * Arm * (1.f - (RR - 1.8f) / 5.2f) * Intensity;
		}
		if (ConvTarget > C.Conv) C.Conv += (ConvTarget - C.Conv) * FMath::Min(Alpha * 2.f, 1.f);
		C.Q += (SatMixingRatio(C.T, C.Psfc) * 0.9f - C.Q) * Alpha * C.Water;

		const float SurfWind = 0.85f * Vg;
		if (Weight > 0.5f)
		{
			if (SurfWind >= 33.f) C.CycloneZone = 2;
			else if (SurfWind >= 17.f) C.CycloneZone = FMath::Max<uint8>(C.CycloneZone, 1);
		}
	}
}

void FAerosSimulation::StepCyclones(float Dt)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	const float Hemi = Coriolis < 0.f ? -1.f : 1.f;
	for (int32 K = Vortices.Num() - 1; K >= 0; --K)
	{
		FAerosVortex& V = Vortices[K];
		V.VelX = 0.5f * (Synoptic.Wind850.X + Synoptic.Wind500.X) - 1.0f;           // beta drift: west...
		V.VelY = 0.5f * (Synoptic.Wind850.Y + Synoptic.Wind500.Y) + 1.5f * Hemi;    // ...and poleward
		V.X += V.VelX * Dt;
		V.Y += V.VelY * Dt;
		V.Age += Dt;

		const int32 CI = FMath::Clamp(int32(V.X / DX), 0, NX - 1), CJ = FMath::Clamp(int32(V.Y / DX), 0, NY - 1);
		const float Water = Cells[CI + CJ * NX].Water;
		const float Shear = FVector2D::Distance(Synoptic.Wind500, Synoptic.WindSurface);
		if (Water > 0.5f)
		{
			const float DPMax = FMath::Clamp((Synoptic.SeaSurfaceTemperature - 26.f) * 18.f, 0.f, 110.f)
			                  * (1.f - FMath::Clamp((Shear - 5.f) / 20.f, 0.f, 1.f));
			V.DeltaP += (DPMax - V.DeltaP) * Dt / (36.f * 3600.f);
		}
		else
		{
			V.DeltaP *= FMath::Exp(-Dt / (10.f * 3600.f)); // landfall decay
			V.Rmax *= 1.f + Dt / (48.f * 3600.f) * 0.2f;
		}

		const float Margin = 2.f * V.Rmax;
		if (V.DeltaP < 3.f || V.X < -Margin || V.Y < -Margin || V.X > NX * DX + Margin || V.Y > NY * DX + Margin)
		{
			Vortices.RemoveAt(K);
		}
	}
}

int32 FAerosSimulation::SpawnCyclone(const FVector& World, float CentralPressure, float RmaxKm)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	FAerosVortex V;
	V.Id = NextVortexId++;
	WorldToDomain(World, V.X, V.Y);
	V.DeltaP = FMath::Clamp(StdPressure - CentralPressure, 3.f, 120.f);
	V.Rmax = FMath::Clamp(RmaxKm, 8.f, 150.f) * 1000.f;
	Vortices.Add(V);
	return V.Id;
}

void FAerosSimulation::TryCycloneGenesis()
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	if (FMath::Abs(Settings.LatitudeDeg) < 5.f || Vortices.Num() >= 3) return;
	if (Synoptic.SeaSurfaceTemperature < 26.5f) return;

	int32 Best = -1; float BestScore = 0.f;
	for (int32 J = 2; J < NY - 2; J += 3)
	{
		for (int32 I = 2; I < NX - 2; I += 3)
		{
			const FAerosCell& C = Cells[I + J * NX];
			if (C.Water < 0.8f || C.Shear06 > 12.f || C.Conv < 0.4f) continue;
			if (RelativeHumidity(C.T850, C.Q850, 850.f) < 0.7f) continue;
			if (C.Conv > BestScore) { BestScore = C.Conv; Best = I + J * NX; }
		}
	}
	if (Best < 0) return;
	if (Hash01(Settings.Seed, uint32(StepCount), 0xC1C10, Best) > 0.3f) return;

	FAerosVortex V;
	V.Id = NextVortexId++;
	V.X = (Best % NX + 0.5f) * DX;
	V.Y = (Best / NX + 0.5f) * DX;
	V.DeltaP = 8.f;
	V.Rmax = 50000.f;
	Vortices.Add(V);
}

// =====================================================================
//  Storm labelling + tracking
// =====================================================================

void FAerosSimulation::DetectAndTrackStorms(float Interval)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	TRACE_CPUPROFILER_EVENT_SCOPE(Aeros_StormTracking);
	const int32 N = NX * NY;
	Labels.SetNumUninitialized(N);
	for (int32 K = 0; K < N; ++K) Labels[K] = -1;

	struct FComp
	{
		float SumW = 0.f, SumX = 0.f, SumY = 0.f, SumSU = 0.f, SumSV = 0.f;
		int32 Count = 0, Start = 0;
		float MaxDBZ = -32.f, MaxW = 0.f, MaxHail = 0.f, MaxRain = 0.f, MaxTop = 0.f, MaxConv = 0.f;
		float MaxLtg = 0.f, SumLtg = 0.f, MaxSTP = 0.f, MaxSCP = 0.f, MaxMeso = 0.f;
		float CAPE = 0.f, SRH01 = 0.f, SRH03 = 0.f, Shear = 0.f, LCL = 0.f;
		int32 HailIdx = -1, WIdx = -1;
	};
	TArray<FComp> Comps;
	CompCells.Reset();
	TArray<int32> Stack;

	auto InMask = [this](int32 K) { const FAerosCell& C = Cells[K]; return C.Conv > 0.2f || C.DBZ >= 40.f; };

	for (int32 Seed = 0; Seed < N; ++Seed)
	{
		if (Labels[Seed] != -1 || !InMask(Seed)) continue;
		const int32 Label = Comps.Num();
		FComp& Comp = Comps.AddDefaulted_GetRef();
		Comp.Start = CompCells.Num();
		Stack.Reset();
		Stack.Add(Seed);
		Labels[Seed] = Label;
		while (Stack.Num() > 0)
		{
			const int32 K = Stack.Pop(EAllowShrinking::No);
			CompCells.Add(K);
			const FAerosCell& C = Cells[K];
			const int32 I = K % NX, J = K / NX;
			const float W = C.Conv + 0.01f;
			Comp.SumW += W; Comp.SumX += W * (I + 0.5f) * DX; Comp.SumY += W * (J + 0.5f) * DX;
			Comp.SumSU += W * C.StormU; Comp.SumSV += W * C.StormV;
			++Comp.Count;
			Comp.MaxDBZ = FMath::Max(Comp.MaxDBZ, C.DBZ);
			if (C.W > Comp.MaxW) { Comp.MaxW = C.W; Comp.WIdx = K; Comp.CAPE = C.CAPE; Comp.SRH01 = C.SRH01; Comp.SRH03 = C.SRH03; Comp.Shear = C.Shear06; Comp.LCL = C.LCL; }
			if (C.Hail > Comp.MaxHail) { Comp.MaxHail = C.Hail; Comp.HailIdx = K; }
			Comp.MaxRain = FMath::Max(Comp.MaxRain, C.PrecipRate);
			Comp.MaxConv = FMath::Max(Comp.MaxConv, C.Conv);
			if (C.Conv > 0.05f) Comp.MaxTop = FMath::Max(Comp.MaxTop, C.CloudTop);
			Comp.MaxLtg = FMath::Max(Comp.MaxLtg, C.Lightning);
			Comp.SumLtg += C.Lightning;
			Comp.MaxSTP = FMath::Max(Comp.MaxSTP, C.STP);
			Comp.MaxSCP = FMath::Max(Comp.MaxSCP, C.SCP);
			Comp.MaxMeso = FMath::Max(Comp.MaxMeso, C.Meso);

			const int32 Nb[4] = { I > 0 ? K - 1 : -1, I < NX - 1 ? K + 1 : -1, J > 0 ? K - NX : -1, J < NY - 1 ? K + NX : -1 };
			for (int32 B : Nb)
			{
				if (B >= 0 && Labels[B] == -1 && InMask(B)) { Labels[B] = Label; Stack.Add(B); }
			}
		}
	}

	for (FAerosTrackedStorm& S : Storms) S.bMatched = false;
	TArray<FAerosTrackedStorm> Next;
	CompOfStorm.Reset();

	// Biggest first so large systems keep their id
	TArray<int32> Order;
	for (int32 K = 0; K < Comps.Num(); ++K)
	{
		if (Comps[K].MaxConv >= 0.3f || Comps[K].MaxDBZ >= 35.f) Order.Add(K);
	}
	Order.Sort([&Comps](int32 A, int32 B) { return Comps[A].Count > Comps[B].Count; });

	for (int32 K : Order)
	{
		const FComp& Comp = Comps[K];
		const float X = Comp.SumX / Comp.SumW, Y = Comp.SumY / Comp.SumW;
		const float Radius = FMath::Sqrt(Comp.Count / PI) * DX;

		int32 BestIdx = -1; float BestD = TNumericLimits<float>::Max();
		for (int32 P = 0; P < Storms.Num(); ++P)
		{
			const FAerosTrackedStorm& S = Storms[P];
			if (S.bMatched) continue;
			const float PredX = S.X + float(S.Data.Velocity.X) * Interval, PredY = S.Y - float(S.Data.Velocity.Y) * Interval;
			const float D = FMath::Sqrt(FMath::Square(PredX - X) + FMath::Square(PredY - Y));
			const float Gate = S.Data.RadiusKm * 1000.f + Radius + 3.f * DX;
			if (D < Gate && D < BestD) { BestD = D; BestIdx = P; }
		}

		FAerosTrackedStorm T;
		float VX = Comp.SumSU / Comp.SumW, VY = Comp.SumSV / Comp.SumW;
		if (BestIdx >= 0)
		{
			FAerosTrackedStorm& Old = Storms[BestIdx];
			Old.bMatched = true;
			T.Data.Id = Old.Data.Id;
			T.Data.Age = Old.Data.Age + Interval;
			// Centroid jumps when cells merge/split: trust the track only within reason
			FVector2D Track((X - Old.X) / Interval, (Y - Old.Y) / Interval);
			if (Track.Size() > 25.0) Track = Track.GetSafeNormal() * 25.0;
			VX = 0.7f * VX + 0.3f * float(Track.X);
			VY = 0.7f * VY + 0.3f * float(Track.Y);
		}
		else
		{
			T.Data.Id = NextStormId++;
			T.Data.Age = 0.f;
		}
		T.X = X; T.Y = Y;
		FAerosStormData& D = T.Data;
		D.Location = DomainToWorld(X, Y);
		D.Velocity = FAerosSnapshot::SimToWorldDir(VX, VY);
		D.RadiusKm = Radius * 0.001f;
		D.NumCells = Comp.Count;
		D.MaxReflectivity = Comp.MaxDBZ;
		D.MaxUpdraft = Comp.MaxW;
		D.MaxHail = Comp.MaxHail;
		D.MaxRainRate = Comp.MaxRain;
		D.CloudTop = Comp.MaxTop;
		D.LightningRate = FMath::Min(Comp.MaxLtg + 0.2f * (Comp.SumLtg - Comp.MaxLtg), 200.f);
		D.CAPE = Comp.CAPE; D.SRH01 = Comp.SRH01; D.SRH03 = Comp.SRH03; D.BulkShear06 = Comp.Shear; D.LCL = Comp.LCL;
		D.STP = Comp.MaxSTP; D.SCP = Comp.MaxSCP; D.Mesocyclone = Comp.MaxMeso;
		D.Intensity = FMath::Clamp(FMath::Max((Comp.MaxDBZ - 30.f) / 35.f, Comp.MaxW / 50.f), 0.f, 1.f);
		D.Type = Comp.MaxMeso > 0.5f ? EAerosStormType::Supercell : (Comp.Count >= 8 ? EAerosStormType::Multicell : EAerosStormType::SingleCell);
		auto CellWorld = [this](int32 Idx) { return DomainToWorld((Idx % NX + 0.5f) * DX, (Idx / NX + 0.5f) * DX); };
		D.HailCoreLocation = Comp.HailIdx >= 0 ? CellWorld(Comp.HailIdx) : D.Location;
		D.UpdraftLocation = Comp.WIdx >= 0 ? CellWorld(Comp.WIdx) : D.Location;
		Next.Add(T);
		CompOfStorm.Add(FIntPoint(Comp.Start, Comp.Count));
	}
	Storms = MoveTemp(Next);
}

void FAerosSimulation::GenerateLightning(float Interval)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	// Drop strikes older than 2 minutes
	Lightning.RemoveAll([this](const FAerosLightningStrike& S) { return SimTime - S.SimTime > 120.0; });

	for (int32 K = 0; K < Storms.Num(); ++K)
	{
		const FAerosStormData& D = Storms[K].Data;
		if (D.LightningRate <= 0.f || !CompOfStorm.IsValidIndex(K)) continue;
		const FIntPoint Range = CompOfStorm[K];
		const float Lambda = D.LightningRate / 60.f * Interval;

		// Poisson sample (Knuth) with deterministic uniforms; a supercell at 120-200 / min needs 60-100 per
		// 30 s step (the old cap of 10 kept every storm at 20 / min)
		int32 Count = 0;
		float L = FMath::Exp(-FMath::Min(Lambda, 80.f)), Prod = 1.f;
		for (int32 It = 0; It < 128; ++It)
		{
			Prod *= Hash01(Settings.Seed, uint32(StepCount), uint32(D.Id), uint32(It));
			if (Prod <= L) break;
			++Count;
		}
		Count = FMath::Min(Count, 100);

		for (int32 S = 0; S < Count; ++S)
		{
			const float U1 = Hash01(Settings.Seed ^ 0xA5A5, uint32(StepCount), uint32(D.Id), uint32(S));
			const float U2 = Hash01(Settings.Seed ^ 0x3C3C, uint32(StepCount), uint32(D.Id), uint32(S));
			const float U3 = Hash01(Settings.Seed ^ 0x9696, uint32(StepCount), uint32(D.Id), uint32(S));
			int32 CellIdx = CompCells[Range.X + FMath::Min(int32(U1 * Range.Y), Range.Y - 1)];
			if (Cells[CellIdx].Conv < 0.3f) CellIdx = CompCells[Range.X]; // fall back to the seed (core)
			const float X = (CellIdx % NX + U2) * DX, Y = (CellIdx / NX + U3) * DX;

			FAerosLightningStrike Strike;
			Strike.Sequence = NextStrikeSeq++;
			Strike.Location = DomainToWorld(X, Y);
			// Spread over the coming interval, own uniform (U2 also places the strike). Dating them in the past
			// interval made half of them already late for the display clock (one step behind): they fired
			// in a burst with each new snapshot, together with the storm's jump.
			Strike.SimTime = float(SimTime + Interval * Hash01(Settings.Seed ^ 0x5A5A, uint32(StepCount), uint32(D.Id), uint32(S)));
			Strike.PeakCurrent = 12.f + 45.f * U3 * U3;
			Strike.bCloudToGround = U1 < 0.3f;
			Strike.StormId = D.Id;
			Lightning.Add(Strike);
		}
	}
	// Volcanic lightning: charge separation in the ash column, independent of CAPE
	for (const FAerosEmitter& E : Emitters)
	{
		const float Rate = 2.f + 25.f * E.Heat * FMath::Clamp(FMath::LogX(10.f, FMath::Max(E.MassRate, 1.f)) - 4.f, 0.f, 4.f) / 4.f; // per min
		const int32 Count = FMath::Min(int32(Rate / 60.f * Interval * (0.5f + Hash01(Settings.Seed, uint32(StepCount), uint32(E.Id), 0xA5))), 12);
		for (int32 K = 0; K < Count; ++K)
		{
			const float U1 = Hash01(Settings.Seed ^ 0x1111, uint32(StepCount), uint32(E.Id), uint32(K));
			const float U2 = Hash01(Settings.Seed ^ 0x2222, uint32(StepCount), uint32(E.Id), uint32(K));
			FAerosLightningStrike Strike;
			Strike.Sequence = NextStrikeSeq++;
			Strike.Location = DomainToWorld(E.X + (U1 - 0.5f) * 6000.f, E.Y + (U2 - 0.5f) * 6000.f);
			Strike.SimTime = float(SimTime - Interval * U1);
			Strike.PeakCurrent = 8.f + 20.f * U2;
			Strike.bCloudToGround = U2 < 0.25f;
			Strike.StormId = -100 - E.Id;
			Lightning.Add(Strike);
		}
	}
	// Room for one step of a whole outbreak (10 storms x 60-100 strikes)
	if (Lightning.Num() > 1536) Lightning.RemoveAt(0, Lightning.Num() - 1536);
}

// =====================================================================
//  Volcanic emitters
// =====================================================================

int32 FAerosSimulation::AddVolcanicEmitter(const FVector& World, float MassRate, float DurationSeconds)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	FAerosEmitter E;
	E.Id = NextEmitterId++;
	WorldToDomain(World, E.X, E.Y);
	E.MassRate = FMath::Max(MassRate, 1.f);
	E.Heat = FMath::Clamp((FMath::LogX(10.f, E.MassRate) - 3.f) / 5.f, 0.1f, 1.f);
	E.Remaining = FMath::Max(DurationSeconds, 60.f);
	Emitters.Add(E);
	return E.Id;
}

void FAerosSimulation::StopEmitter(int32 Id)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	Emitters.RemoveAll([Id](const FAerosEmitter& E) { return E.Id == Id; });
}

void FAerosSimulation::StepEmitters(float Dt)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	for (int32 K = Emitters.Num() - 1; K >= 0; --K)
	{
		FAerosEmitter& E = Emitters[K];
		const int32 CI = int32(E.X / DX), CJ = int32(E.Y / DX);
		if (CI >= 0 && CJ >= 0 && CI < NX && CJ < NY)
		{
			// ~10% of the erupted mass is fine ash that stays airborne, spread over 3x3 cells
			const float AshPerCell = E.MassRate * 0.1f * 1000.f * Dt / (9.f * DX * DX); // g/m2
			for (int32 J = FMath::Max(0, CJ - 1); J <= FMath::Min(NY - 1, CJ + 1); ++J)
			{
				for (int32 I = FMath::Max(0, CI - 1); I <= FMath::Min(NX - 1, CI + 1); ++I)
				{
					FAerosCell& C = Cells[I + J * NX];
					C.Ash += AshPerCell;
					if (I == CI && J == CJ)
					{
						// Buoyant eruption column: forces a (pyro)convective cell, with its lightning and rain
						C.Conv = FMath::Max(C.Conv, 0.7f * E.Heat);
						C.T += 1.5f * E.Heat * Dt / 600.f;
						C.Q = FMath::Min(C.Q + 0.002f * E.Heat * Dt / 600.f, 0.03f); // magmatic water vapour
					}
				}
			}
		}
		E.Remaining -= Dt;
		if (E.Remaining <= 0.f) Emitters.RemoveAt(K);
	}
}

// =====================================================================
//  Hydrology: surface runoff routing (Manning), floods
// =====================================================================

void FAerosSimulation::StepHydrology(float Dt)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	TRACE_CPUPROFILER_EVENT_SCOPE(Aeros_Hydrology);
	MaxFlood = 0.f;
	for (const FAerosCell& C : Cells) MaxFlood = FMath::Max(MaxFlood, C.Flood);
	if (MaxFlood < 1e-4f) return;
	if (FloodOut.Num() != NX * NY) FloodOut.SetNumZeroed(NX * NY);

	constexpr float InvManning = 1.f / 0.05f; // floodplain roughness
	const float InvDX = 1.f / DX;

	// Pass 1: each wet cell decides how much water leaves toward each neighbour
	ParallelFor(NY, [&](int32 J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			const int32 K = I + J * NX;
			const FAerosCell& C = Cells[K];
			FVector4f& Out = FloodOut[K];
			Out = FVector4f(0, 0, 0, 0);
			if (C.Water >= 0.5f || C.Flood < 1e-4f) continue;

			const float H = C.Elev + C.Flood;
			const int32 NI[4] = { I + 1, I - 1, I, I };
			const int32 NJ[4] = { J, J, J + 1, J - 1 };
			float Total = 0.f;
			float O[4];
			for (int32 D = 0; D < 4; ++D)
			{
				O[D] = 0.f;
				if (NI[D] < 0 || NJ[D] < 0 || NI[D] >= NX || NJ[D] >= NY) continue;
				const FAerosCell& N = Cells[NI[D] + NJ[D] * NX];
				const float HN = N.Water >= 0.5f ? N.Eta : N.Elev + N.Flood; // the sea is at 0 + anomaly
				const float Drop = H - HN;
				if (Drop <= 0.f) continue;
				const float Slope = Drop * InvDX;
				const float V = FMath::Min(InvManning * FMath::Pow(C.Flood, 2.f / 3.f) * FMath::Sqrt(Slope), 6.f);
				O[D] = V * C.Flood * Dt * InvDX;
				Total += O[D];
			}
			const float Limit = 0.5f * C.Flood;
			const float Scale = Total > Limit ? Limit / Total : 1.f;
			Out = FVector4f(O[0] * Scale, O[1] * Scale, O[2] * Scale, O[3] * Scale);
		}
	});

	// Pass 2: apply outflows and inflows
	ParallelFor(NY, [&](int32 J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			const int32 K = I + J * NX;
			FAerosCell& C = Cells[K];
			if (C.Water >= 0.5f) continue;
			const FVector4f& Mine = FloodOut[K];
			float In = 0.f;
			if (I + 1 < NX) In += FloodOut[K + 1].Y;   // east neighbour flowing west
			if (I - 1 >= 0) In += FloodOut[K - 1].X;   // west neighbour flowing east
			if (J + 1 < NY) In += FloodOut[K + NX].W;  // north neighbour flowing south
			if (J - 1 >= 0) In += FloodOut[K - NX].Z;  // south neighbour flowing north
			const float OutSum = Mine.X + Mine.Y + Mine.Z + Mine.W;
			const float Depth = FMath::Max(C.Flood, 1e-3f);
			C.FlowU = (Mine.X - Mine.Y) * DX / (Dt * Depth);
			C.FlowV = (Mine.Z - Mine.W) * DX / (Dt * Depth);
			C.Flood = FMath::Max(0.f, C.Flood - OutSum + In);
		}
	});

	// Water reaching the sea raises it only while the ocean solver runs (otherwise the sea is infinite)
	if (bOceanActive)
	{
		for (int32 J = 0; J < NY; ++J)
		{
			for (int32 I = 0; I < NX; ++I)
			{
				const int32 K = I + J * NX;
				FAerosCell& C = Cells[K];
				if (C.Water < 0.5f) continue;
				float In = 0.f;
				if (I + 1 < NX) In += FloodOut[K + 1].Y;
				if (I - 1 >= 0) In += FloodOut[K - 1].X;
				if (J + 1 < NY) In += FloodOut[K + NX].W;
				if (J - 1 >= 0) In += FloodOut[K - NX].Z;
				C.Eta += In;
			}
		}
	}
}

// =====================================================================
//  Ocean: tsunami propagation and storm surge (linear shallow water)
// =====================================================================

bool FAerosSimulation::TriggerTsunami(const FVector& World, float Magnitude)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	float Xm, Ym;
	WorldToDomain(World, Xm, Ym);
	// Sea-floor uplift and rupture size from moment magnitude (Wells & Coppersmith-like scaling)
	const float Amp = 0.5f * FMath::Pow(10.f, 0.5f * (Magnitude - 7.f));                  // m: M7 0.5, M8 1.6, M9 5
	const float Rkm = FMath::Clamp(0.5f * FMath::Pow(10.f, 0.5f * Magnitude - 1.8f), DX * 0.002f, NX * DX * 0.00025f);
	const float R = Rkm * 1000.f;
	bool bAny = false;
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			FAerosCell& C = Cells[I + J * NX];
			if (C.Water < 0.5f) continue;
			const float D2 = FMath::Square((I + 0.5f) * DX - Xm) + FMath::Square((J + 0.5f) * DX - Ym);
			const float G0 = FMath::Exp(-D2 / (R * R));
			if (G0 < 0.01f) continue;
			C.Eta += Amp * G0;
			bAny = true;
		}
	}
	if (bAny)
	{
		bOceanActive = true;
		OceanQuietTime = 0.f;
	}
	return bAny;
}

void FAerosSimulation::StepOcean(float Dt)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	TRACE_CPUPROFILER_EVENT_SCOPE(Aeros_Ocean);
	float HMax = 10.f;
	int32 NumWater = 0;
	for (const FAerosCell& C : Cells)
	{
		if (C.Water >= 0.5f) { HMax = FMath::Max(HMax, C.Bathy); ++NumWater; }
	}
	if (NumWater == 0) { bOceanActive = false; return; }
	bOceanActive = true;

	constexpr float Grav = 9.81f;
	const float MaxSub = 0.4f * DX / FMath::Sqrt(Grav * HMax);
	const int32 NumSub = FMath::Clamp(FMath::CeilToInt(Dt / MaxSub), 1, 200);
	const float H = Dt / NumSub;
	const float Inv2DX = 0.5f / DX;
	const int32 Sponge = 6;

	auto IsSea = [this](int32 I, int32 J) { return I >= 0 && J >= 0 && I < NX && J < NY && Cells[I + J * NX].Water >= 0.5f; };
	auto SpongeFactor = [&](int32 I, int32 J)
	{
		const int32 D = FMath::Min(FMath::Min(I, NX - 1 - I), FMath::Min(J, NY - 1 - J));
		return D >= Sponge ? 1.f : 1.f - 0.15f * float(Sponge - D) / Sponge; // absorbs waves leaving the domain
	};

	for (int32 Sub = 0; Sub < NumSub; ++Sub)
	{
		// Momentum: gravity (sea slope), inverse barometer + wind stress (storm surge), bottom friction
		ParallelFor(NY, [&](int32 J)
		{
			for (int32 I = 0; I < NX; ++I)
			{
				FAerosCell& C = Cells[I + J * NX];
				if (C.Water < 0.5f) continue;
				auto EtaAt = [&](int32 X, int32 Y) { return IsSea(X, Y) ? Cells[X + Y * NX].Eta : C.Eta; };
				auto IBAt = [&](int32 X, int32 Y)
				{
					const FAerosCell& N = (X >= 0 && Y >= 0 && X < NX && Y < NY) ? Cells[X + Y * NX] : C;
					return -0.01f * (N.P + N.PForced); // 1 hPa lower pressure = 1 cm higher sea
				};
				const float Depth = FMath::Max(C.Bathy + C.Eta, 1.f);
				const float DEdx = (EtaAt(I + 1, J) - EtaAt(I - 1, J) - (IBAt(I + 1, J) - IBAt(I - 1, J))) * Inv2DX;
				const float DEdy = (EtaAt(I, J + 1) - EtaAt(I, J - 1) - (IBAt(I, J + 1) - IBAt(I, J - 1))) * Inv2DX;
				const float WU = C.Us + C.Uo, WV = C.Vs + C.Vo;
				const float WS = FMath::Sqrt(WU * WU + WV * WV);
				C.MU += H * (-Grav * Depth * DEdx + 1.8e-6f * WS * WU);
				C.MV += H * (-Grav * Depth * DEdy + 1.8e-6f * WS * WV);
				const float MS = FMath::Sqrt(C.MU * C.MU + C.MV * C.MV);
				const float Fr = 1.f / (1.f + H * 0.0025f * MS / (Depth * Depth));
				const float Sp = SpongeFactor(I, J);
				C.MU *= Fr * Sp;
				C.MV *= Fr * Sp;
			}
		});
		// Continuity
		ParallelFor(NY, [&](int32 J)
		{
			for (int32 I = 0; I < NX; ++I)
			{
				FAerosCell& C = Cells[I + J * NX];
				if (C.Water < 0.5f) continue;
				auto MUAt = [&](int32 X, int32 Y) { return IsSea(X, Y) ? Cells[X + Y * NX].MU : 0.f; };
				auto MVAt = [&](int32 X, int32 Y) { return IsSea(X, Y) ? Cells[X + Y * NX].MV : 0.f; };
				const float Div = (MUAt(I + 1, J) - MUAt(I - 1, J) + MVAt(I, J + 1) - MVAt(I, J - 1)) * Inv2DX;
				C.Prev2 = C.Eta - H * Div; // staged: neighbours must read the old Eta
			}
		});
		ParallelFor(NY, [&](int32 J)
		{
			for (int32 I = 0; I < NX; ++I)
			{
				FAerosCell& C = Cells[I + J * NX];
				if (C.Water < 0.5f) continue;
				C.Eta = FMath::Clamp(C.Prev2, -30.f, 30.f) * SpongeFactor(I, J);
			}
		});
	}

	// Run-up: sea above the coastal land floods it (broad-crested weir), and drains back later through hydrology
	float MaxEta = 0.f;
	for (FAerosCell& C : Cells) C.TsunamiZone = 0;
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			FAerosCell& C = Cells[I + J * NX];
			if (C.Water < 0.5f) continue;
			MaxEta = FMath::Max(MaxEta, FMath::Abs(C.Eta));
			if (FMath::Abs(C.Eta) > 0.3f) C.TsunamiZone = 1;
			const int32 NI[4] = { I + 1, I - 1, I, I };
			const int32 NJ[4] = { J, J, J + 1, J - 1 };
			for (int32 D = 0; D < 4; ++D)
			{
				if (NI[D] < 0 || NJ[D] < 0 || NI[D] >= NX || NJ[D] >= NY) continue;
				FAerosCell& L = Cells[NI[D] + NJ[D] * NX];
				if (L.Water >= 0.5f) continue;
				// Coarse cells miss wave steepening on the beach: empirical run-up factor ~2 (field surveys)
				const float Head = 2.f * C.Eta - (FMath::Max(L.Elev, 0.f) + L.Flood);
				if (Head <= 0.05f) continue;
				const float Q = 1.7f * Head * FMath::Sqrt(Head);              // m2/s over the coast
				const float Move = FMath::Min(Q * Dt / DX, Head * 0.5f);      // m of water this step
				L.Flood += Move;
				L.TsunamiZone = 1;
				C.Eta -= Move * 0.25f;                                        // land cell is ~1/4 of the sea perimeter
			}
		}
	}

	OceanQuietTime = MaxEta < 0.02f ? OceanQuietTime + Dt : 0.f;
	if (Vortices.Num() == 0 && OceanQuietTime > 1800.f)
	{
		for (FAerosCell& C : Cells) { C.Eta = 0.f; C.MU = 0.f; C.MV = 0.f; }
		bOceanActive = false;
	}
}

// =====================================================================
//  Interventions / domain management
// =====================================================================

void FAerosSimulation::InjectThermal(const FVector& World, float RadiusKm, float DeltaT, float DeltaQ)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	float Xm, Ym;
	WorldToDomain(World, Xm, Ym);
	const float R = FMath::Max(RadiusKm * 1000.f, DX);
	const int32 Ext = FMath::CeilToInt(2.f * R / DX);
	const int32 CI = int32(Xm / DX), CJ = int32(Ym / DX);
	for (int32 J = FMath::Max(0, CJ - Ext); J <= FMath::Min(NY - 1, CJ + Ext); ++J)
	{
		for (int32 I = FMath::Max(0, CI - Ext); I <= FMath::Min(NX - 1, CI + Ext); ++I)
		{
			const float Dx = (I + 0.5f) * DX - Xm, Dy = (J + 0.5f) * DX - Ym;
			const float G0 = FMath::Exp(-(Dx * Dx + Dy * Dy) / (R * R));
			FAerosCell& C = Cells[I + J * NX];
			C.T += DeltaT * G0;
			C.T850 += DeltaT * 0.3f * G0;
			C.Q = FMath::Max(1e-5f, C.Q + DeltaQ * G0);
			C.Q850 = FMath::Max(1e-6f, C.Q850 + DeltaQ * 0.5f * G0);
			if (DeltaT > 0.f && G0 > 0.3f) C.Conv = FMath::Max(C.Conv, 0.25f * G0);
		}
	}
}

void FAerosSimulation::InjectRain(const FVector& World, float RadiusKm, float Millimetres)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	float Xm, Ym;
	WorldToDomain(World, Xm, Ym);
	const float R = FMath::Max(RadiusKm * 1000.f, DX);
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			const float D2 = FMath::Square((I + 0.5f) * DX - Xm) + FMath::Square((J + 0.5f) * DX - Ym);
			const float G0 = FMath::Exp(-D2 / (R * R));
			if (G0 < 0.01f) continue;
			FAerosCell& C = Cells[I + J * NX];
			C.Rain += Millimetres * G0;          // kg/m2 = mm: falls out over the next ~15 minutes
			C.Cloud = FMath::Max(C.Cloud, 1.5f * G0);
		}
	}
}

void FAerosSimulation::InjectOutflow(const FVector& World, float RadiusKm, float DeltaT)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	float Xm, Ym;
	WorldToDomain(World, Xm, Ym);
	const float R = FMath::Max(RadiusKm * 1000.f, DX);
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			const float D2 = FMath::Square((I + 0.5f) * DX - Xm) + FMath::Square((J + 0.5f) * DX - Ym);
			const float G0 = FMath::Exp(-D2 / (R * R));
			if (G0 < 0.01f) continue;
			FAerosCell& C = Cells[I + J * NX];
			C.ColdPool = FMath::Max(C.ColdPool + DeltaT * G0, -15.f);
			C.T += DeltaT * G0;
		}
	}
}

void FAerosSimulation::Recenter(const FVector& World)
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	float Xm, Ym;
	WorldToDomain(World, Xm, Ym);
	const int32 SX = FMath::RoundToInt((Xm - NX * DX * 0.5f) / DX);
	const int32 SY = FMath::RoundToInt((Ym - NY * DX * 0.5f) / DX);
	if (SX == 0 && SY == 0) return;

	Settings.DomainCenter.X += SX * DX * 100.0;
	Settings.DomainCenter.Y -= SY * DX * 100.0;
	CellOffsetX += SX;
	CellOffsetY += SY;

	Prev = Cells;
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			const int32 OI = I + SX, OJ = J + SY;
			FAerosCell& C = Cells[I + J * NX];
			if (OI >= 0 && OJ >= 0 && OI < NX && OJ < NY)
			{
				C = Prev[OI + OJ * NX];
			}
			else
			{
				C = MakeBackgroundCell((I + 0.5f) * DX, (J + 0.5f) * DX);
				ApplyStaticSurface(I, J, C);
				C.Tg = C.Water > 0.5f ? Synoptic.SeaSurfaceTemperature : C.T;
				ColumnDiagnostics(C);
				continue;
			}
			ApplyStaticSurface(I, J, C);
		}
	}
	for (FAerosVortex& V : Vortices) { V.X -= SX * DX; V.Y -= SY * DX; }
	for (FAerosEmitter& E : Emitters) { E.X -= SX * DX; E.Y -= SY * DX; }
	RecomputeSlopes();
	for (FAerosTrackedStorm& S : Storms) { S.X -= SX * DX; S.Y -= SY * DX; }
}

// =====================================================================
//  Snapshot
// =====================================================================

void FAerosSimulation::FillSnapshot(FAerosSnapshot& Out) const
{
	using namespace Aeros::Phys;
	using namespace AerosSimInternal;
	Out.Settings = Settings;
	Out.SimTime = SimTime;
	Out.Generation = uint64(StepCount);
	Out.NX = NX;
	Out.NY = NY;
	Out.CellM = DX;
	Out.DomainCenter = Settings.DomainCenter;
	Out.Clock = GetDateTime();
	Out.Synoptic = Synoptic;

	Out.Cells.SetNumUninitialized(NX * NY);
	for (int32 K = 0; K < NX * NY; ++K)
	{
		const FAerosCell& C = Cells[K];
		FAerosSnapCell& S = Out.Cells[K];
		S.T = C.T; S.Td = C.Td; S.RH = C.RH; S.P = C.Psfc; S.Tg = C.Tg;
		S.Us = C.Us + C.Uo; S.Vs = C.Vs + C.Vo; S.U850 = C.U850; S.V850 = C.V850;
		S.Visibility = C.Visibility;
		S.CloudCover = C.CloudCover; S.CloudBase = C.CloudBase; S.CloudTop = C.CloudTop; S.CloudWater = C.Cloud;
		S.PrecipRate = C.PrecipRate; S.Hail = C.Hail; S.DBZ = C.DBZ; S.Lightning = C.Lightning;
		S.Conv = C.Conv; S.W = C.W; S.Meso = C.Meso;
		S.CAPE = C.CAPE; S.CIN = C.CIN; S.LCL = C.LCL; S.LFC = C.LFC; S.EL = C.EL; S.FZL = C.FZL; S.LI = C.LI;
		S.SRH01 = C.SRH01; S.SRH03 = C.SRH03; S.Shear06 = C.Shear06; S.STP = C.STP; S.SCP = C.SCP; S.SHIP = C.SHIP;
		S.Wet = C.Wet; S.Snow = C.Snow; S.HailCover = C.HailCov;
		S.Soil = C.Soil / SoilCapacity; S.Elev = C.Elev;
		S.Flood = C.Flood; S.FlowU = C.FlowU; S.FlowV = C.FlowV; S.Eta = C.Eta;
		S.Dust = C.Dust; S.AshFall = C.AshFall; S.AshDepth = C.AshDepth; S.Blow = C.Blow; S.FeelsLike = C.FeelsLike;
		S.bWater = C.Water >= 0.5f ? 1 : 0;
		S.PType = C.PType; S.Phen = C.Phen;
	}

	Out.Storms.Reset(Storms.Num());
	for (const FAerosTrackedStorm& S : Storms) Out.Storms.Add(S.Data);

	Out.Cyclones.Reset(Vortices.Num());
	for (const FAerosVortex& V : Vortices)
	{
		FAerosCycloneData D;
		D.Id = V.Id;
		D.Location = DomainToWorld(V.X, V.Y);
		D.Velocity = FAerosSnapshot::SimToWorldDir(V.VelX, V.VelY);
		D.CentralPressure = StdPressure - V.DeltaP;
		D.MaxWind = V.MaxWind();
		D.RadiusMaxWindKm = V.Rmax * 0.001f;
		D.Category = SaffirSimpson(D.MaxWind);
		D.Age = V.Age;
		Out.Cyclones.Add(D);
	}

	Out.Lightning = Lightning;

	Out.Stats.SimTime = float(SimTime);
	Out.Stats.StepMs = LastStepMs;
	Out.Stats.StepsTotal = int32(StepCount);
	Out.Stats.NumCells = NX * NY;
	Out.Stats.NumStorms = Storms.Num();
	Out.Stats.NumCyclones = Vortices.Num();
}
