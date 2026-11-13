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
