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

		/** 0 -> 1 -> 0 over the life: spin-up over the first 12 %, rope-out from 75 % (AAerosTornado::GetStrength). */
		inline float LifeStrength(float Age01)
		{
			return SmoothStep01(Age01 / 0.12f) * (1.f - SmoothStep01((Age01 - 0.75f) / 0.25f));
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

		/** Peak wind of the funnel's vortex. Its circulation decays more slowly than the wind at the ground
		 *  (sqrt of the life strength), and as the core shrinks in the rope-out the wind in it speeds up
		 *  (v = circulation / core): the rope keeps reaching the ground until late, then lifts. */
		inline float FunnelVmaxMs(float MaxWindMs, float Age01)
		{
			const float Circulation = std::sqrt(FMath::Max(LifeStrength(Age01), 0.f));
			const float CoreRatio = FMath::Max(CoreRadiusAt(1.f, Age01), 0.1f);
			return FMath::Min(MaxWindMs * Circulation / CoreRatio, 2.f * MaxWindMs);
		}
	}
}
