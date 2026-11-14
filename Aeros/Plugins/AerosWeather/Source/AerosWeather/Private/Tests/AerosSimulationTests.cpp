// Copyright Aeros. All Rights Reserved.
// Session Frontend > Automation > "Aeros" (or: UnrealEditor-Cmd ... -ExecCmds="Automation RunTests Aeros")

#include "Misc/AutomationTest.h"
#include "Core/AerosSimulation.h"
#include "Core/AerosSnapshot.h"
#include "Core/AerosPhysics.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAerosPhysicsTest, "Aeros.Physics.Thermodynamics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAerosPhysicsTest::RunTest(const FString& Parameters)
{
	using namespace Aeros::Phys;
	TestTrue(TEXT("Es(0C) ~ 6.11 hPa"), FMath::IsNearlyEqual(SatVaporPressure(0.f), 6.112f, 0.01f));
	TestTrue(TEXT("Es(30C) ~ 42.4 hPa"), FMath::IsNearlyEqual(SatVaporPressure(30.f), 42.4f, 0.5f));
	TestTrue(TEXT("Dew point round trip"), FMath::IsNearlyEqual(DewPoint(MixingRatioFromDewPoint(18.f, 1000.f), 1000.f), 18.f, 0.05f));
	TestTrue(TEXT("LCL Espy"), FMath::IsNearlyEqual(LCLHeight(30.f, 20.f), 1250.f, 1.f));
	TestTrue(TEXT("Hail 30 m/s updraft ~ 4.5 cm"), FMath::IsNearlyEqual(MaxHailForUpdraft(30.f), 45.f, 2.f));
	TestEqual(TEXT("EF3 at 55 m/s"), EFScale(55.f), 3);
	TestEqual(TEXT("Cat 4 at 65 m/s"), SaffirSimpson(65.f), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAerosSupercellTest, "Aeros.Simulation.SupercellOutbreak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAerosSupercellTest::RunTest(const FString& Parameters)
{
	FAerosSimulationSettings S;
	S.GridSizeX = S.GridSizeY = 96;
	S.Scenario = EAerosScenario::SupercellOutbreak;
	S.StartDayOfYear = 140;
	S.StartHour = 12.f;
	S.LatitudeDeg = 37.f;

	FAerosSimulation A, B;
	A.Initialize(S);
	B.Initialize(S);
	A.InjectThermal(FVector::ZeroVector, 6.f, 3.f, 0.002f);
	B.InjectThermal(FVector::ZeroVector, 6.f, 3.f, 0.002f);

	float MaxCAPE = 0.f, MaxW = 0.f, MaxDBZ = -32.f;
	for (int32 Step = 0; Step < 360; ++Step) // 3 hours
	{
		A.Step(30.f);
		B.Step(30.f);
	}
	bool bFinite = true, bSame = true;
	for (int32 K = 0; K < A.GetCells().Num(); ++K)
	{
		const FAerosCell& C = A.GetCells()[K];
		bFinite &= FMath::IsFinite(C.T) && FMath::IsFinite(C.Q) && FMath::IsFinite(C.P) && FMath::IsFinite(C.Us) && FMath::IsFinite(C.CAPE);
		bSame &= C.T == B.GetCells()[K].T && C.Conv == B.GetCells()[K].Conv;
		MaxCAPE = FMath::Max(MaxCAPE, C.CAPE);
		MaxW = FMath::Max(MaxW, C.W);
		MaxDBZ = FMath::Max(MaxDBZ, C.DBZ);
	}
	TestTrue(TEXT("All fields finite"), bFinite);
	TestTrue(TEXT("Deterministic (same seed -> same weather, needed for multiplayer)"), bSame);
	TestTrue(TEXT("Unstable environment (CAPE > 2000)"), MaxCAPE > 2000.f);
	TestTrue(TEXT("Deep convection developed (W > 20 m/s)"), MaxW > 20.f);
	TestTrue(TEXT("Radar echoes (> 45 dBZ)"), MaxDBZ > 45.f);
	TestTrue(TEXT("Storms tracked"), A.GetStorms().Num() > 0);

	FAerosSnapshot Snap;
	A.FillSnapshot(Snap);
	TestTrue(TEXT("Snapshot valid"), Snap.IsValid());
	TestTrue(TEXT("Snapshot sample valid"), Snap.Sample(FVector::ZeroVector).bValid);
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace AerosTests
{
	FAerosSimulationSettings Small(EAerosScenario Scenario)
	{
		FAerosSimulationSettings S;
		S.GridSizeX = S.GridSizeY = 64;
		S.Scenario = Scenario;
		S.StartDayOfYear = 180;
		S.StartHour = 11.f;
		return S;
	}
	void Flat(FAerosSimulation& Sim, float Height)
	{
		TArray<float> H;
		H.Init(Height, Sim.GetNX() * Sim.GetNY());
		Sim.SetElevation(H, Sim.GetSettings().DomainCenter);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAerosHazardTest, "Aeros.Simulation.Disasters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAerosHazardTest::RunTest(const FString& Parameters)
{
	using namespace AerosTests;
	// Flood: a 250 mm cloudburst leaves standing water
	{
		FAerosSimulation Sim;
		Sim.Initialize(Small(EAerosScenario::FairWeather));
		Flat(Sim, 10.f);
		Sim.InjectRain(FVector::ZeroVector, 10.f, 250.f);
		for (int32 K = 0; K < 60; ++K) Sim.Step(30.f);
		float MaxFlood = 0.f;
		for (const FAerosCell& C : Sim.GetCells()) MaxFlood = FMath::Max(MaxFlood, C.Flood);
		TestTrue(TEXT("Cloudburst floods (> 10 cm)"), MaxFlood > 0.1f);
	}
	// Tsunami: M9 offshore floods the coast
	{
		FAerosSimulationSettings S = Small(EAerosScenario::FairWeather);
		FAerosWaterRegion Ocean;
		Ocean.Center = FVector(6000000.0, 0, 0); // coast 20 km east
		Ocean.RadiusKm = 40.f;
		S.WaterRegions.Add(Ocean);
		FAerosSimulation Sim;
		Sim.Initialize(S);
		Flat(Sim, 2.f);
		TestTrue(TEXT("Tsunami triggered over the ocean"), Sim.TriggerTsunami(FVector(4500000.0, 0, 0), 9.f));
		float MaxRunup = 0.f;
		for (int32 K = 0; K < 60; ++K)
		{
			Sim.Step(30.f);
			for (const FAerosCell& C : Sim.GetCells()) if (C.Water < 0.5f) MaxRunup = FMath::Max(MaxRunup, C.Flood);
		}
		TestTrue(TEXT("Coast inundated (> 30 cm)"), MaxRunup > 0.3f);
	}
	// Dust: a gust front over the desert raises sand
	{
		FAerosSimulation Sim;
		Sim.Initialize(Small(EAerosScenario::DesertDust));
		Sim.InjectOutflow(FVector::ZeroVector, 15.f, -12.f);
		float MaxDust = 0.f;
		for (int32 K = 0; K < 60; ++K)
		{
			Sim.Step(30.f);
			for (const FAerosCell& C : Sim.GetCells()) MaxDust = FMath::Max(MaxDust, C.Dust);
		}
		TestTrue(TEXT("Haboob lifts dust (> 2 g/m2)"), MaxDust > 2.f);
	}
	// Volcano: ash falls downwind
	{
		FAerosSimulation Sim;
		Sim.Initialize(Small(EAerosScenario::FairWeather));
		Sim.AddVolcanicEmitter(FVector(-3000000.0, 0, 0), 1e6f, 7200.f);
		for (int32 K = 0; K < 120; ++K) Sim.Step(30.f);
		float MaxAsh = 0.f;
		for (const FAerosCell& C : Sim.GetCells()) MaxAsh = FMath::Max(MaxAsh, C.AshDepth);
		TestTrue(TEXT("Ash deposit (> 0.5 mm)"), MaxAsh > 0.5f);
	}
	// Blizzard: snowpack + gale = whiteout
	{
		FAerosSimulation Sim;
		Sim.Initialize(Small(EAerosScenario::Blizzard));
		for (int32 K = 0; K < 60; ++K) Sim.Step(30.f);
		int32 Blizzard = 0;
		for (const FAerosCell& C : Sim.GetCells()) Blizzard += C.Phen == uint8(EAerosPhenomenon::Blizzard) ? 1 : 0;
		TestTrue(TEXT("Blizzard conditions"), Blizzard > 0);
	}
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS
#include "Core/AerosCloudVolume.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAerosCloudVolumeTest, "Aeros.Clouds.VoxelAnatomy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAerosCloudVolumeTest::RunTest(const FString& Parameters)
{
	// --- A mature supercell: tower, anvil downwind, nothing above the EL, clear air upwind
	FAerosSimulationSettings S;
	S.GridSizeX = S.GridSizeY = 96;
	S.Scenario = EAerosScenario::SupercellOutbreak;
	S.StartDayOfYear = 140;
	S.StartHour = 12.f;
	S.LatitudeDeg = 37.f;
	FAerosSimulation Sim;
	Sim.Initialize(S);
	Sim.InjectThermal(FVector::ZeroVector, 6.f, 3.f, 0.002f);
	for (int32 Step = 0; Step < 360; ++Step) Sim.Step(30.f);
	FAerosSnapshot Snap;
	Sim.FillSnapshot(Snap);

	const FAerosStormData* Strongest = nullptr;
	for (const FAerosStormData& St : Snap.Storms)
		if (!Strongest || St.CloudTop > Strongest->CloudTop) Strongest = &St;
	TestTrue(TEXT("A storm exists"), Strongest != nullptr);
	if (!Strongest) return true;
	const FAerosStormData& St = *Strongest;

	// --- Lightning: the busiest storm flashes at its rate over the last 30 s step (a per-step cap of 10 kept
	// supercells at 20 flashes / min whatever their 60-120 / min rate)
	{
		const FAerosStormData* Busiest = nullptr;
		for (const FAerosStormData& B : Snap.Storms)
			if (!Busiest || B.LightningRate > Busiest->LightningRate) Busiest = &B;
		if (Busiest && Busiest->LightningRate >= 40.f)
		{
			// Strikes are generated once a minute and kept 2 minutes: at least one minute's worth is listed
			int32 Flashes = 0;
			for (const FAerosLightningStrike& L : Snap.Lightning)
				if (L.StormId == Busiest->Id) ++Flashes;
			TestTrue(TEXT("Busiest storm flashes at its rate (>= 60 % of one minute)"), Flashes >= int32(0.6f * Busiest->LightningRate));
		}
		else
		{
			TestTrue(TEXT("Outbreak has a storm with >= 40 flashes / min"), false);
		}
	}

	FAerosCloudVolumeDesc Desc;
	Desc.Center = FAerosCloudVolume::SnapCenter(St.UpdraftLocation);
	TArray<uint8> Grid;
	FAerosCloudVolume::Build(Snap, Desc, Grid);
	TestEqual(TEXT("Grid size RG8"), Grid.Num(), Desc.NX * Desc.NY * Desc.NZ * 2);

	const float ELkm = St.CloudTop * 0.001f, LCLkm = FMath::Max(St.LCL, 300.f) * 0.001f;
	const FVector Dir500 = FAerosSnapshot::SimToWorldDir(Snap.Synoptic.Wind500.X, Snap.Synoptic.Wind500.Y).GetSafeNormal2D();
	// Anvils leave along the 500 hPa wind turned by AnvilBend (sim frame counter-clockwise = world clockwise)
	float Bend0 = 0.f, BendTurn = 0.f;
	FAerosCloudVolume::AnvilBend(St.UpdraftLocation, Bend0, BendTurn);
	const double BendAng = FMath::Atan2(Dir500.Y, Dir500.X) - (Bend0 + 0.2f * BendTurn);
	const FVector Downwind = St.UpdraftLocation + FVector(FMath::Cos(BendAng), FMath::Sin(BendAng), 0.0) * 2500000.0;
	const FVector Upwind = St.UpdraftLocation - Dir500 * 3500000.0;
	TestTrue(TEXT("Tower is dense at mid height"), FAerosCloudVolume::SampleDensity(Grid, Desc, St.UpdraftLocation, 0.5f * (LCLkm + ELkm)) > 0.5f);
	TestTrue(TEXT("Nothing 2.5 km above the EL"), FAerosCloudVolume::SampleDensity(Grid, Desc, St.UpdraftLocation, ELkm + 2.5f) < 0.05f);
	TestTrue(TEXT("Anvil downwind near the EL"), FAerosCloudVolume::SampleDensity(Grid, Desc, Downwind, ELkm - 0.8f) > 0.2f);
	// Upwind checks on this storm alone: in an outbreak another storm upwind legitimately spreads its anvil there
	FAerosSnapshot Single = Snap;
	Single.Storms.Reset();
	Single.Storms.Add(St);
	TArray<uint8> SingleGrid;
	FAerosCloudVolume::Build(Single, Desc, SingleGrid);
	TestTrue(TEXT("No anvil far upwind"), FAerosCloudVolume::SampleDensity(SingleGrid, Desc, Upwind, ELkm - 0.8f) < 0.05f);
	TestTrue(TEXT("Clear below the cloud base away from storms"), FAerosCloudVolume::SampleDensity(SingleGrid, Desc, Upwind, 0.3f) < 0.05f);

	// --- Two overlapping anvils with different tops (11 and 13 km): the anvil top must stay continuous
	// across the overlap (no 2 km step where one anvil takes over from the other)
	{
		FAerosSnapshot Pair = Snap;
		Pair.Storms.Reset();
		const FVector Side(-Dir500.Y, Dir500.X, 0.0);
		FAerosStormData Low = St, High = St;
		Low.CloudTop = 11000.f;
		High.CloudTop = 13000.f;
		High.UpdraftLocation = High.UpdraftLocation + Side * 600000.0;
		if (!High.HailCoreLocation.IsZero()) High.HailCoreLocation = High.HailCoreLocation + Side * 600000.0;
		Pair.Storms.Add(Low);
		Pair.Storms.Add(High);
		TArray<uint8> PairGrid;
		FAerosCloudVolume::Build(Pair, Desc, PairGrid);
		auto AnvilTop = [&](const FVector& At)
		{
			for (float Alt = Desc.HeightKm - 0.5f * Desc.VoxelKmZ(); Alt > 6.f; Alt -= Desc.VoxelKmZ())
				if (FAerosCloudVolume::SampleDensity(PairGrid, Desc, At, Alt) > 0.1f) return Alt;
			return 0.f;
		};
		float MaxStep = 0.f, Prev = -1.f;
		for (double Off = -300000.0; Off <= 900000.0; Off += 30000.0)
		{
			const float Top = AnvilTop(St.UpdraftLocation + Dir500 * 2000000.0 + Side * Off);
			if (Top > 0.f && Prev > 0.f) MaxStep = FMath::Max(MaxStep, FMath::Abs(Top - Prev));
			Prev = Top;
		}
		TestTrue(TEXT("Overlapping anvils: top has no step (< 0.7 km between columns 300 m apart)"), MaxStep < 0.7f);
	}

	// --- Anvil bend: within +-10 deg / +-25 deg, smooth (two cells of one storm agree), varied (storms differ)
	{
		float A0 = 0.f, T0 = 0.f, A1 = 0.f, T1 = 0.f, MaxA = 0.f, MinA = 0.f;
		bool bInRange = true;
		for (int32 K = 0; K < 64; ++K)
		{
			const FVector P(K * 3700000.0, K * -2300000.0, 0.0);
			FAerosCloudVolume::AnvilBend(P, A0, T0);
			bInRange &= FMath::Abs(A0) <= FMath::DegreesToRadians(10.01f) && FMath::Abs(T0) <= FMath::DegreesToRadians(25.01f);
			MaxA = FMath::Max(MaxA, A0);
			MinA = FMath::Min(MinA, A0);
		}
		TestTrue(TEXT("Anvil bend within range"), bInRange);
		TestTrue(TEXT("Anvil bend varies between storms (> 8 deg spread)"), MaxA - MinA > FMath::DegreesToRadians(8.f));
		FAerosCloudVolume::AnvilBend(FVector(5000000.0, 3000000.0, 0.0), A0, T0);
		FAerosCloudVolume::AnvilBend(FVector(5200000.0, 3000000.0, 0.0), A1, T1);
		TestTrue(TEXT("Anvil bend smooth over 2 km (< 2 deg)"), FMath::Abs(A1 - A0) < FMath::DegreesToRadians(2.f));
	}

	// --- Storm outside the grid: nothing drawn, no wrap-around
	FAerosCloudVolumeDesc Far = Desc;
	Far.Center = FAerosCloudVolume::SnapCenter(St.UpdraftLocation + FVector(20000000.0, 0.0, 0.0));
	TArray<uint8> FarGrid;
	FAerosCloudVolume::Build(Snap, Far, FarGrid);
	uint8 FarMax = 0;
	for (int32 K = 0; K < FarGrid.Num(); K += 2) FarMax = FMath::Max(FarMax, FarGrid[K]);
	TestTrue(TEXT("Grid 200 km away from every storm stays empty"), FarMax < 13);

	// --- Fair weather: no storm, no stratiform deck -> empty grid (cumulus come from the shader)
	FAerosSimulation Fair;
	FAerosSimulationSettings FS = S;
	FS.Scenario = EAerosScenario::FairWeather;
	Fair.Initialize(FS);
	for (int32 Step = 0; Step < 120; ++Step) Fair.Step(30.f);
	FAerosSnapshot FairSnap;
	Fair.FillSnapshot(FairSnap);
	FAerosCloudVolumeDesc FairDesc;
	TArray<uint8> FairGrid;
	FAerosCloudVolume::Build(FairSnap, FairDesc, FairGrid);
	uint8 FairMax = 0;
	for (int32 K = 0; K < FairGrid.Num(); K += 2) FairMax = FMath::Max(FairMax, FairGrid[K]);
	TestTrue(TEXT("Fair weather grid is empty"), FairMax < 13);

	// --- Recentring snaps to 10 km
	TestTrue(TEXT("SnapCenter rounds to 10 km"), FAerosCloudVolume::SnapCenter(FVector(1234567.0, -987654.0, 0.0)).Equals(FVector(1000000.0, -1000000.0, 0.0)));
	return true;
}

#endif
