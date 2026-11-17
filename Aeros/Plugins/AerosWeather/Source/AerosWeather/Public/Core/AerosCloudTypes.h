// Copyright Aeros. All Rights Reserved.
//
// Aeros::CloudTypes - pure C++ (no UObject): which clouds a column of the simulation carries, by layer, from
// its physics (WMO genera). Each genus forms only under its own conditions and at its own height:
//   low (base < 2 km)  Cu (convective boundary layer; humilis / mediocris / congestus by depth), St (stable,
//                      saturated surface layer with some wind), Sc (moist layer capped by an inversion, dry
//                      above), Ns (stratiform precipitation, deep and saturated);
//   middle (2-7 km)    As (large-scale ascent, moist mid levels: thick sheet), Ac (moderately moist mid levels,
//                      steeper lapse rate: thin layer of cells);
//   high (6-11 km)     Ci / Cs / Cc (upper troposphere moist w.r.t. ice; Cs under large-scale ascent, Cc in
//                      an unstable upper layer), base where the air reaches about -38 C.
// Heights are metres above the ground. Form: 0 = sheet (St, As, Cs) .. 1 = cells (Cu, Ac, Cc); Sc 0.5, Ci 0.5.

#pragma once

#include "CoreMinimal.h"
#include "AerosTypes.h"

namespace Aeros
{
	namespace CloudTypes
	{
		/** What a column knows (simulation cell diagnostics). */
		struct FColumn
		{
			float T = 15.f;               // C, near the ground
			float RH = 0.5f;              // 0-1, near the ground
			float T850 = 5.f;             // C (~1.5 km)
			float RH850 = 0.5f;           // 0-1
			float T500 = -20.f;           // C (~5.6 km)
			float LCL = 1000.f;           // m: lifting condensation level
			float BLTop = 600.f;          // m: depth of the mixed boundary layer
			float SurfaceHeatFlux = 0.f;  // W/m2, > 0 heats the air (sunny ground)
			float CAPE = 0.f;             // J/kg
			float Conv = 0.f;             // 0-1 convective activity (thunderstorm cells)
			float ConvTop = 0.f;          // m: top of the convective cell when Conv > 0.05
			float LapseRate75 = 6.5f;     // K/km, 700-500 hPa
			float Ascent = 0.f;           // m/s, large-scale vertical motion at ~850 hPa (fronts > 0)
			float Wind = 5.f;             // m/s near the ground
			float PrecipRate = 0.f;       // mm/h
			float UpperRH = 0.5f;         // relative humidity w.r.t. ice near 300 hPa
		};

		struct FLayer
		{
			float Cover = 0.f;   // 0-1
			float Base = 0.f;    // m above the ground
			float Top = 0.f;     // m above the ground
			float Form = 0.f;    // 0 sheet .. 1 cells
		};

		struct FLayers
		{
			FLayer Low, Mid, High;
		};

		using EGenus = EAerosCloudGenus;

		inline float Smooth01(float X)
		{
			X = FMath::Clamp(X, 0.f, 1.f);
			return X * X * (3.f - 2.f * X);
		}

		/** Lapse rate (K/km) between the ground and 850 hPa (~1.5 km): <= 0 an inversion, ~10 a convective layer. */
		inline float LowLapseRate(const FColumn& C)
		{
			return (C.T - C.T850) / 1.5f;
		}

		/** Height (m above the ground) where the air reaches about -38 C: ice clouds form above it. */
		inline float IceLevel(const FColumn& C)
		{
			return FMath::Clamp(5600.f + (C.T500 + 38.f) / 6.5f * 1000.f, 6000.f, 11000.f);
		}

		inline FLayers Diagnose(const FColumn& C)
		{
			FLayers Out;
			const float Lapse = LowLapseRate(C);
			const float Stable = Smooth01((4.f - Lapse) / 4.f);   // 1 under an inversion, 0 from 4 K/km up

			// ---- Low layer: the genus with the most cover wins its column; form, base and top blend by cover
			struct FCand { float Cover, Base, Top, Form; };
			FCand Cands[5] = {};
			int32 N = 0;
			// Cumulus: thermals of a sunny boundary layer reaching their condensation level
			{
				const float Convective = Smooth01((C.SurfaceHeatFlux - 10.f) / 60.f) * Smooth01((C.BLTop - C.LCL + 300.f) / 600.f) * Smooth01((C.RH - 0.35f) / 0.15f);
				const float Cover = Convective * FMath::Lerp(0.2f, 0.55f, Smooth01((C.RH - 0.45f) / 0.3f)) * (1.f - 0.7f * Stable);
				// Depth from the buoyancy: humilis (< 0.7 km) .. mediocris (0.7-2.5 km)
				const float Depth = FMath::Lerp(300.f, 2600.f, Smooth01(C.CAPE / 900.f));
				const float Base = FMath::Clamp(C.LCL, 300.f, 3000.f);
				Cands[N++] = { Cover, Base, Base + Depth, 1.f };
			}
			// Stratus: a saturated, stable layer near the ground with some wind (calm air gives fog instead)
			float StCover = 0.f;
			{
				StCover = Stable * Smooth01((C.RH - 0.9f) / 0.07f) * Smooth01((C.Wind - 1.f) / 2.f);
				const float Base = FMath::Clamp(C.LCL, 80.f, 400.f);
				Cands[N++] = { StCover, Base, Base + 300.f + 400.f * StCover, 0.f };
			}
			// Stratocumulus: a moist boundary layer under a lid (inversion), dry air above it
			{
				const float Lid = Smooth01((5.f - Lapse) / 4.f);
				// (a saturated layer at the ground, LCL < ~200 m, is stratus or fog, not stratocumulus)
				const float Moist = Smooth01((C.RH - 0.7f) / 0.2f) * Smooth01((1500.f - C.LCL) / 500.f) * Smooth01((C.LCL - 200.f) / 200.f);
				const float DryAbove = Smooth01((0.85f - C.RH850) / 0.3f);
				const float Cover = Lid * Moist * (0.4f + 0.6f * DryAbove) * (1.f - StCover);
				const float Base = FMath::Clamp(C.LCL, 300.f, 1500.f);
				Cands[N++] = { Cover, Base, FMath::Max(Base + 300.f, FMath::Min(Base + 800.f, 1900.f)), 0.5f };
			}
			// Nimbostratus: steady, non-convective precipitation from a deep saturated layer
			float NsCover = 0.f;
			{
				NsCover = Smooth01((C.PrecipRate - 0.3f) / 1.5f) * (1.f - Smooth01((C.Conv - 0.1f) / 0.2f)) * Smooth01((C.RH850 - 0.85f) / 0.1f);
				const float Base = FMath::Clamp(C.LCL, 300.f, 1500.f);
				Cands[N++] = { NsCover, Base, 4500.f + 2500.f * Smooth01(C.Ascent / 0.1f), 0.f };
			}
			// Convection (towering cumulus, cumulonimbus): its own cell top
			if (C.Conv > 0.05f)
			{
				const float Base = FMath::Clamp(C.LCL, 300.f, 3000.f);
				Cands[N++] = { Smooth01(C.Conv / 0.3f), Base, FMath::Max(C.ConvTop, Base + 2000.f), 1.f };
			}
			{
				float W = 0.f, Best = 0.f;
				FLayer& L = Out.Low;
				for (int32 K = 0; K < N; ++K)
				{
					const FCand& X = Cands[K];
					Best = FMath::Max(Best, X.Cover);
					const float Wt = X.Cover * X.Cover;   // the dominant genus sets the shape
					W += Wt; L.Base += Wt * X.Base; L.Top += Wt * X.Top; L.Form += Wt * X.Form;
				}
				L.Cover = Best;
				if (W > 0.f) { L.Base /= W; L.Top /= W; L.Form /= W; }
				else { L.Base = FMath::Clamp(C.LCL, 300.f, 3000.f); L.Top = L.Base + 500.f; L.Form = 1.f; }
			}

			// ---- Middle layer (2-7 km): humidity between 850 hPa and the upper troposphere
			{
				const float MidRH = 0.6f * C.RH850 + 0.4f * FMath::Min(C.UpperRH, 1.f);
				const float AsCover = Smooth01((MidRH - 0.7f) / 0.2f) * Smooth01((C.Ascent + 0.01f) / 0.05f) * (1.f - NsCover);
				const float AcCover = Smooth01((MidRH - 0.55f) / 0.2f) * (1.f - Smooth01((MidRH - 0.9f) / 0.08f)) * (0.4f + 0.6f * Smooth01((C.LapseRate75 - 6.f) / 1.5f)) * (1.f - AsCover) * (1.f - NsCover);
				FLayer& M = Out.Mid;
				if (AsCover >= AcCover)
				{
					M.Cover = AsCover;
					M.Base = FMath::Lerp(4000.f, 2500.f, Smooth01((MidRH - 0.8f) / 0.15f));   // lowers as it moistens
					M.Top = M.Base + FMath::Lerp(2000.f, 3500.f, Smooth01((MidRH - 0.8f) / 0.15f));
					M.Form = 0.f;
				}
				else
				{
					M.Cover = AcCover;
					M.Base = 3000.f + 1800.f * Smooth01((0.8f - MidRH) / 0.3f);
					M.Top = M.Base + FMath::Lerp(400.f, 800.f, Smooth01((C.LapseRate75 - 6.f) / 2.f));
					M.Form = 1.f;
				}
				M.Base = FMath::Max(M.Base, Out.Low.Cover > 0.2f ? Out.Low.Top + 300.f : 2000.f);
				M.Top = FMath::Max(M.Top, M.Base + 300.f);
				if (M.Base > 7000.f) M.Cover = 0.f;
			}

			// ---- High layer: ice clouds where the upper troposphere is moist w.r.t. ice
			{
				FLayer& H = Out.High;
				H.Cover = Smooth01((C.UpperRH - 0.65f) / 0.35f);
				const float Cs = Smooth01((C.Ascent - 0.005f) / 0.04f) * Smooth01((C.UpperRH - 0.85f) / 0.12f);
				const float Cc = Smooth01((C.LapseRate75 - 7.2f) / 0.8f) * (1.f - Cs);
				H.Form = 0.5f - 0.5f * Cs + 0.5f * Cc;
				H.Base = IceLevel(C);
				H.Top = H.Base + FMath::Lerp(1000.f, 1500.f, Cs) - 600.f * Cc;
			}
			return Out;
		}

		/** Low genus of a diagnosis (Cumulonimbus when the convective cell glaciates high up). */
		inline EGenus LowGenus(const FLayers& L, float Conv)
		{
			if (L.Low.Cover < 0.05f) return EGenus::None;
			if (Conv > 0.3f && L.Low.Top > 7000.f) return EGenus::Cumulonimbus;
			if (L.Low.Top - L.Low.Base > 3500.f && L.Low.Form < 0.3f) return EGenus::Nimbostratus;
			if (L.Low.Form > 0.75f) return EGenus::Cumulus;
			if (L.Low.Form > 0.3f) return EGenus::Stratocumulus;
			return EGenus::Stratus;
		}

		inline EGenus MidGenus(const FLayers& L)
		{
			if (L.Mid.Cover < 0.05f) return EGenus::None;
			return L.Mid.Form > 0.5f ? EGenus::Altocumulus : EGenus::Altostratus;
		}

		inline EGenus HighGenus(const FLayers& L)
		{
			if (L.High.Cover < 0.05f) return EGenus::None;
			if (L.High.Form < 0.25f) return EGenus::Cirrostratus;
			return L.High.Form > 0.75f ? EGenus::Cirrocumulus : EGenus::Cirrus;
		}

		/** Cumulus species by depth: 0 humilis (< 0.7 km), 1 mediocris (< 2.5 km), 2 congestus. */
		inline int32 CumulusSpecies(float DepthM)
		{
			return DepthM < 700.f ? 0 : (DepthM < 2500.f ? 1 : 2);
		}
	}
}
