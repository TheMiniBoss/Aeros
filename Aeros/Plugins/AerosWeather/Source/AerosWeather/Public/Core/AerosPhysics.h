// Copyright Aeros. All Rights Reserved.
// Atmospheric physics helpers. Pure math, no UObject. Safe on any thread.

#pragma once

#include "CoreMinimal.h"

namespace Aeros::Phys
{
	constexpr float Rd        = 287.05f;    // J/(kg K)  dry air gas constant
	constexpr float Rv        = 461.5f;     // J/(kg K)  water vapour gas constant
	constexpr float Cp        = 1005.0f;    // J/(kg K)
	constexpr float Lv        = 2.501e6f;   // J/kg      latent heat vaporisation
	constexpr float Lf        = 3.337e5f;   // J/kg      latent heat fusion
	constexpr float G         = 9.80665f;   // m/s2
	constexpr float Omega     = 7.2921e-5f; // rad/s
	constexpr float Eps       = 0.622f;     // Rd/Rv
	constexpr float Kappa     = Rd / Cp;
	constexpr float K0        = 273.15f;    // 0 C in K
	constexpr float Sigma     = 5.670e-8f;  // Stefan-Boltzmann
	constexpr float Solar     = 1361.f;     // W/m2
	constexpr float RhoAir    = 1.15f;      // kg/m3 boundary-layer air
	constexpr float StdPressure = 1013.25f; // hPa

	/** Saturation vapour pressure over water (Bolton 1980). Tc in C, result in hPa. */
	FORCEINLINE float SatVaporPressure(float Tc)
	{
		return 6.112f * FMath::Exp(17.67f * Tc / (Tc + 243.5f));
	}

	/** Saturation mixing ratio (kg/kg). */
	FORCEINLINE float SatMixingRatio(float Tc, float P_hPa)
	{
		const float Es = SatVaporPressure(Tc);
		return Eps * Es / FMath::Max(P_hPa - Es, 1.f);
	}

	/** Vapour pressure (hPa) from mixing ratio. */
	FORCEINLINE float VaporPressure(float Q, float P_hPa)
	{
		return FMath::Max(Q, 1e-7f) * P_hPa / (Eps + FMath::Max(Q, 1e-7f));
	}

	/** Dew point (C) from mixing ratio and pressure. */
	FORCEINLINE float DewPoint(float Q, float P_hPa)
	{
		const float L = FMath::Loge(VaporPressure(Q, P_hPa) / 6.112f);
		return 243.5f * L / (17.67f - L);
	}

	/** Mixing ratio (kg/kg) from dew point. */
	FORCEINLINE float MixingRatioFromDewPoint(float TdC, float P_hPa)
	{
		return SatMixingRatio(TdC, P_hPa);
	}

	FORCEINLINE float RelativeHumidity(float Tc, float Q, float P_hPa)
	{
		return FMath::Clamp(Q / FMath::Max(SatMixingRatio(Tc, P_hPa), 1e-7f), 0.f, 1.f);
	}

	/** Lifting condensation level height above ground (Espy). */
	FORCEINLINE float LCLHeight(float Tc, float TdC)
	{
		return FMath::Max(0.f, 125.f * (Tc - TdC));
	}

	/** Pseudo-adiabatic dT/dp (K/hPa) for saturated parcel. */
	FORCEINLINE float MoistAdiabatDTdP(float Tk, float P_hPa)
	{
		const float Qs = SatMixingRatio(Tk - K0, P_hPa);
		const float Num = (Rd * Tk + Lv * Qs);
		const float Den = (Cp + (Lv * Lv * Qs * Eps) / (Rd * Tk * Tk));
		return Num / (Den * P_hPa);
	}

	FORCEINLINE float VirtualTemp(float Tk, float Q)
	{
		return Tk * (1.f + 0.61f * Q);
	}

	/** Coriolis parameter. */
	FORCEINLINE float CoriolisF(float LatitudeDeg)
	{
		return 2.f * Omega * FMath::Sin(FMath::DegreesToRadians(LatitudeDeg));
	}

	/**
	 * Hail terminal velocity, Vt = 9 * D^0.8 (D in cm) (Knight & Heymsfield like fit).
	 * Inverse gives the largest stone an updraft can keep aloft.
	 */
	FORCEINLINE float HailTerminalVelocity(float D_mm)
	{
		return 9.f * FMath::Pow(FMath::Max(D_mm, 0.f) * 0.1f, 0.8f);
	}

	FORCEINLINE float MaxHailForUpdraft(float W)
	{
		return W <= 0.f ? 0.f : 10.f * FMath::Pow(W / 9.f, 1.25f);
	}

	/** Reflectivity (dBZ) from rain rate (mm/h), Marshall-Palmer Z = 200 R^1.6. */
	FORCEINLINE float DBZFromRain(float R)
	{
		return R > 0.01f ? 10.f * FMath::LogX(10.f, 200.f * FMath::Pow(R, 1.6f)) : -32.f;
	}

	/** Snow Z = 2000 S^2 (S liquid equivalent mm/h). */
	FORCEINLINE float DBZFromSnow(float S)
	{
		return S > 0.01f ? 10.f * FMath::LogX(10.f, 2000.f * S * S) : -32.f;
	}

	/** Hail cores: ~50 dBZ at 10 mm, ~64 dBZ at 50 mm. */
	FORCEINLINE float DBZFromHail(float D_mm)
	{
		return D_mm > 1.f ? 50.f + 20.f * FMath::LogX(10.f, D_mm / 10.f) : -32.f;
	}

	/** Price & Rind lightning flash rate (flashes/min) from cloud top height (km). */
	FORCEINLINE float FlashRate(float CloudTop_km)
	{
		return 3.44e-5f * FMath::Pow(FMath::Max(CloudTop_km, 0.f), 4.9f);
	}

	/** Enhanced Fujita rating from 3 s gust (m/s). */
	FORCEINLINE int32 EFScale(float Wind)
	{
		if (Wind < 29.f) return 0;
		if (Wind < 38.f) return 1;
		if (Wind < 49.f) return 2;
		if (Wind < 60.f) return 3;
		if (Wind < 74.f) return 4;
		return 5;
	}

	/** Saffir-Simpson category from 1 min sustained wind (m/s). 0 = tropical storm or less. */
	FORCEINLINE int32 SaffirSimpson(float Wind)
	{
		if (Wind < 33.f) return 0;
		if (Wind < 43.f) return 1;
		if (Wind < 50.f) return 2;
		if (Wind < 58.f) return 3;
		if (Wind < 70.f) return 4;
		return 5;
	}

	/** Deterministic hash -> [0,1). Same on every machine: used for net-safe randomness. */
	FORCEINLINE uint32 Hash(uint32 A, uint32 B, uint32 C, uint32 D)
	{
		uint32 H = A * 0x9E3779B1u ^ (B + 0x7F4A7C15u) * 0x85EBCA77u;
		H ^= (C + 0x165667B1u) * 0xC2B2AE3Du;
		H ^= (D + 0x27D4EB2Fu) * 0x9E3779B1u;
		H ^= H >> 16; H *= 0x85EBCA6Bu;
		H ^= H >> 13; H *= 0xC2B2AE35u;
		H ^= H >> 16;
		return H;
	}

	FORCEINLINE float Hash01(uint32 A, uint32 B, uint32 C, uint32 D)
	{
		return float(Hash(A, B, C, D) >> 8) * (1.f / 16777216.f);
	}

	FORCEINLINE float SmoothStep01(float X)
	{
		X = FMath::Clamp(X, 0.f, 1.f);
		return X * X * (3.f - 2.f * X);
	}
}
