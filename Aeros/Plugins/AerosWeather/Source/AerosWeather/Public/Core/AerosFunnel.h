// Copyright Aeros. All Rights Reserved.
//
// Aeros::Funnel - pure C++ (no UObject): the condensation funnel of a tornado, from its vortex.
// Tangential wind v(r) = Vmax * 2 (r/rc) / (1 + (r/rc)^2); cyclostrophic balance gives the pressure
// deficit dp(r) = rho Vmax^2 / (1 + (r/rc)^2). Air at height h condenses where its pressure falls to the
// cloud-base pressure: K dp(r) >= rho g (base - h). So the funnel narrows downward, reaches lower with
// a stronger vortex or a lower base, and touches the ground only when K Vmax^2 >= g base.
// K >= 1 calibrates what the bare model leaves out (moist cooled inflow, stronger near-ground winds).
// The cloud shader (AerosCloudDensity.ush) mirrors these formulas exactly.

#pragma once

#include "CoreMinimal.h"
#include <cmath>

namespace Aeros
{
	namespace Funnel
	{
		constexpr float AirDensity = 1.1f;   // kg/m3
		constexpr float Gravity = 9.81f;     // m/s2

		struct FParams
		{
			float VmaxMs = 0.f;
			float CoreRadiusM = 80.f;
			float CloudBaseM = 1000.f;   // above the ground
			float K = 1.f;
		};

		inline float TangentialWindMs(float VmaxMs, float RadiusM, float CoreRadiusM)
		{
			const float X = FMath::Max(RadiusM, 0.f) / FMath::Max(CoreRadiusM, 1.f);
			return VmaxMs * 2.f * X / (1.f + X * X);
		}

		inline float PressureDeficitPa(float VmaxMs, float RadiusM, float CoreRadiusM)
		{
			const float X = FMath::Max(RadiusM, 0.f) / FMath::Max(CoreRadiusM, 1.f);
			return AirDensity * VmaxMs * VmaxMs / (1.f + X * X);
		}

		/** Funnel radius (m) at a height above the ground; 0 where the air does not condense. At or above
		 *  the base it returns the radius 1 m under the base (the funnel's top, where it meets the cloud). */
		inline float RadiusAtM(const FParams& P, float HeightM)
		{
			const float Below = FMath::Max(P.CloudBaseM - FMath::Max(HeightM, 0.f), 1.f);
			const float Ratio = FMath::Max(P.K, 0.f) * P.VmaxMs * P.VmaxMs / (Gravity * Below) - 1.f;
			return Ratio > 0.f ? FMath::Max(P.CoreRadiusM, 1.f) * std::sqrt(Ratio) : 0.f;
		}

		/** Height (m) of the funnel's tip above the ground (0 = touching). */
		inline float TipHeightM(const FParams& P)
		{
			const float Reach = FMath::Max(P.K, 0.f) * P.VmaxMs * P.VmaxMs / Gravity;
			return FMath::Max(FMath::Max(P.CloudBaseM, 0.f) - Reach, 0.f);
		}

		inline float SmoothStep01(float X)
		{
			X = FMath::Clamp(X, 0.f, 1.f);
			return X * X * (3.f - 2.f * X);
		}

		/** Life of a tornado, Age01 = 0 at birth, 1 at death. The funnel lowers from the wall cloud (TouchdownEnd),
		 *  then the wind climbs one EF category after the other up to the peak (RiseEnd, longer for a violent
		 *  tornado), holds it (MatureEnd), falls back the same way (DecayEnd) and dies in the rope-out. A future
		 *  EF5 is first an EF0, then EF1, EF2... it never appears at its peak. */
		constexpr float TouchdownEnd = 0.1f;
		constexpr float MatureEnd = 0.58f;
		constexpr float DecayEnd = 0.92f;

		/** Wind the tornado starts and ends with: weak EF0 (32 m/s), or its peak if it never gets stronger. */
		inline float StartWindMs(float PeakMs)
		{
			return FMath::Min(32.f, FMath::Max(PeakMs, 0.f));
		}

		/** End of the intensification: 12 % of the life for an EF0, ~42 % for an EF5 (~6 % per category). */
		inline float RiseEnd(float PeakMs)
		{
			return 0.12f + 0.3f * SmoothStep01((PeakMs - 32.f) / 60.f);
		}

		/** Peak ground wind (m/s) at a moment of the life (see TouchdownEnd). Linear between the stages so every
		 *  category lasts about as long as the others. */
		inline float LifeWindMs(float PeakMs, float Age01)
		{
			const float Start = StartWindMs(PeakMs);
			const float Peak = FMath::Max(PeakMs, Start);
			const float T = FMath::Clamp(Age01, 0.f, 1.f);
			const float Rise = RiseEnd(Peak);
			if (T < TouchdownEnd) return Start * SmoothStep01(T / TouchdownEnd);
			if (T < Rise) return FMath::Lerp(Start, Peak, (T - TouchdownEnd) / (Rise - TouchdownEnd));
			if (T < MatureEnd) return Peak;
			if (T < DecayEnd) return FMath::Lerp(Peak, Start, (T - MatureEnd) / (DecayEnd - MatureEnd));
			return Start * (1.f - SmoothStep01((T - DecayEnd) / (1.f - DecayEnd)));
		}

		/** Age (0-1) to jump to when the tornado loses its storm: the point of the weakening where the wind is
		 *  the wind now (never earlier than now): it weakens from where it is, category by category. */
		inline float DecayAgeForWind(float PeakMs, float WindMs, float Age01)
		{
			const float Start = StartWindMs(PeakMs);
			const float Peak = FMath::Max(PeakMs, Start);
			if (WindMs >= Start)
			{
				const float Frac = Peak > Start ? FMath::Clamp((Peak - WindMs) / (Peak - Start), 0.f, 1.f) : 1.f;
				return FMath::Max(Age01, MatureEnd + Frac * (DecayEnd - MatureEnd));
			}
			// Weaker than the start wind (still lowering, or already roping out): into the rope-out where the wind
			// has fallen as low (the wind decreases there: bisection)
			float Lo = DecayEnd, Hi = 1.f;
			for (int32 It = 0; It < 24; ++It)
			{
				const float Mid = 0.5f * (Lo + Hi);
				if (LifeWindMs(Peak, Mid) > WindMs) Lo = Mid; else Hi = Mid;
			}
			return FMath::Max(Age01, Hi);
		}

		/** 0 -> 1 over the last 35 % of the life (the rope-out stage). */
		inline float RopeOut(float Age01)
		{
			return SmoothStep01((Age01 - 0.65f) / 0.35f);
		}

		/** The core thins as the tornado ropes out. */
		inline float CoreRadiusAt(float CoreRadiusM, float Age01)
		{
			return CoreRadiusM * (1.f - 0.6f * RopeOut(Age01));
		}

		/** Core radius over the life: narrow while the tornado is weak, full at its peak, thinning in the rope-out. */
		inline float LifeCoreRadiusM(float CoreRadiusM, float PeakMs, float Age01)
		{
			const float Peak = FMath::Max(PeakMs, 1.f);
			const float Wind = FMath::Max(LifeWindMs(Peak, Age01), StartWindMs(Peak));
			return CoreRadiusAt(CoreRadiusM, Age01) * (0.45f + 0.55f * FMath::Clamp(Wind / Peak, 0.f, 1.f));
		}

		/** Peak wind of the funnel's vortex. At birth its circulation builds up (sqrt): the funnel lowers slowly
		 *  from the cloud. In the rope-out the circulation decays more slowly than the ground wind, and as the core
		 *  shrinks the wind in it speeds up (v = circulation / core): the rope reaches the ground until late,
		 *  then lifts. */
		inline float FunnelVmaxMs(float PeakMs, float Age01)
		{
			const float Start = StartWindMs(PeakMs);
			const float T = FMath::Clamp(Age01, 0.f, 1.f);
			if (T < TouchdownEnd) return Start * std::sqrt(SmoothStep01(T / TouchdownEnd));
			const float Wind = LifeWindMs(PeakMs, T);
			const float Circulation = T < DecayEnd ? Wind : Start * std::sqrt(FMath::Max(Wind / FMath::Max(Start, 1.f), 0.f));
			const float CoreRatio = FMath::Max(CoreRadiusAt(1.f, T), 0.1f);
			return FMath::Min(Circulation / CoreRatio, 2.f * FMath::Max(Wind, Circulation));
		}

		/** K for which this tornado's mature funnel (Vmax = MaxWind) reaches the ground, 1.3x over the
		 *  touch so its foot is ~0.55 core radii wide; never below KMin. The bare model leaves the moist,
		 *  cooled inflow out: with a fixed K of 2 an EF3 under a 1.5 km base hung 900 m above the ground,
		 *  while most photographed tornadoes show a condensation funnel down to the debris cloud. It still
		 *  hangs from the cloud while it spins up and lifts at the end of the rope-out. */
		inline float TouchdownK(float MaxWindMs, float CloudBaseM, float KMin, float Margin = 1.3f)
		{
			const float V = FMath::Max(MaxWindMs, 1.f);
			return FMath::Max(KMin, FMath::Max(Margin, 1.f) * Gravity * FMath::Max(CloudBaseM, 0.f) / (V * V));
		}

		/** Margin for TouchdownK: 1.3 (a cone, its foot ~0.55 core radii wide) up to 3.3 for violent
		 *  tornadoes (~85 m/s and more): a wedge ~1.5 core radii wide at the ground. */
		inline float WedgeMargin(float MaxWindMs)
		{
			return 1.3f + 2.f * SmoothStep01((MaxWindMs - 55.f) / 30.f);
		}

		/** Condensation K at a moment of the life: the funnel of the current wind reaches the ground (a cone while
		 *  weak, a wedge once violent), never computed below the start wind (the descent and the rope-out keep
		 *  the K of the start: they hang from the cloud). */
		inline float FunnelK(float PeakMs, float Age01, float CloudBaseM, float KMin)
		{
			const float Wind = FMath::Max(LifeWindMs(PeakMs, Age01), StartWindMs(PeakMs));
			return TouchdownK(Wind, CloudBaseM, KMin, WedgeMargin(Wind));
		}
	}
}
