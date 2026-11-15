// Runs the engine automation tests of Private/Tests/AerosSimulationTests.cpp without Unreal.
// Same test code as in the editor (Session Frontend > Automation > Aeros), compiled against the shim.

#include <cstdio>
#include <string>

struct FAutoTestBase
{
	int Fails = 0;
	bool TestTrue(const char* Name, bool bValue)
	{
		std::printf("  [%s] %s\n", bValue ? "PASS" : "FAIL", Name);
		if (!bValue) ++Fails;
		return bValue;
	}
	template <class A, class B> bool TestEqual(const char* Name, A Actual, B Expected) { return TestTrue(Name, Actual == Expected); }
};

#define WITH_DEV_AUTOMATION_TESTS 1
#define TEXT(x) x
#define IMPLEMENT_SIMPLE_AUTOMATION_TEST(Cls, Name, Flags) struct Cls : FAutoTestBase { bool RunTest(const std::string& Parameters); };
#define FString std::string
namespace EAutomationTestFlags { enum { EditorContext = 1, EngineFilter = 2 }; }

#include "Tests/AerosSimulationTests.cpp"
#undef FString

// ---- Cloud noise (AerosCloudRender, pure C++): standalone only -------------------------------
#include "AerosCloudNoise.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace CloudNoiseTests
{
	int At(int Size, int X, int Y, int Z, int C) { return ((Z * Size + Y) * Size + X) * 4 + C; }

	// Seam difference (last slice vs first, wrapped) against the average neighbour difference.
	bool Tileable(const std::vector<uint8_t>& V, int Size, int C, int Axis)
	{
		double Seam = 0, Inner = 0; long SeamN = 0, InnerN = 0;
		for (int A = 0; A < Size; ++A) for (int B = 0; B < Size; ++B) for (int K = 0; K < Size; ++K)
		{
			int P[3] = { A, B, K };
			int Q[3] = { A, B, K };
			Q[Axis] = (P[Axis] + 1) % Size;
			const double D = std::abs(int(V[At(Size, P[0], P[1], P[2], C)]) - int(V[At(Size, Q[0], Q[1], Q[2], C)]));
			if (P[Axis] == Size - 1) { Seam += D; ++SeamN; } else { Inner += D; ++InnerN; }
		}
		return Seam / SeamN <= 1.25 * (Inner / InnerN);
	}

	void Percentiles(const std::vector<uint8_t>& V, int C, int& P5, int& P95)
	{
		long Hist[256] = {}; long N = 0;
		for (size_t I = C; I < V.size(); I += 4) { ++Hist[V[I]]; ++N; }
		long Acc = 0; P5 = -1; P95 = -1;
		for (int K = 0; K < 256; ++K)
		{
			Acc += Hist[K];
			if (P5 < 0 && Acc >= N * 5 / 100) P5 = K;
			if (P95 < 0 && Acc >= N * 95 / 100) P95 = K;
		}
	}

	double Correlation(const std::vector<uint8_t>& V, int A, int B)
	{
		double Sa = 0, Sb = 0, Saa = 0, Sbb = 0, Sab = 0; double N = double(V.size() / 4);
		for (size_t I = 0; I < V.size(); I += 4)
		{
			const double X = V[I + A], Y = V[I + B];
			Sa += X; Sb += Y; Saa += X * X; Sbb += Y * Y; Sab += X * Y;
		}
		const double Cov = Sab / N - (Sa / N) * (Sb / N);
		const double Va = Saa / N - (Sa / N) * (Sa / N), Vb = Sbb / N - (Sb / N) * (Sb / N);
		return Cov / std::sqrt(std::max(Va * Vb, 1e-9));
	}

	int Run()
	{
		FAutoTestBase T;
		const std::vector<uint8_t> Shape = AerosCloudNoise::MakeShape(128, 1);
		const std::vector<uint8_t> Detail = AerosCloudNoise::MakeDetail(32, 2);
		T.TestTrue("CloudNoise sizes", Shape.size() == size_t(128) * 128 * 128 * 4 && Detail.size() == size_t(32) * 32 * 32 * 4);

		bool bTile = true;
		for (int C = 0; C < 4; ++C) for (int Axis = 0; Axis < 3; ++Axis)
		{
			bTile &= Tileable(Shape, 128, C, Axis);
			bTile &= Tileable(Detail, 32, C, Axis);
		}
		T.TestTrue("CloudNoise_Tileable (seam <= 1.25 x neighbour diff, all channels and axes)", bTile);

		bool bRange = true;
		for (int C = 0; C < 4; ++C)
		{
			int P5, P95;
			Percentiles(Shape, C, P5, P95); bRange &= P5 <= 20 && P95 >= 235;
			Percentiles(Detail, C, P5, P95); bRange &= P5 <= 20 && P95 >= 235;
		}
		T.TestTrue("CloudNoise_Range (P5 <= 20, P95 >= 235)", bRange);

		double MaxCorr = 0;
		for (int A = 0; A < 4; ++A) for (int B = A + 1; B < 4; ++B) MaxCorr = std::max(MaxCorr, std::abs(Correlation(Shape, A, B)));
		std::printf("    shape max channel correlation %.3f\n", MaxCorr);
		T.TestTrue("CloudNoise_ChannelsDiffer (correlation < 0.9)", MaxCorr < 0.9);

		T.TestTrue("CloudNoise_Deterministic", AerosCloudNoise::MakeShape(128, 1) == Shape && AerosCloudNoise::MakeDetail(32, 2) == Detail);
		return T.Fails;
	}
}

// ---- Flash list (Core, pure C++): standalone only ----------------------------------------------
#include "Core/AerosFlashList.h"

namespace FlashListTests
{
	int Run()
	{
		FAutoTestBase T;
		{
			Aeros::TFlashList<8> L;
			for (int I = 0; I < 20; ++I) L.Add(0.05f * I, FVector(I, 0, 0));
			const bool bNum = L.Num() == 8;
			T.TestTrue("FlashList_KeepsBrightest: Num = 8", bNum);
			if (bNum)
			{
				T.TestTrue("FlashList_KeepsBrightest: first is 0.95", std::abs(L[0].Intensity - 0.95f) < 1e-5f && L[0].Location.X == 19.0);
				T.TestTrue("FlashList_KeepsBrightest: last is 0.6", std::abs(L[7].Intensity - 0.6f) < 1e-5f);
				bool bSorted = true;
				for (int I = 1; I < 8; ++I) bSorted &= L[I - 1].Intensity >= L[I].Intensity;
				T.TestTrue("FlashList_KeepsBrightest: sorted, brightest first", bSorted);
			}
		}
		{
			Aeros::TFlashList<8> L;
			L.Add(0.f, FVector(1, 2, 3));
			L.Add(-1.f, FVector(1, 2, 3));
			T.TestTrue("FlashList_IgnoresZero: empty", L.Num() == 0);
			L.Add(0.5f, FVector(1, 2, 3));
			L.Add(0.f, FVector(4, 5, 6));
			T.TestTrue("FlashList_IgnoresZero: one entry", L.Num() == 1 && L[0].Intensity == 0.5f);
			L.Reset();
			T.TestTrue("FlashList Reset", L.Num() == 0);
		}
		return T.Fails;
	}
}

// ---- Funnel model (Core, pure C++): standalone only --------------------------------------------
#include "Core/AerosFunnel.h"

namespace FunnelTests
{
	bool Near(float A, float B, float Eps) { return std::abs(A - B) <= Eps; }

	int Run()
	{
		using namespace Aeros::Funnel;
		FAutoTestBase T;
		{
			FParams P; P.VmaxMs = 0.f; P.CoreRadiusM = 100.f; P.CloudBaseM = 1000.f; P.K = 1.f;
			bool bNone = true;
			for (int H = 0; H <= 1000; H += 50) bNone &= RadiusAtM(P, float(H)) == 0.f;
			T.TestTrue("Funnel_NoVortexNoFunnel", bNone && TipHeightM(P) == 1000.f);
		}
		{
			FParams P; P.CoreRadiusM = 100.f; P.CloudBaseM = 800.f; P.K = 1.f;
			P.VmaxMs = 89.f; const float TipStrong = TipHeightM(P);
			P.VmaxMs = 85.f; const float TipWeak = TipHeightM(P);
			T.TestTrue("Funnel_TipOnGround (89 m/s touches, 85 m/s hangs)", TipStrong == 0.f && TipWeak > 0.f);
		}
		{
			auto R = [](float V, float Rc, float Base) { FParams P; P.VmaxMs = V; P.CoreRadiusM = Rc; P.CloudBaseM = Base; P.K = 3.f; return RadiusAtM(P, 200.f); };
			const bool bV = R(40, 100, 1000) < R(60, 100, 1000) && R(60, 100, 1000) < R(80, 100, 1000);
			const bool bRc = R(70, 50, 1000) < R(70, 100, 1000) && R(70, 100, 1000) < R(70, 200, 1000);
			const bool bBase = R(70, 100, 800) > R(70, 100, 1000) && R(70, 100, 1000) > R(70, 100, 1500);
			T.TestTrue("Funnel_RadiusMonotonic (Vmax up, core up, base down -> wider)", bV && bRc && bBase);
		}
		{
			FParams P; P.VmaxMs = 70.f; P.CoreRadiusM = 100.f; P.CloudBaseM = 1000.f; P.K = 1.5f;
			const float Tip = TipHeightM(P);
			T.TestTrue("Funnel_NarrowsDownward", RadiusAtM(P, 900.f) > RadiusAtM(P, 500.f) && RadiusAtM(P, 500.f) > RadiusAtM(P, Tip + 1.f)
				&& Near(RadiusAtM(P, Tip), 0.f, 0.5f) && Tip > 0.f && Tip < 1000.f);
		}
		{
			bool bOk = true;
			const float Bases[] = { 0.f, 1.f, 1000.f };
			const float Cores[] = { 1.f, 100.f, 2000.f };
			const float Vs[] = { 0.f, 50.f, 120.f };
			for (float Base : Bases) for (float Rc : Cores) for (float V : Vs)
			{
				FParams P; P.VmaxMs = V; P.CoreRadiusM = Rc; P.CloudBaseM = Base; P.K = 2.f;
				const float Hs[] = { 0.f, Base * 0.5f, Base, Base + 100.f };
				for (float H : Hs)
				{
					const float Rf = RadiusAtM(P, H);
					bOk &= std::isfinite(Rf) && Rf >= 0.f;
				}
				const float Tip = TipHeightM(P);
				bOk &= std::isfinite(Tip) && Tip >= 0.f;
				bOk &= std::isfinite(PressureDeficitPa(V, 0.f, Rc)) && std::isfinite(TangentialWindMs(V, 0.f, Rc));
			}
			T.TestTrue("Funnel_DegenerateInputs (finite, non-negative)", bOk);
		}
		{
			const bool bLife = Near(LifeStrength(0.f), 0.f, 1e-4f) && Near(LifeStrength(0.06f), 0.5f, 1e-3f) && Near(LifeStrength(0.5f), 1.f, 1e-4f)
				&& Near(LifeStrength(0.875f), 0.5f, 1e-3f) && Near(LifeStrength(1.f), 0.f, 1e-4f);
			T.TestTrue("Funnel_LifeMatchesTornado (0, 0.5, 1, 0.5, 0)", bLife);
		}
		{
			// The funnel's wind follows the circulation: as the core shrinks in the rope-out the wind in it
			// speeds up, so the rope still reaches the ground late in the life, then lifts at the very end.
			auto Tip = [](float Age01) { FParams P; P.VmaxMs = FunnelVmaxMs(70.f, Age01); P.CoreRadiusM = CoreRadiusAt(100.f, Age01); P.CloudBaseM = 800.f; P.K = 2.f; return TipHeightM(P); };
			T.TestTrue("Funnel_RopeTouchesLate (age 0.92 on the ground, 0.985 lifted)", Tip(0.92f) == 0.f && Tip(0.985f) > 200.f);
			T.TestTrue("Funnel_DescendsAtBirth (tip lower at 0.08 than at 0.03, on the ground at 0.5)", Tip(0.08f) < Tip(0.03f) && Tip(0.03f) > 0.f && Tip(0.5f) == 0.f);
			T.TestTrue("Funnel_VmaxNeverAboveTwiceMax", FunnelVmaxMs(70.f, 0.99f) <= 140.f && FunnelVmaxMs(70.f, 0.5f) <= 70.f + 1e-3f);
		}
		{
			T.TestTrue("Funnel wind peaks at the core", Near(TangentialWindMs(60.f, 100.f, 100.f), 60.f, 1e-3f) && TangentialWindMs(60.f, 50.f, 100.f) < 60.f && TangentialWindMs(60.f, 200.f, 100.f) < 60.f);
			T.TestTrue("Funnel pressure deficit 1.1 x V^2 at the centre", Near(PressureDeficitPa(100.f, 0.f, 100.f), 11000.f, 1.f));
		}
		return T.Fails;
	}
}

// ---- Storm chaser rules (AerosWeatherDemo, pure C++): standalone only -----------------------------
#include "AerosChaseRules.h"

namespace ChaseTests
{
	int Run()
	{
		using namespace AerosChase;
		FAutoTestBase T;
		auto Tor = [](int EF, float D, float Off = 0.f, float Vis = 30.f)
		{
			FPhotoInput I; I.Subject = ESubject::Tornado; I.EF = EF; I.DistanceKm = D; I.CenterOffset = Off; I.VisibilityKm = Vis;
			return PhotoPoints(I);
		};
		T.TestTrue("Chase_TornadoCloserScoresMore (1 km > 5 km > 15 km > 0)", Tor(2, 1.f) > Tor(2, 5.f) && Tor(2, 5.f) > Tor(2, 15.f) && Tor(2, 15.f) > 0);
		T.TestTrue("Chase_TornadoBeyond20kmScoresNothing", Tor(5, 21.f) == 0);
		T.TestTrue("Chase_StrongerTornadoScoresMore", Tor(4, 3.f) > Tor(1, 3.f));
		T.TestTrue("Chase_FramingMatters (centre > edge > off screen = 0)", Tor(2, 3.f, 0.1f) > Tor(2, 3.f, 0.8f) && Tor(2, 3.f, 1.1f) == 0);
		T.TestTrue("Chase_VisibilityMatters (rain at 1 km visibility halves or worse)", Tor(2, 3.f, 0.f, 1.f) * 2 <= Tor(2, 3.f, 0.f, 30.f));
		{
			FPhotoInput I; I.Subject = ESubject::Supercell; I.CenterOffset = 0.2f;
			I.DistanceKm = 5.f; const int Near = PhotoPoints(I);
			I.DistanceKm = 30.f; const int Mid = PhotoPoints(I);
			I.DistanceKm = 90.f; const int Far = PhotoPoints(I);
			T.TestTrue("Chase_SupercellNeedsDistance (5 km 0, 30 km > 0, 90 km 0)", Near == 0 && Mid > 0 && Far == 0);
		}
		{
			FPhotoInput I; I.Subject = ESubject::Lightning; I.DistanceKm = 5.f; I.Brightness = 1.f;
			const int Lit = PhotoPoints(I);
			I.Brightness = 0.f;
			T.TestTrue("Chase_LightningNeedsTheFlash", Lit > 0 && PhotoPoints(I) == 0);
		}
		T.TestTrue("Chase_NoDamageInCalmWeather", DamagePerSecond(10.f, 0.f, 0.f) == 0.f && DamagePerSecond(34.f, 19.f, 0.3f) == 0.f);
		T.TestTrue("Chase_DamageGrowsWithHazard", DamagePerSecond(60.f, 0.f, 0.f) > DamagePerSecond(45.f, 0.f, 0.f) && DamagePerSecond(0.f, 50.f, 0.f) > DamagePerSecond(0.f, 30.f, 0.f)
			&& DamagePerSecond(0.f, 0.f, 1.f) > 0.f);
		T.TestTrue("Chase_LightningDamage (0 beyond 300 m, most at the strike)", LightningDamage(301.f) == 0.f && LightningDamage(0.f) > LightningDamage(150.f) && LightningDamage(150.f) > 0.f);
		T.TestTrue("Chase_HailPointsGrowWithSize", HailPoints(50.f) > HailPoints(25.f) && HailPoints(25.f) > 0 && HailPoints(10.f) == 0);
		T.TestTrue("Chase_RespawnPenaltyIsAQuarter", RespawnPenalty(1000) == 250 && RespawnPenalty(0) == 0);
		T.TestTrue("Chase_RankGrowsWithScore", RankIndex(0) == 0 && RankIndex(100000) == 4 && RankIndex(800) >= RankIndex(300));
		return T.Fails;
	}
}

int main()
{
	int Fails = 0;
	{ FAerosPhysicsTest T;   std::printf("Physics\n");   T.RunTest(""); Fails += T.Fails; }
	{ FAerosSupercellTest T; std::printf("Supercell\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosHazardTest T;    std::printf("Disasters\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosCloudVolumeTest T; std::printf("Cloud voxels\n"); T.RunTest(""); Fails += T.Fails; }
	{ std::printf("Cloud noise\n"); Fails += CloudNoiseTests::Run(); }
	{ std::printf("Flash list\n"); Fails += FlashListTests::Run(); }
	{ std::printf("Funnel\n"); Fails += FunnelTests::Run(); }
	{ std::printf("Storm chaser\n"); Fails += ChaseTests::Run(); }
	std::printf("failures: %d\n", Fails);
	return Fails == 0 ? 0 : 1;
}
