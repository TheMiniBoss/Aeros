// Copyright Aeros. All Rights Reserved.
//
// Procedural demo terrain: a pure C++ height function (no UObject, no engine type), so it can be
// previewed outside Unreal (see Tests/Standalone/terrain_preview.cpp). Deterministic: same seed and
// same regions = same terrain on every machine.
//
// Units: metres, Unreal axes (X east, Y south). Heights are world Z / 100 (Z = 0 is the old flat ground,
// the sea is SeaLevelM below it).
//
// Layout around the demo start (origin):
//   * plains within ~5-15 km (tour cameras, crates, flood test),
//   * rolling hills and lakes (hollows below sea level) further out,
//   * mountain ranges (ridged noise, up to ~3 km, snow) from ~25 km, where a low-frequency mask allows them,
//   * the ocean regions (same circles as the simulation) with a shelf, beaches and bays,
//   * dunes in the desert regions,
//   * flat pads (volcano base) at their natural height.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace AerosDemoTerrainModel
{
	struct FCircle
	{
		double X = 0.0;        // m (Unreal X)
		double Y = 0.0;        // m (Unreal Y)
		double RadiusM = 0.0;
		double EdgeM = 1000.0; // blend width
		double Value = 0.0;    // pads: flat height (filled by FModel::Init)
	};

	struct FSurface
	{
		float HeightM = 0.f;  // world Z / 100
		float Desert = 0.f;   // 0-1: desert sand
		float Mountain = 0.f; // 0-1: inside a mountain range
		float Dryness = 0.f;  // 0-1: green to dry grass (slow variation)
	};

	inline double Clamp01(double X) { return X < 0.0 ? 0.0 : (X > 1.0 ? 1.0 : X); }
	inline double Smooth(double E0, double E1, double X)
	{
		const double T = Clamp01((X - E0) / (E1 - E0));
		return T * T * (3.0 - 2.0 * T);
	}
	inline double Lerp(double A, double B, double T) { return A + (B - A) * T; }

	inline uint32_t Hash2(int32_t X, int32_t Y, uint32_t Seed)
	{
		uint32_t H = Seed * 0x9E3779B9u + 0x632BE5ABu;
		H ^= uint32_t(X) * 0x85EBCA6Bu;
		H = (H << 13) | (H >> 19);
		H ^= uint32_t(Y) * 0xC2B2AE35u;
		H ^= H >> 16; H *= 0x7FEB352Du;
		H ^= H >> 15; H *= 0x846CA68Bu;
		H ^= H >> 16;
		return H;
	}

	/** Gradient (Perlin-style) noise, about -1..1. */
	inline double Gradient(double X, double Y, uint32_t Seed)
	{
		static const double G[16][2] = {
			{ 1.0, 0.0 }, { 0.92388, 0.38268 }, { 0.70711, 0.70711 }, { 0.38268, 0.92388 },
			{ 0.0, 1.0 }, { -0.38268, 0.92388 }, { -0.70711, 0.70711 }, { -0.92388, 0.38268 },
			{ -1.0, 0.0 }, { -0.92388, -0.38268 }, { -0.70711, -0.70711 }, { -0.38268, -0.92388 },
			{ 0.0, -1.0 }, { 0.38268, -0.92388 }, { 0.70711, -0.70711 }, { 0.92388, -0.38268 } };
		const double FX = std::floor(X), FY = std::floor(Y);
		const int32_t IX = int32_t(FX), IY = int32_t(FY);
		const double DX = X - FX, DY = Y - FY;
		auto Corner = [&](int32_t OX, int32_t OY)
		{
			const double* V = G[Hash2(IX + OX, IY + OY, Seed) & 15u];
			return V[0] * (DX - OX) + V[1] * (DY - OY);
		};
		const double UX = DX * DX * DX * (DX * (DX * 6.0 - 15.0) + 10.0);
		const double UY = DY * DY * DY * (DY * (DY * 6.0 - 15.0) + 10.0);
		return Lerp(Lerp(Corner(0, 0), Corner(1, 0), UX), Lerp(Corner(0, 1), Corner(1, 1), UX), UY) * 1.41;
	}

	/** Fractal sum, about -0.7..0.7 (octaves rotated by 37 degrees: no grid-aligned artefacts). */
	inline double Fbm(double X, double Y, int Octaves, uint32_t Seed)
	{
		double Sum = 0.0, Amp = 0.5, Norm = 0.0;
		for (int O = 0; O < Octaves; ++O)
		{
			Sum += Amp * Gradient(X, Y, Seed + uint32_t(O) * 101u);
			Norm += Amp;
			const double NX = 1.6 * X - 1.2 * Y, NY = 1.2 * X + 1.6 * Y;
			X = NX; Y = NY;
			Amp *= 0.5;
		}
		return Sum / Norm;
	}

	/** Ridged fractal (sharp crests), 0..1. */
	inline double Ridged(double X, double Y, int Octaves, uint32_t Seed)
	{
		double Sum = 0.0, Amp = 0.5, Norm = 0.0, Weight = 1.0;
		for (int O = 0; O < Octaves; ++O)
		{
			double N = 1.0 - std::fabs(Gradient(X, Y, Seed + uint32_t(O) * 131u));
			N *= N;
			N *= Weight;
			Weight = Clamp01(N * 1.6);
			Sum += Amp * N;
			Norm += Amp;
			const double NX = 1.6 * X - 1.2 * Y, NY = 1.2 * X + 1.6 * Y;
			X = NX; Y = NY;
			Amp *= 0.5;
		}
		return Sum / Norm;
	}

	class FModel
	{
	public:
		uint32_t Seed = 1337;
		double SeaLevelM = -1.5;
		double FlatRadiusM = 5000.0;   // plains around the origin, blended out to 3x this radius
		std::vector<FCircle> Oceans;   // sea inside (same circles as FAerosWaterRegion)
		std::vector<FCircle> Deserts;  // dunes and sand (same circles as FAerosDesertRegion)
		std::vector<FCircle> Pads;     // flat areas (volcano base)

		/** Call once after filling the regions: pads take the natural height of their centre. */
		void Init()
		{
			for (FCircle& P : Pads) P.Value = std::fmax(double(Raw(P.X, P.Y).HeightM), SeaLevelM + 40.0); // never in a lake
		}

		/** Distance to the nearest ocean shore (km), positive at sea; bays push the sea inland. */
		double ShoreKm(double X, double Y) const
		{
			double D = -1e9;
			for (const FCircle& C : Oceans)
			{
				const double Dist = std::sqrt((X - C.X) * (X - C.X) + (Y - C.Y) * (Y - C.Y));
				D = std::fmax(D, (C.RadiusM - Dist) * 1e-3);
			}
			if (D < -40.0) return D;
			const double KX = X * 1e-3, KY = Y * 1e-3;
			// Bays only push the sea inland: the simulation's ocean (circle) stays sea, and the extra
			// sea is below sea level, which the simulation also reads as ocean (terrain sampling).
			return D + 12.0 * Smooth(-0.05, 0.35, Fbm(KX / 30.0 + 3.3, KY / 30.0 - 8.8, 5, Seed + 83u));
		}

		FSurface Sample(double X, double Y) const
		{
			FSurface S = Raw(X, Y);
			for (const FCircle& P : Pads)
			{
				const double Dist = std::sqrt((X - P.X) * (X - P.X) + (Y - P.Y) * (Y - P.Y));
				const double A = Smooth(0.0, 1.0, (P.RadiusM - Dist) / P.EdgeM + 0.5);
				if (A > 0.0) S.HeightM = float(Lerp(S.HeightM, P.Value, A));
			}
			return S;
		}

	private:
		FSurface Raw(double X, double Y) const
		{
			const double KX = X * 1e-3, KY = Y * 1e-3;
			const double RKm = std::sqrt(KX * KX + KY * KY);
			const double Sea = SeaLevelM;
			FSurface S;

			// Rolling land: continental swell, hills, fine undulation
			const double Cont = Fbm(KX / 70.0, KY / 70.0, 4, Seed + 11u);
			const double Hills = Fbm(KX / 8.0, KY / 8.0, 6, Seed + 23u);
			const double Fine = Fbm(KX / 1.1, KY / 1.1, 4, Seed + 37u);
			double H = 70.0 + 110.0 * Cont + 60.0 * Hills + 6.0 * Fine;

			// Mountain ranges: ridged crests where a slow mask allows them, none near the start
			const double MMask = Smooth(0.1, 0.3, Fbm(KX / 110.0 + 17.3, KY / 110.0 - 5.1, 3, Seed + 41u)) * Smooth(20.0, 40.0, RKm);
			if (MMask > 0.0)
			{
				const double Rg = Ridged(KX / 26.0, KY / 26.0, 8, Seed + 53u);
				H += MMask * (4200.0 * Rg * Rg * Rg + 350.0 * (Hills + 0.5));
			}
			S.Mountain = float(MMask);

			// Lakes: hollows dug below sea level (water fills them), away from the mountains
			const double LakeN = Fbm(KX / 16.0 + 40.7, KY / 16.0 + 9.2, 4, Seed + 61u);
			const double LakeA = Smooth(0.27, 0.36, LakeN) * (1.0 - Smooth(0.05, 0.3, MMask));
			if (LakeA > 0.0) H = Lerp(H, Sea - 4.0 - 30.0 * Smooth(0.36, 0.5, LakeN), LakeA);

			// Deserts: low dunes on a plateau (crests stretched across the prevailing wind)
			double Des = 0.0;
			for (const FCircle& C : Deserts)
			{
				// Ragged edge (+-10 km) instead of a disc
				const double Dist = std::sqrt((X - C.X) * (X - C.X) + (Y - C.Y) * (Y - C.Y)) + 14000.0 * Fbm(KX / 25.0, KY / 25.0, 3, Seed + 67u);
				Des = std::fmax(Des, Smooth(0.0, 1.0, (C.RadiusM - Dist) / C.EdgeM + 0.5));
			}
			if (Des > 0.0)
			{
				const double Dune = Ridged(KX / 1.4, KY / 0.7, 3, Seed + 71u);
				H = Lerp(H, 160.0 + 60.0 * Cont + 45.0 * Dune, Des);
			}
			S.Desert = float(Des);

			// Coast: gentle beach (4 m per km), shelf, then the slope to the abyss
			const double De = ShoreKm(X, Y);
			if (De > -10.0)
			{
				double Coast = De <= 50.0 ? Sea - 4.0 * De : Sea - 200.0 - 40.0 * (De - 50.0);
				Coast = std::fmax(Coast, Sea - 4000.0);
				const double W = De >= 0.0 ? 1.0 : Smooth(-10.0, 0.0, De);
				H = Lerp(H, Coast, W);
			}

			// Plains around the start
			const double FlatKm = FlatRadiusM * 1e-3;
			H = Lerp(Sea + 2.2 + 1.2 * Fine, H, Smooth(FlatKm, 3.0 * FlatKm, RKm));

			S.HeightM = float(H);
			S.Dryness = float(Smooth(-0.25, 0.3, Fbm(KX / 30.0 - 7.7, KY / 30.0 + 2.1, 3, Seed + 97u)));
			return S;
		}
	};
}
