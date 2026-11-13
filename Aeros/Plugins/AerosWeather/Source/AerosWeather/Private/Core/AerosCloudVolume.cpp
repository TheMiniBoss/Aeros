// Copyright Aeros. All Rights Reserved.

#include "Core/AerosCloudVolume.h"
#include "Core/AerosSnapshot.h"
#include "Async/ParallelFor.h"

// Unity build: helpers in a uniquely named namespace (see CLAUDE.md).
namespace AerosCloudVolumeInternal
{
	constexpr double CmPerKm = 100000.0;

	inline float Smooth(float Edge0, float Edge1, float X)
	{
		const float T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	/** Everything the per-voxel loop needs about one storm, in km and in the grid's local frame. */
	struct FStormShape
	{
		float UX = 0.f, UY = 0.f;        // updraft (km, local)
		float HX = 0.f, HY = 0.f;        // hail / precipitation core (km, local)
		float LCL = 1.f, EL = 10.f;      // km
		float TowerR = 3.f;              // km
		float CoreR = 4.f;               // km
		float Overshoot = 0.f;           // km above the EL
		float Darkness = 0.f;            // 0-1
		float WallR = 0.f;               // 0 = no wall cloud
		float FlankX[4] = {}, FlankY[4] = {}, FlankTop[4] = {};
		float Reach = 10.f;              // km: columns farther than this from the updraft ignore the storm
	};

	/** Anvil footprint per column (km of anvil top, strength, distance along the anvil / length). */
	struct FAnvilColumn
	{
		float Strength = 0.f;
		float TopKm = 0.f;
		float Along = 0.f; // 0 at the tower, 1 at the far end
		float NearCore = 0.f; // 1 close to the tower (mammatus), 0 beyond 30 km
	};
}

FVector FAerosCloudVolume::SnapCenter(const FVector& Viewer, float StepKm)
{
	const double Step = double(StepKm) * AerosCloudVolumeInternal::CmPerKm;
	return FVector(double(FMath::FloorToInt64(Viewer.X / Step + 0.5)) * Step, double(FMath::FloorToInt64(Viewer.Y / Step + 0.5)) * Step, 0.0);
}

float FAerosCloudVolume::SampleDensity(const TArray<uint8>& RG8, const FAerosCloudVolumeDesc& Desc, const FVector& World, float AltKm)
{
	using namespace AerosCloudVolumeInternal;
	const double Half = Desc.SizeKm * 0.5 * CmPerKm;
	const int32 X = FMath::FloorToInt(float((World.X - (Desc.Center.X - Half)) / (Desc.SizeKm * CmPerKm) * Desc.NX));
	const int32 Y = FMath::FloorToInt(float((World.Y - (Desc.Center.Y - Half)) / (Desc.SizeKm * CmPerKm) * Desc.NY));
	const int32 Z = FMath::FloorToInt(AltKm / Desc.VoxelKmZ());
	if (X < 0 || Y < 0 || Z < 0 || X >= Desc.NX || Y >= Desc.NY || Z >= Desc.NZ) return 0.f;
	const int32 Index = 2 * (X + Y * Desc.NX + Z * Desc.NX * Desc.NY);
	return RG8.IsValidIndex(Index) ? RG8[Index] / 255.f : 0.f;
}

float FAerosCloudVolume::GustFront(const FAerosSnapshot& Snap, int32 I, int32 J)
{
	// Cold outflow leading edge: surface convergence next to heavy rain (but not inside the core)
	const float Inv2Dx = 1.f / (2.f * Snap.CellM);
	const float Div = (Snap.At(I + 1, J).Us - Snap.At(I - 1, J).Us) * Inv2Dx + (Snap.At(I, J + 1).Vs - Snap.At(I, J - 1).Vs) * Inv2Dx;
	float RainNear = 0.f;
	for (int32 DY = -3; DY <= 3; ++DY)
		for (int32 DX = -3; DX <= 3; ++DX)
			RainNear = FMath::Max(RainNear, Snap.At(I + DX, J + DY).PrecipRate);
	const float Here = Snap.At(I, J).PrecipRate;
	return FMath::Clamp(-Div * 1500.f, 0.f, 1.f) * FMath::Clamp(RainNear / 15.f, 0.f, 1.f) * (Here < 20.f ? 1.f : 0.3f);
}

void FAerosCloudVolume::Build(const FAerosSnapshot& Snap, const FAerosCloudVolumeDesc& Desc, TArray<uint8>& OutRG8)
{
	using namespace AerosCloudVolumeInternal;
	const int32 NX = Desc.NX, NY = Desc.NY, NZ = Desc.NZ;
	OutRG8.SetNumZeroed(NX * NY * NZ * 2);
	if (!Snap.IsValid()) return;

	const float VXY = Desc.VoxelKmXY(), VZ = Desc.VoxelKmZ();
	const float HalfKm = Desc.SizeKm * 0.5f;
	auto ToLocal = [&Desc](const FVector& W, float& OutX, float& OutY)
	{
		OutX = float((W.X - Desc.Center.X) / CmPerKm);
		OutY = float((W.Y - Desc.Center.Y) / CmPerKm);
	};

	// Upper wind (500 hPa) in world km axes: anvils spread along it, towers lean with it
	FVector Dir500 = FAerosSnapshot::SimToWorldDir(Snap.Synoptic.Wind500.X, Snap.Synoptic.Wind500.Y).GetSafeNormal2D();
	if (Dir500.Size2D() < 0.5) Dir500 = FVector(1.0, 0.0, 0.0);
	const float Upper = float(Snap.Synoptic.Wind500.Size());
	const float DX5 = float(Dir500.X), DY5 = float(Dir500.Y);

	// ---- Storm shapes near the grid
	TArray<FStormShape> Shapes;
	TArray<FAnvilColumn> Anvil;
	Anvil.SetNum(NX * NY);
	for (const FAerosStormData& S : Snap.Storms)
	{
		FStormShape Sh;
		ToLocal(S.UpdraftLocation, Sh.UX, Sh.UY);
		ToLocal(S.HailCoreLocation.IsZero() ? S.UpdraftLocation : S.HailCoreLocation, Sh.HX, Sh.HY);
		if (FMath::Abs(Sh.UX) > HalfKm + 90.f || FMath::Abs(Sh.UY) > HalfKm + 90.f) continue;
		Sh.LCL = FMath::Clamp(S.LCL * 0.001f, 0.3f, 3.f);
		Sh.EL = FMath::Clamp(S.CloudTop * 0.001f, Sh.LCL + 1.f, Desc.HeightKm - 1.f);
		Sh.TowerR = FMath::Clamp(S.RadiusKm * 0.35f, 2.f, 6.f);
		Sh.CoreR = FMath::Max(S.RadiusKm * 0.6f, 3.f);
		Sh.Overshoot = FMath::Clamp((S.MaxUpdraft - 30.f) / 20.f, 0.f, 1.f) * 2.f;
		Sh.Darkness = FMath::Clamp(FMath::Sqrt(S.MaxRainRate / 100.f), 0.f, 1.f);
		Sh.WallR = S.Mesocyclone > 0.4f ? FMath::Clamp(Sh.TowerR * 0.6f, 1.5f, 3.f) : 0.f;
		// Flanking line: towers stepping down behind-right of the updraft
		FVector Motion = S.Velocity.GetSafeNormal2D();
		if (Motion.Size2D() < 0.5) Motion = Dir500;
		const float FX = float(-Motion.X - Motion.Y), FY = float(-Motion.Y + Motion.X); // -motion + right
		const float FL = FMath::Max(FMath::Sqrt(FX * FX + FY * FY), 0.001f);
		static const float FlankFrac[4] = { 0.8f, 0.6f, 0.45f, 0.3f };
		for (int32 K = 0; K < 4; ++K)
		{
			Sh.FlankX[K] = Sh.UX + FX / FL * 4.f * (K + 1);
			Sh.FlankY[K] = Sh.UY + FY / FL * 4.f * (K + 1);
			Sh.FlankTop[K] = Sh.LCL + FlankFrac[K] * (Sh.EL - Sh.LCL);
		}
		const float Lean = 0.25f * (Sh.EL - Sh.LCL);
		Sh.Reach = FMath::Max(Sh.TowerR + Lean, FMath::Max(Sh.CoreR + 2.f, 18.f)) + 2.f;
		Shapes.Add(Sh);

		// Anvil footprint: only mature cumulonimbus reach the tropopause
		if (S.CloudTop < 9000.f) continue;
		const float Strength = 0.5f + 0.5f * FMath::Clamp((S.CloudTop - 9000.f) / 4000.f, 0.f, 1.f);
		const float LengthKm = 15.f + 35.f * Strength + 0.8f * FMath::Min(Upper, 20.f);
		const float AX = Sh.UX + DX5 * Lean, AY = Sh.UY + DY5 * Lean; // starts from the leaning top
		for (float Along = -2.f; Along <= LengthKm; Along += VXY)
		{
			const float Fade = Along < 0.f ? 0.6f : FMath::Sqrt(FMath::Max(1.f - Along / LengthKm, 0.f));
			const float Radius = 3.f + 0.18f * FMath::Max(Along, 0.f);
			const float CX = AX + DX5 * Along, CY = AY + DY5 * Along;
			const int32 X0 = FMath::FloorToInt((CX - Radius + HalfKm) / VXY), X1 = FMath::CeilToInt((CX + Radius + HalfKm) / VXY);
			const int32 Y0 = FMath::FloorToInt((CY - Radius + HalfKm) / VXY), Y1 = FMath::CeilToInt((CY + Radius + HalfKm) / VXY);
			for (int32 Y = FMath::Max(Y0, 0); Y <= FMath::Min(Y1, NY - 1); ++Y)
			{
				for (int32 X = FMath::Max(X0, 0); X <= FMath::Min(X1, NX - 1); ++X)
				{
					const float PX = (X + 0.5f) * VXY - HalfKm, PY = (Y + 0.5f) * VXY - HalfKm;
					const float D2 = (FMath::Square(PX - CX) + FMath::Square(PY - CY)) / FMath::Square(Radius);
					if (D2 >= 1.f) continue;
					const float V = Strength * Fade * (1.f - D2 * D2);
					FAnvilColumn& A = Anvil[X + Y * NX];
					if (V > A.Strength)
					{
						A.Strength = V;
						A.TopKm = Sh.EL;
						A.Along = FMath::Clamp(Along / LengthKm, 0.f, 1.f);
						A.NearCore = FMath::Clamp(1.f - Along / 30.f, 0.f, 1.f);
					}
				}
			}
		}
	}

	// ---- Fill, one row of columns per task
	ParallelFor(NY, [&](int32 Y)
	{
		TArray<int32> Near;
		for (int32 X = 0; X < NX; ++X)
		{
			const float PX = (X + 0.5f) * VXY - HalfKm, PY = (Y + 0.5f) * VXY - HalfKm;
			const FVector World(Desc.Center.X + PX * CmPerKm, Desc.Center.Y + PY * CmPerKm, 0.0);

			// Column from the simulation (nearest cell): stratiform deck, gust front, convection
			float StratDensity = 0.f, StratBase = 0.f, StratTop = 0.f, Gust = 0.f, Conv = 0.f;
			float GX = 0.f, GY = 0.f;
			if (Snap.WorldToGrid(World, GX, GY))
			{
				// Bilinear between the 4 nearest cell centres: nearest-cell reads drew 2 km square blocks
				const float FX = GX - 0.5f, FY = GY - 0.5f;
				const int32 I = FMath::FloorToInt(FX), J = FMath::FloorToInt(FY);
				const float TX = FX - I, TY = FY - J;
				const float W00 = (1.f - TX) * (1.f - TY), W10 = TX * (1.f - TY), W01 = (1.f - TX) * TY, W11 = TX * TY;
				const FAerosSnapCell& C00 = Snap.At(I, J);
				const FAerosSnapCell& C10 = Snap.At(I + 1, J);
				const FAerosSnapCell& C01 = Snap.At(I, J + 1);
				const FAerosSnapCell& C11 = Snap.At(I + 1, J + 1);
				auto Mix = [&](float A00, float A10, float A01, float A11) { return A00 * W00 + A10 * W10 + A01 * W01 + A11 * W11; };
				Conv = FMath::Clamp(Mix(C00.Conv, C10.Conv, C01.Conv, C11.Conv), 0.f, 1.f);
				const float Cover = Mix(C00.CloudCover, C10.CloudCover, C01.CloudCover, C11.CloudCover);
				// Stratiform deck only for a really overcast, non-convective sky (nimbostratus / stratus)
				StratDensity = 0.5f * Smooth(0.7f, 0.95f, Cover) * (1.f - Smooth(0.1f, 0.25f, Conv));
				if (StratDensity > 0.f)
				{
					StratBase = Mix(C00.CloudBase, C10.CloudBase, C01.CloudBase, C11.CloudBase) * 0.001f;
					StratTop = FMath::Max(Mix(C00.CloudTop, C10.CloudTop, C01.CloudTop, C11.CloudTop) * 0.001f, StratBase + 0.3f);
				}
				Gust = Mix(GustFront(Snap, I, J), GustFront(Snap, I + 1, J), GustFront(Snap, I, J + 1), GustFront(Snap, I + 1, J + 1));
			}

			Near.Reset();
			for (int32 K = 0; K < Shapes.Num(); ++K)
			{
				const FStormShape& Sh = Shapes[K];
				if (FMath::Square(PX - Sh.UX) + FMath::Square(PY - Sh.UY) < FMath::Square(Sh.Reach)) Near.Add(K);
			}
			const FAnvilColumn& A = Anvil[X + Y * NX];
			if (Near.Num() == 0 && A.Strength <= 0.f && StratDensity <= 0.f && Gust <= 0.01f) continue;

			for (int32 Z = 0; Z < NZ; ++Z)
			{
				const float Alt = (Z + 0.5f) * VZ;
				float Dens = 0.f, Dark = 0.f;

				// Stratiform deck
				if (StratDensity > 0.f && Alt > StratBase && Alt < StratTop) Dens = StratDensity * Smooth(StratBase, StratBase + 0.2f, Alt);

				// Shelf cloud: layered wedge on the gust front, 0.45-1.9 km
				if (Gust > 0.01f) Dens = FMath::Max(Dens, Gust * 0.8f * Smooth(0.4f, 0.5f, Alt) * (1.f - Smooth(1.3f, 1.9f, Alt)));

				// Anvil: flat top at the EL, base rising downwind, mammatus lobes near the core
				if (A.Strength > 0.f)
				{
					const float Thick = (1.5f + 1.0f * A.Strength) * (1.f - 0.5f * A.Along);
					const float Lobes = 0.5f * A.NearCore * FMath::Clamp(0.5f + 0.25f * (FMath::Sin(PX * 1.9f) + FMath::Sin(PY * 2.3f)), 0.f, 1.f);
					const float Base = A.TopKm - Thick - Lobes;
					if (Alt > Base && Alt < A.TopKm + 0.2f)
					{
						Dens = FMath::Max(Dens, 0.8f * A.Strength * Smooth(Base, Base + 0.25f, Alt) * (1.f - Smooth(A.TopKm - 0.1f, A.TopKm + 0.2f, Alt)));
					}
				}

				for (int32 K : Near)
				{
					const FStormShape& Sh = Shapes[K];
					// Tower, leaning downshear, domed top
					if (Alt >= Sh.LCL && Alt <= Sh.EL)
					{
						const float Lean = 0.25f * (Alt - Sh.LCL);
						const float D = FMath::Sqrt(FMath::Square(PX - (Sh.UX + DX5 * Lean)) + FMath::Square(PY - (Sh.UY + DY5 * Lean)));
						const float Dome = Alt > Sh.EL - 1.f ? FMath::Sqrt(FMath::Max(1.f - FMath::Square(Alt - (Sh.EL - 1.f)), 0.f)) : 1.f;
						const float R = Sh.TowerR * FMath::Max(Dome, 0.05f);
						Dens = FMath::Max(Dens, 1.f - Smooth(0.6f * R, R, D));
						Dark = FMath::Max(Dark, 0.3f * Conv * (Alt - Sh.LCL) / (Sh.EL - Sh.LCL));
					}
					// Overshooting top above the EL
					if (Sh.Overshoot > 0.f && Alt > Sh.EL && Alt < Sh.EL + Sh.Overshoot)
					{
						const float Lean = 0.25f * (Sh.EL - Sh.LCL);
						const float D = FMath::Sqrt(FMath::Square(PX - (Sh.UX + DX5 * Lean)) + FMath::Square(PY - (Sh.UY + DY5 * Lean)));
						const float R = 0.5f * Sh.TowerR * FMath::Sqrt(FMath::Max(1.f - FMath::Square((Alt - Sh.EL) / Sh.Overshoot), 0.f));
						if (R > 0.01f) Dens = FMath::Max(Dens, 1.f - Smooth(0.6f * R, R, D));
					}
					// Precipitation core (forward flank): thick, dark
					if (Alt >= Sh.LCL && Alt <= 0.7f * Sh.EL)
					{
						const float D = FMath::Sqrt(FMath::Square(PX - Sh.HX) + FMath::Square(PY - Sh.HY));
						const float Core = 0.8f * (1.f - Smooth(0.7f * Sh.CoreR, Sh.CoreR, D));
						if (Core > 0.f)
						{
							Dens = FMath::Max(Dens, Core);
							Dark = FMath::Max(Dark, Sh.Darkness * Core / 0.8f);
						}
					}
					// Wall cloud: lowered rotating base under the updraft
					if (Sh.WallR > 0.f && Alt >= Sh.LCL - 0.4f && Alt < Sh.LCL)
					{
						const float D = FMath::Sqrt(FMath::Square(PX - Sh.UX) + FMath::Square(PY - Sh.UY));
						Dens = FMath::Max(Dens, 0.9f * (1.f - Smooth(0.7f * Sh.WallR, Sh.WallR, D)));
					}
					// Flanking line: smaller domed towers
					for (int32 F = 0; F < 4; ++F)
					{
						if (Alt < Sh.LCL || Alt > Sh.FlankTop[F]) continue;
						const float D = FMath::Sqrt(FMath::Square(PX - Sh.FlankX[F]) + FMath::Square(PY - Sh.FlankY[F]));
						const float Top = Sh.FlankTop[F];
						const float Dome = Alt > Top - 0.8f ? FMath::Sqrt(FMath::Max(1.f - FMath::Square((Alt - (Top - 0.8f)) / 0.8f), 0.f)) : 1.f;
						const float R = 1.5f * FMath::Max(Dome, 0.05f);
						Dens = FMath::Max(Dens, 0.9f * (1.f - Smooth(0.6f * R, R, D)));
					}
				}

				if (Dens <= 0.f) continue;
				const int32 Index = 2 * (X + Y * NX + Z * NX * NY);
				OutRG8[Index] = uint8(FMath::Clamp(Dens, 0.f, 1.f) * 255.f + 0.5f);
				OutRG8[Index + 1] = uint8(FMath::Clamp(Dark, 0.f, 1.f) * 255.f + 0.5f);
			}
		}
	});
}
