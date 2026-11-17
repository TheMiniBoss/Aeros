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
			// The funnel of the tornado as it lives (FunnelVmaxMs, FunnelK, LifeCoreRadiusM): it hangs from the cloud at
			// birth, reaches the ground at every stage of its life (weak or violent, low or high base), and lifts
			// again at the very end of the rope-out
			bool bTouch = true, bSpinUp = true, bLift = true;
			const float Winds[] = { 30.f, 55.f, 95.f };
			const float Bases[] = { 300.f, 1500.f, 3000.f };
			auto Funnel = [](float W, float Base, float Age01)
			{
				FParams P; P.CoreRadiusM = LifeCoreRadiusM(100.f, W, Age01); P.CloudBaseM = Base;
				P.K = FunnelK(W, Age01, Base, 2.f); P.VmaxMs = FunnelVmaxMs(W, Age01);
				return P;
			};
			for (float W : Winds) for (float Base : Bases)
			{
				for (float Age : { 0.12f, 0.3f, 0.5f, 0.7f, 0.9f })
				{
					const FParams P = Funnel(W, Base, Age);
					bTouch &= TipHeightM(P) == 0.f && RadiusAtM(P, 0.f) > 0.2f * P.CoreRadiusM;
				}
				bSpinUp &= TipHeightM(Funnel(W, Base, 0.02f)) > 0.f;
				bLift &= TipHeightM(Funnel(W, Base, 0.995f)) > 0.f;
			}
			T.TestTrue("Funnel_TouchesGroundAllItsLife", bTouch);
			T.TestTrue("Funnel_SpinUpHangsFromCloud", bSpinUp);
			T.TestTrue("Funnel_RopeOutLiftsAtTheEnd", bLift);
			T.TestTrue("Funnel_TouchdownKKeepsLargerK", TouchdownK(90.f, 300.f, 5.f) == 5.f);
			{
				// A violent tornado is a wedge at its peak, but a cone while it is still EF0-EF1
				const FParams Peak = Funnel(95.f, 1200.f, 0.5f), Young = Funnel(95.f, 1200.f, 0.13f);
				const FParams Weak = Funnel(40.f, 1200.f, 0.5f);
				T.TestTrue("Funnel_ViolentIsAWedge (ground radius >= 1.4 core at the EF5 peak, < 0.8 for an EF1)",
					RadiusAtM(Peak, 0.f) >= 1.4f * Peak.CoreRadiusM && RadiusAtM(Weak, 0.f) < 0.8f * Weak.CoreRadiusM);
				T.TestTrue("Funnel_GrowsIntoAWedge (EF5 to be: narrower and a cone at its EF0 start)",
					RadiusAtM(Young, 0.f) < 0.8f * Young.CoreRadiusM && RadiusAtM(Young, 0.f) < 0.5f * RadiusAtM(Peak, 0.f));
			}
			{
				// Descends slowly: the tip is lower at 6 % of the life than at 3 %, still above the ground at 3 %
				const float Tip3 = TipHeightM(Funnel(55.f, 1000.f, 0.03f)), Tip6 = TipHeightM(Funnel(55.f, 1000.f, 0.06f));
				T.TestTrue("Funnel_DescendsAtBirth", Tip3 > 0.f && Tip6 < Tip3 && TipHeightM(Funnel(55.f, 1000.f, 0.1f)) == 0.f);
			}
		}
		{
			// EF goes up one category after the other, holds, then comes back down the same way
			auto EF = [](float W) { return Aeros::Phys::EFScale(W); };
			const float Peak = 95.f;
			int Prev = 0, MaxEF = 0, Steps = 0;
			bool bOneStep = true, bUpThenDown = true, bDown = false;
			float Spent[6] = {};
			for (int K = 0; K <= 1000; ++K)
			{
				const float Age = K / 1000.f;
				const float W = LifeWindMs(Peak, Age);
				const int E = W >= 29.f ? EF(W) : -1;
				if (K > 0 && E >= 0 && Prev >= 0)
				{
					bOneStep &= std::abs(E - Prev) <= 1;
					if (E < Prev) bDown = true;
					if (bDown && E > Prev) bUpThenDown = false;
					if (E != Prev) ++Steps;
				}
				if (E >= 0 && Age < 0.5f) Spent[E] += 0.001f;
				MaxEF = std::max(MaxEF, E);
				Prev = E;
			}
			T.TestTrue("Tornado_EFOneCategoryAtATime", bOneStep);
			T.TestTrue("Tornado_EFUpThenDown (EF0 -> EF5 -> EF0: 10 changes)", bUpThenDown && MaxEF == 5 && Steps == 10);
			bool bLasts = true;
			for (int E = 0; E < 5; ++E) bLasts &= Spent[E] >= 0.04f;
			T.TestTrue("Tornado_EachCategoryLasts (>= 4 % of the life on the way up)", bLasts);
			T.TestTrue("Tornado_StartsAndEndsWeak (EF0 at 12 % and 90 %, peak at 50 %)",
				EF(LifeWindMs(Peak, 0.12f)) == 0 && EF(LifeWindMs(Peak, 0.9f)) == 0 && Near(LifeWindMs(Peak, 0.5f), Peak, 1e-3f));
			T.TestTrue("Tornado_WeakOneStaysWeak (35 m/s never above EF0)", EF(LifeWindMs(35.f, 0.5f)) == 0 && LifeWindMs(35.f, 0.5f) <= 35.f);
			T.TestTrue("Tornado_NoWindAtBirthAndDeath", LifeWindMs(Peak, 0.f) == 0.f && LifeWindMs(Peak, 1.f) < 1e-3f);
			{
				// A tornado that loses its storm weakens from where it is: it never jumps to a stronger stage
				bool bNoJump = true;
				for (int K = 0; K <= 100; ++K)
				{
					const float Age = K / 100.f;
					const float Now = LifeWindMs(Peak, Age);
					const float Then = DecayAgeForWind(Peak, Now, Age);
					bNoJump &= Then >= Age && Then >= MatureEnd && LifeWindMs(Peak, Then) <= Now + 0.5f;
				}
				T.TestTrue("Tornado_OrphanWeakensFromWhereItIs", bNoJump && Near(DecayAgeForWind(Peak, 60.f, 0.3f), MatureEnd + (Peak - 60.f) / (Peak - 32.f) * (DecayEnd - MatureEnd), 1e-3f));
			}
			T.TestTrue("EF scale (real thresholds: 35 EF0, 45 EF1, 55 EF2, 70 EF3, 85 EF4, 95 EF5)",
				EF(35.f) == 0 && EF(45.f) == 1 && EF(55.f) == 2 && EF(70.f) == 3 && EF(85.f) == 4 && EF(95.f) == 5);
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
		T.TestTrue("Funnel_VmaxNeverAboveTwiceTheWind", FunnelVmaxMs(70.f, 0.99f) <= 140.f && FunnelVmaxMs(70.f, 0.5f) <= 70.f + 1e-3f);
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

// ---- Cloud genera (Core, pure C++): standalone only ----------------------------------------------
#include "Core/AerosCloudTypes.h"

namespace CloudTypeTests
{
	int Run()
	{
		using namespace Aeros::CloudTypes;
		FAutoTestBase T;
		auto Summer = []() { FColumn C; C.T = 28.f; C.RH = 0.5f; C.T850 = 14.f; C.RH850 = 0.4f; C.T500 = -12.f; C.LCL = 1400.f; C.BLTop = 1800.f; C.SurfaceHeatFlux = 250.f; C.CAPE = 300.f; C.Wind = 4.f; C.UpperRH = 0.4f; return C; };
		{
			const FColumn C = Summer();
			const FLayers L = Diagnose(C);
			T.TestTrue("Sunny afternoon: cumulus (fair-weather cover 15-50 %), nothing above", LowGenus(L, 0.f) == EGenus::Cumulus && L.Low.Cover > 0.15f && L.Low.Cover <= 0.5f
				&& MidGenus(L) == EGenus::None && HighGenus(L) == EGenus::None);
			T.TestTrue("Cumulus base at the condensation level, mediocris with some buoyancy", std::abs(L.Low.Base - 1400.f) < 1.f && CumulusSpecies(L.Low.Top - L.Low.Base) == 1);
			FColumn Weak = C; Weak.CAPE = 30.f;
			const FLayers LW = Diagnose(Weak);
			T.TestTrue("Little buoyancy: cumulus humilis", CumulusSpecies(LW.Low.Top - LW.Low.Base) == 0);
			FColumn Night = C; Night.SurfaceHeatFlux = -20.f; Night.T = 16.f;
			T.TestTrue("No sun, no fair-weather cumulus", Diagnose(Night).Low.Cover < 0.05f);
		}
		{
			FColumn C; C.T = 8.f; C.RH = 0.97f; C.T850 = 9.f; C.RH850 = 0.6f; C.LCL = 100.f; C.BLTop = 300.f; C.SurfaceHeatFlux = -30.f; C.Wind = 4.f; C.UpperRH = 0.3f;
			const FLayers L = Diagnose(C);
			T.TestTrue("Saturated stable layer with wind: stratus, low (base < 400 m)", LowGenus(L, 0.f) == EGenus::Stratus && L.Low.Cover > 0.5f && L.Low.Base < 400.f && L.Low.Top < 1200.f);
			C.Wind = 0.5f;
			T.TestTrue("Same air, calm: no stratus (fog instead)", Diagnose(C).Low.Cover < 0.1f);
		}
		{
			FColumn C; C.T = 10.f; C.RH = 0.85f; C.T850 = 9.f; C.RH850 = 0.35f; C.LCL = 600.f; C.BLTop = 700.f; C.SurfaceHeatFlux = 10.f; C.Wind = 6.f; C.UpperRH = 0.3f;
			const FLayers L = Diagnose(C);
			T.TestTrue("Moist layer under an inversion, dry above: stratocumulus below 2 km", LowGenus(L, 0.f) == EGenus::Stratocumulus && L.Low.Cover > 0.5f && L.Low.Top <= 2000.f);
		}
		{
			FColumn C; C.T = 12.f; C.RH = 0.9f; C.T850 = 8.f; C.RH850 = 0.95f; C.T500 = -18.f; C.LCL = 400.f; C.Ascent = 0.06f; C.UpperRH = 1.05f; C.Wind = 8.f;
			const FLayers L = Diagnose(C);
			T.TestTrue("Warm front: altostratus (thick sheet, 2-7 km) under cirrostratus", MidGenus(L) == EGenus::Altostratus && L.Mid.Cover > 0.5f
				&& L.Mid.Base >= 2000.f && L.Mid.Top - L.Mid.Base >= 2000.f && HighGenus(L) == EGenus::Cirrostratus);
			C.PrecipRate = 3.f;
			const FLayers R = Diagnose(C);
			T.TestTrue("Steady rain: nimbostratus, low base, deep (> 4 km)", LowGenus(R, 0.f) == EGenus::Nimbostratus && R.Low.Base < 2000.f && R.Low.Top > 4000.f && R.Mid.Cover < L.Mid.Cover);
		}
		{
			FColumn C; C.RH850 = 0.7f; C.UpperRH = 0.6f; C.LapseRate75 = 7.5f;
			const FLayers L = Diagnose(C);
			T.TestTrue("Moderately moist, steep lapse rate aloft: altocumulus (2-7 km, thin)", MidGenus(L) == EGenus::Altocumulus && L.Mid.Base >= 2000.f && L.Mid.Top <= 7000.f && L.Mid.Top - L.Mid.Base < 1000.f);
		}
		{
			FColumn C; C.UpperRH = 0.95f; C.RH850 = 0.3f;
			const FLayers L = Diagnose(C);
			T.TestTrue("Moist upper troposphere: cirrus, above 6 km", HighGenus(L) == EGenus::Cirrus && L.High.Base >= 6000.f);
			C.LapseRate75 = 8.f;
			T.TestTrue("Unstable upper layer: cirrocumulus", HighGenus(Diagnose(C)) == EGenus::Cirrocumulus);
			C.T500 = -30.f;
			FColumn Warm = C; Warm.T500 = -10.f;
			T.TestTrue("Ice level higher in warm air", Diagnose(Warm).High.Base > Diagnose(C).High.Base);
		}
		{
			FColumn C; C.T = 18.f; C.RH = 0.45f; C.T850 = 12.f; C.RH850 = 0.3f; C.UpperRH = 0.3f; C.SurfaceHeatFlux = -10.f;
			const FLayers L = Diagnose(C);
			T.TestTrue("Dry anticyclone at night: clear sky", L.Low.Cover < 0.05f && L.Mid.Cover < 0.05f && L.High.Cover < 0.05f);
		}
		{
			// Layers stay in their floors: low top < middle base < high base
			FColumn C = Summer(); C.RH850 = 0.75f; C.UpperRH = 0.95f; C.LapseRate75 = 7.f;
			const FLayers L = Diagnose(C);
			T.TestTrue("Layers ordered (low top < mid base < high base)", L.Low.Top < L.Mid.Base && L.Mid.Top <= 7000.f && L.Mid.Base < L.High.Base && L.Mid.Cover > 0.05f);
		}
		return T.Fails;
	}
}

int main()
{
	int Fails = 0;
	{ FAerosPhysicsTest T;   std::printf("Physics\n");   T.RunTest(""); Fails += T.Fails; }
	{ FAerosSupercellTest T; std::printf("Supercell\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosSingleStormTest T; std::printf("Single storm\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosStormTrackTest T; std::printf("Storm track\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosStormLifeTest T; std::printf("Storm life\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosHazardTest T;    std::printf("Disasters\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosCloudVolumeTest T; std::printf("Cloud voxels\n"); T.RunTest(""); Fails += T.Fails; }
	{ std::printf("Cloud noise\n"); Fails += CloudNoiseTests::Run(); }
	{ std::printf("Flash list\n"); Fails += FlashListTests::Run(); }
	{ std::printf("Funnel\n"); Fails += FunnelTests::Run(); }
	{ std::printf("Cloud types\n"); Fails += CloudTypeTests::Run(); }
	{ std::printf("Storm chaser\n"); Fails += ChaseTests::Run(); }
	std::printf("failures: %d\n", Fails);
	return Fails == 0 ? 0 : 1;
}
