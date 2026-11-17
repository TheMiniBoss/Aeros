// Copyright Aeros. All Rights Reserved.

#include "Core/AerosCloudVolume.h"
#include "Core/AerosSnapshot.h"
#include "Core/AerosPhysics.h"
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
		float FlankX[4] = {}, FlankY[4] = {}, FlankTop[4] = {};
		float Reach = 10.f;              // km: columns farther than this from the updraft ignore the storm
		bool bActive = true;             // false: a dead storm's anvil alone (no tower, rain, flank)
		float Collapse = 0.f;            // 0-1: the tower thins as the updraft collapses
		// Supercell rain hook round the mesocyclone (Rot = 0: none)
		float Rot = 0.f;                 // 0-1 organisation of the rotation (mesocyclone)
		float MX = 1.f, MY = 0.f;        // motion (unit, local frame)
		float RX = 0.f, RY = 1.f;        // the mesocyclone's side: right of the motion in the north, left in the south
		float MesoX = 0.f, MesoY = 0.f;  // km, local: under the wall cloud (AAerosTornado::AnchorFor)
		float HookR = 2.5f;              // km
		float HookCover = 0.f;           // radians swept from the forward flank round the back (classic ~190 deg)
		float RainScale = 1.f;           // rain of the convective cells near the storm (LP dry, HP wet)
	};

	/** Anvil footprint per column (km of anvil top, strength, distance along the anvil / length).
	 *  Overlapping anvils blend: top, along and near-core are weighted means (Weight = sum of the splats),
	 *  so the top does not jump by 2 km where one anvil takes over from another. */
	struct FAnvilColumn
	{
		float Strength = 0.f;
		float TopKm = 0.f;
		float Along = 0.f; // 0 at the tower, 1 at the far end
		float NearCore = 0.f; // 1 close to the tower (mammatus), 0 beyond 30 km
		float Weight = 0.f;
	};

	/** Smooth 2D value noise in [0, 1] on a lattice of Cell km, from world km (stable when the grid recentres). */
	inline float ValueNoise(float WX, float WY, float Cell, uint32 Seed)
	{
		const float FX = WX / Cell, FY = WY / Cell;
		const int32 IX = FMath::FloorToInt(FX), IY = FMath::FloorToInt(FY);
		const float TX = FX - IX, TY = FY - IY;
		const float SX = TX * TX * (3.f - 2.f * TX), SY = TY * TY * (3.f - 2.f * TY);
		auto H = [Seed](int32 X, int32 Y) { return Aeros::Phys::Hash01(uint32(X), uint32(Y), Seed, 0x51ED27u); };
		const float A = FMath::Lerp(H(IX, IY), H(IX + 1, IY), SX);
		const float B = FMath::Lerp(H(IX, IY + 1), H(IX + 1, IY + 1), SX);
		return FMath::Lerp(A, B, SY);
	}
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

float FAerosCloudVolume::SampleRain(const TArray<uint8>& RG8, const FAerosCloudVolumeDesc& Desc, const FVector& World, float AltKm)
{
	using namespace AerosCloudVolumeInternal;
	const double Half = Desc.SizeKm * 0.5 * CmPerKm;
	const int32 X = FMath::FloorToInt(float((World.X - (Desc.Center.X - Half)) / (Desc.SizeKm * CmPerKm) * Desc.NX));
	const int32 Y = FMath::FloorToInt(float((World.Y - (Desc.Center.Y - Half)) / (Desc.SizeKm * CmPerKm) * Desc.NY));
	const int32 Z = FMath::FloorToInt(AltKm / Desc.VoxelKmZ());
	if (X < 0 || Y < 0 || Z < 0 || X >= Desc.NX || Y >= Desc.NY || Z >= Desc.NZ) return 0.f;
	const int32 Index = 2 * (X + Y * Desc.NX + Z * Desc.NX * Desc.NY) + 1;
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

void FAerosCloudVolume::AnvilBend(const FVector& WorldCm, float& OutAngle0, float& OutTurn)
{
	using namespace AerosCloudVolumeInternal;
	const float WX = float(WorldCm.X / CmPerKm), WY = float(WorldCm.Y / CmPerKm);
	OutAngle0 = (ValueNoise(WX, WY, 40.f, 21u) - 0.5f) * 2.f * FMath::DegreesToRadians(10.f);
	OutTurn = (ValueNoise(WX, WY, 40.f, 22u) - 0.5f) * 2.f * FMath::DegreesToRadians(25.f);
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
	// Storms are tracked once a simulated minute, the grid is built at every snapshot (~30 s): move them on
	// to the snapshot's time, or the clouds jumped ~1 km at each tracking (every minute of the clock)
	// Ground height (km above sea level) under a point, bilinear: cloud bases and tops are heights above
	// the ground in the simulation, and the grid's z is above sea level
	auto GroundKm = [&Snap](const FVector& World)
	{
		float GX = 0.f, GY = 0.f;
		if (!Snap.WorldToGrid(World, GX, GY)) return 0.f;
		const float FX = GX - 0.5f, FY = GY - 0.5f;
		const int32 I = FMath::FloorToInt(FX), J = FMath::FloorToInt(FY);
		const float TX = FX - I, TY = FY - J;
		const float E = Snap.At(I, J).Elev * (1.f - TX) * (1.f - TY) + Snap.At(I + 1, J).Elev * TX * (1.f - TY)
			+ Snap.At(I, J + 1).Elev * (1.f - TX) * TY + Snap.At(I + 1, J + 1).Elev * TX * TY;
		return FMath::Max(E, 0.f) * 0.001f;
	};

	for (int32 StormIndex = 0; StormIndex < Snap.Storms.Num(); ++StormIndex)
	{
		const FAerosStormData S = Snap.StormAt(StormIndex, Snap.SimTime);
		FStormShape Sh;
		Sh.bActive = S.IsActive();
		ToLocal(S.UpdraftLocation, Sh.UX, Sh.UY);
		ToLocal(S.HailCoreLocation.IsZero() ? S.UpdraftLocation : S.HailCoreLocation, Sh.HX, Sh.HY);
		if (FMath::Abs(Sh.UX) > HalfKm + 90.f || FMath::Abs(Sh.UY) > HalfKm + 90.f) continue;
		// Orographic: the storm stands on the ground under its updraft
		const float Ground = GroundKm(S.UpdraftLocation);
		Sh.LCL = Ground + FMath::Clamp(S.LCL * 0.001f, 0.3f, 3.f);
		Sh.EL = FMath::Clamp(Ground + S.CloudTop * 0.001f, Sh.LCL + 1.f, Desc.HeightKm - 1.f);
		const float Mini = S.SupercellType == EAerosSupercellType::LowTopped ? 0.75f : 1.f;
		Sh.TowerR = FMath::Clamp(S.RadiusKm * 0.35f, 2.f, 6.f) * Mini;
		Sh.CoreR = FMath::Max(S.RadiusKm * 0.6f, 3.f) * Mini;
		Sh.Overshoot = FMath::Clamp((S.MaxUpdraft - 30.f) / 20.f, 0.f, 1.f) * 2.f;
		Sh.Darkness = FMath::Clamp(FMath::Sqrt(S.MaxRainRate / 100.f), 0.f, 1.f);
		// Collapse: the updraft falls well under its best, the tower thins (it rains itself out)
		if (S.Stage == EAerosStormStage::Dissipating && S.PeakUpdraft > 5.f)
		{
			Sh.Collapse = FMath::Clamp(1.f - S.MaxUpdraft / S.PeakUpdraft, 0.f, 1.f);
			Sh.TowerR *= 1.f - 0.45f * Sh.Collapse;
			Sh.Overshoot *= 1.f - Sh.Collapse;
		}
		// Storm-relative frame: motion, and the side of the mesocyclone (right of the motion in the northern
		// hemisphere, mirrored in the southern one: cyclonic storms are left movers there)
		FVector Motion = S.Velocity.GetSafeNormal2D();
		if (Motion.Size2D() < 0.5) Motion = Dir500;
		const float Hemi = Snap.Settings.LatitudeDeg < 0.f ? -1.f : 1.f;
		Sh.MX = float(Motion.X); Sh.MY = float(Motion.Y);
		Sh.RX = -Sh.MY * Hemi; Sh.RY = Sh.MX * Hemi;
		Sh.MesoX = Sh.UX - Sh.MX + 0.5f * Sh.RX;
		Sh.MesoY = Sh.UY - Sh.MY + 0.5f * Sh.RY;
		// Supercell flavour (Keraunos): where the rain falls round the mesocyclone. LP: a small core far
		// forward-left, light; classic: the forward-flank core and a hook wrapping behind the mesocyclone; HP:
		// a big dark core close in, the rain wrapping right round it (tornadoes hidden in the rain).
		Sh.Rot = Smooth(0.3f, 0.8f, S.Mesocyclone) * (Sh.bActive ? 1.f : 0.f);
		if (Sh.Rot > 0.f)
		{
			const float Load = FMath::Clamp(S.PrecipLoad, 0.f, 1.f);
			const float Ahead = FMath::Lerp(5.f, 2.f, Load), Left = FMath::Lerp(5.f, 2.5f, Load);
			const float SynX = Sh.UX + Ahead * Sh.MX - Left * Sh.RX, SynY = Sh.UY + Ahead * Sh.MY - Left * Sh.RY;
			Sh.HX = FMath::Lerp(Sh.HX, SynX, Sh.Rot);
			Sh.HY = FMath::Lerp(Sh.HY, SynY, Sh.Rot);
			Sh.CoreR = FMath::Lerp(Sh.CoreR, FMath::Min(Sh.CoreR, 5.f) * FMath::Lerp(0.55f, 1.35f, Load), Sh.Rot);
			Sh.Darkness = FMath::Min(Sh.Darkness * FMath::Lerp(1.f, FMath::Lerp(0.3f, 1.25f, Load), Sh.Rot), 1.f);
			Sh.HookR = FMath::Lerp(2.2f, 3.4f, Load) * Mini;
			Sh.HookCover = FMath::DegreesToRadians(FMath::Lerp(190.f, 320.f, Smooth(0.5f, 0.9f, Load))) * Smooth(0.3f, 0.45f, Load);
			Sh.RainScale = FMath::Lerp(1.f, FMath::Lerp(0.35f, 1.15f, Load), Sh.Rot);
		}
		// Flanking line: towers stepping down behind the updraft, on the mesocyclone's side
		const float FX = -Sh.MX + Sh.RX, FY = -Sh.MY + Sh.RY;
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
		if (Sh.bActive) Shapes.Add(Sh);

		// Anvil footprint: it builds while the storm matures (S.Anvil), spreads at the top the ice reached, and
		// outlives the updraft (a dead storm is its anvil alone, dissolving)
		if (S.Anvil < 0.03f) continue;
		const float AnvilTopKm = FMath::Clamp(Ground + FMath::Max(S.AnvilTop, S.CloudTop) * 0.001f, Sh.LCL + 1.f, Desc.HeightKm - 1.f);
		const float Strength = (0.5f + 0.5f * FMath::Clamp((AnvilTopKm - Ground - 9.f) / 4.f, 0.f, 1.f)) * Smooth(0.f, 0.5f, S.Anvil);
		const float LengthKm = (15.f + 35.f * Strength + 0.8f * FMath::Min(Upper, 20.f)) * (0.35f + 0.65f * FMath::Clamp(S.Anvil, 0.f, 1.f));
		const float AX = Sh.UX + DX5 * Lean, AY = Sh.UY + DY5 * Lean; // starts from the leaning top
		// Fans out and curves (AnvilBend, simulation frame counter-clockwise = world frame clockwise: Y is flipped)
		float Angle0 = 0.f, Turn = 0.f;
		FAerosCloudVolume::AnvilBend(S.UpdraftLocation, Angle0, Turn);
		const float BaseAngle = FMath::Atan2(DY5, DX5);
		float PathX = AX, PathY = AY;
		for (float Along = -2.f; Along <= LengthKm; Along += VXY)
		{
			const float Fade = Along < 0.f ? 0.6f : FMath::Sqrt(FMath::Max(1.f - Along / LengthKm, 0.f));
			const float Radius = 3.f + 0.18f * FMath::Max(Along, 0.f);
			const float Ang = BaseAngle - (Angle0 + Turn * FMath::Max(Along, 0.f) / LengthKm);
			float CX = AX + FMath::Cos(Ang) * Along, CY = AY + FMath::Sin(Ang) * Along;
			if (Along >= 0.f)
			{
				CX = PathX;
				CY = PathY;
				PathX += FMath::Cos(Ang) * VXY;
				PathY += FMath::Sin(Ang) * VXY;
			}
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
					if (V <= 0.f) continue;
					FAnvilColumn& A = Anvil[X + Y * NX];
					A.Strength = FMath::Max(A.Strength, V);
					A.Weight += V;
					A.TopKm += V * AnvilTopKm;
					A.Along += V * FMath::Clamp(Along / LengthKm, 0.f, 1.f);
					A.NearCore += V * FMath::Clamp(1.f - Along / 30.f, 0.f, 1.f);
				}
			}
		}
	}

	for (FAnvilColumn& A : Anvil)
	{
		if (A.Weight <= 0.f) continue;
		A.TopKm /= A.Weight;
		A.Along /= A.Weight;
		A.NearCore /= A.Weight;
	}
	const float CenterKmX = float(Desc.Center.X / CmPerKm), CenterKmY = float(Desc.Center.Y / CmPerKm);

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
			float ConvBase = 0.f, ConvTop = 0.f, ConvRain = 0.f;
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
				// Stratiform decks (St, Sc, Ns) are drawn by the renderer's low layer (Core/AerosCloudTypes.h)
				StratDensity = 0.f * Cover;
				if (StratDensity > 0.f)
				{
					// Heights above the ground: the deck follows the terrain (orographic)
					const float Ground = FMath::Max(Mix(C00.Elev, C10.Elev, C01.Elev, C11.Elev), 0.f) * 0.001f;
					StratBase = Ground + Mix(C00.CloudBase, C10.CloudBase, C01.CloudBase, C11.CloudBase) * 0.001f;
					StratTop = FMath::Max(Ground + Mix(C00.CloudTop, C10.CloudTop, C01.CloudTop, C11.CloudTop) * 0.001f, StratBase + 0.3f);
				}
				Gust = Mix(GustFront(Snap, I, J), GustFront(Snap, I + 1, J), GustFront(Snap, I, J + 1), GustFront(Snap, I + 1, J + 1));
				// Convective column wherever the simulation convects (tracked storm or not), from the cell's
				// base to its top above the ground, with its rain under the base
				if (Conv > 0.15f)
				{
					const float Ground = FMath::Max(Mix(C00.Elev, C10.Elev, C01.Elev, C11.Elev), 0.f) * 0.001f;
					ConvBase = Ground + Mix(C00.CloudBase, C10.CloudBase, C01.CloudBase, C11.CloudBase) * 0.001f;
					ConvTop = FMath::Max(Ground + Mix(C00.CloudTop, C10.CloudTop, C01.CloudTop, C11.CloudTop) * 0.001f, ConvBase + 1.f);
					ConvRain = FMath::Clamp(FMath::Sqrt(FMath::Max(Mix(C00.PrecipRate, C10.PrecipRate, C01.PrecipRate, C11.PrecipRate), 0.f) / 100.f), 0.f, 1.f);
				}
			}

			Near.Reset();
			for (int32 K = 0; K < Shapes.Num(); ++K)
			{
				const FStormShape& Sh = Shapes[K];
				if (FMath::Square(PX - Sh.UX) + FMath::Square(PY - Sh.UY) < FMath::Square(Sh.Reach)) Near.Add(K);
			}
			const FAnvilColumn& A = Anvil[X + Y * NX];
			if (Near.Num() == 0 && A.Strength <= 0.f && StratDensity <= 0.f && Gust <= 0.01f && ConvTop <= 0.f) continue;
			// The cells' own rain near a supercell follows its flavour (an LP is dry, an HP very wet), and the
			// hook: rain curling from the forward flank round the back of the mesocyclone
			float HookRain = 0.f, HookBase = 0.f;
			for (int32 K : Near)
			{
				const FStormShape& Sh = Shapes[K];
				if (Sh.Rot <= 0.f) continue;
				ConvRain *= FMath::Lerp(1.f, Sh.RainScale, 1.f - Smooth(10.f, 18.f, FMath::Sqrt(FMath::Square(PX - Sh.UX) + FMath::Square(PY - Sh.UY))));
				if (Sh.HookCover <= 0.f) continue;
				const float HX = PX - Sh.MesoX, HY = PY - Sh.MesoY;
				const float Dist = FMath::Sqrt(HX * HX + HY * HY);
				// Angle from the forward flank (left of the mesocyclone, 0) round the back (pi/2) to its right (pi)
				float Ang = FMath::Atan2(-(HX * Sh.MX + HY * Sh.MY), -(HX * Sh.RX + HY * Sh.RY));
				if (Ang < -0.35f) Ang += 2.f * PI;
				const float Sweep = Smooth(-0.35f, 0.f, Ang) * (1.f - Smooth(Sh.HookCover - 0.6f, Sh.HookCover, Ang));
				const float Band = 1.f - Smooth(0.55f, 1.3f, FMath::Abs(Dist - Sh.HookR));
				const float Hook = 0.75f * Sh.Darkness * Sh.Rot * Sweep * Band;
				if (Hook > HookRain) { HookRain = Hook; HookBase = Sh.LCL; }
			}

			for (int32 Z = 0; Z < NZ; ++Z)
			{
				const float Alt = (Z + 0.5f) * VZ;
				float Dens = 0.f, Dark = 0.f, RainAmount = 0.f;

				// Convective column (multicell areas away from the towers were clear sky)
				if (ConvTop > 0.f)
				{
					const float Col = 0.9f * Smooth(0.15f, 0.5f, Conv) * Smooth(ConvBase - 0.1f, ConvBase + 0.25f, Alt) * (1.f - Smooth(ConvTop - 0.6f, ConvTop + 0.5f, Alt));
					Dens = FMath::Max(Dens, Col);
					if (Col > 0.f) Dark = FMath::Max(Dark, ConvRain * Col);
					if (Alt < ConvBase && ConvRain > 0.05f) RainAmount = FMath::Max(RainAmount, ConvRain * Smooth(0.15f, 0.5f, Conv));
				}

				// Stratiform deck
				if (StratDensity > 0.f && Alt > StratBase && Alt < StratTop) Dens = StratDensity * Smooth(StratBase, StratBase + 0.2f, Alt);

				// Shelf cloud: layered wedge on the gust front, 0.45-1.9 km
				if (Gust > 0.01f) Dens = FMath::Max(Dens, Gust * 0.8f * Smooth(0.4f, 0.5f, Alt) * (1.f - Smooth(1.3f, 1.9f, Alt)));

				// Anvil: flat top at the EL, base rising downwind, mammatus lobes near the core
				if (A.Strength > 0.f)
				{
					// Thinner toward the edges and far downwind: a wedge, not a slab with vertical walls
					const float Thick = (1.5f + 1.0f * A.Strength) * (1.f - 0.5f * A.Along) * (0.3f + 0.7f * Smooth(0.f, 0.6f, A.Strength));
					// Mammatus near the core: irregular pouches (value noise on world km), not a sine grid
					const float WX = CenterKmX + PX, WY = CenterKmY + PY;
					const float Pouch = Smooth(0.45f, 0.85f, ValueNoise(WX, WY, 1.6f, 11u) * 0.7f + ValueNoise(WX, WY, 0.7f, 12u) * 0.3f);
					const float Lobes = 0.7f * A.NearCore * Pouch;
					const float Base = A.TopKm - Thick - Lobes;
					// Ramps over ~3-4 voxels (333 m each): a 300 m fall-off put the top in one layer or the
					// next, and seen from above the anvil showed terraces like contour lines. The shader's
					// threshold (voxIce) keeps the visible surface crisp.
					if (Alt > Base - 0.4f && Alt < A.TopKm + 0.8f)
					{
						Dens = FMath::Max(Dens, 0.8f * A.Strength * Smooth(Base - 0.4f, Base + 0.5f, Alt) * (1.f - Smooth(A.TopKm - 0.6f, A.TopKm + 0.8f, Alt)));
					}
				}

				for (int32 K : Near)
				{
					const FStormShape& Sh = Shapes[K];
					// Tower, leaning downshear, domed top (thinner and softer while it collapses)
					if (Alt >= Sh.LCL && Alt <= Sh.EL)
					{
						const float Lean = 0.25f * (Alt - Sh.LCL);
						const float D = FMath::Sqrt(FMath::Square(PX - (Sh.UX + DX5 * Lean)) + FMath::Square(PY - (Sh.UY + DY5 * Lean)));
						const float Dome = Alt > Sh.EL - 1.f ? FMath::Sqrt(FMath::Max(1.f - FMath::Square(Alt - (Sh.EL - 1.f)), 0.f)) : 1.f;
						const float R = Sh.TowerR * FMath::Max(Dome, 0.05f);
						// No darkness here: the glaciated upper tower is bright white; its dark base comes
						// from the renderer's self-shadowing, not from the albedo
						Dens = FMath::Max(Dens, (1.f - 0.4f * Sh.Collapse) * (1.f - Smooth(0.6f * R, R, D)));
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
					// Rain shaft below the base: no cloud density (R stays 0), rain amount in G; the shader turns
					// it into a thin grey veil (a mesh curtain drew over the clouds as a white cylinder)
					if (Alt < Sh.LCL && Sh.Darkness > 0.f)
					{
						const float D = FMath::Sqrt(FMath::Square(PX - Sh.HX) + FMath::Square(PY - Sh.HY));
						const float Rain = Sh.Darkness * (1.f - Smooth(0.5f * Sh.CoreR, Sh.CoreR, D));
						if (Rain > 0.f) RainAmount = FMath::Max(RainAmount, Rain);
					}
					// (The wall cloud is drawn by the renderer, rotating: AerosCloudDensity.ush, storm descriptors)
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

				if (HookRain > 0.f)
				{
					// Under the base: the rain of the hook; in the low cloud above it, its darkness
					if (Alt < HookBase) RainAmount = FMath::Max(RainAmount, HookRain);
					else if (Alt < HookBase + 2.f && Dens > 0.f) Dark = FMath::Max(Dark, HookRain);
				}
				if (Dens <= 0.f && RainAmount <= 0.f) continue;
				const int32 Index = 2 * (X + Y * NX + Z * NX * NY);
				OutRG8[Index] = uint8(FMath::Clamp(Dens, 0.f, 1.f) * 255.f + 0.5f);
				OutRG8[Index + 1] = uint8(FMath::Clamp(FMath::Max(Dark, RainAmount), 0.f, 1.f) * 255.f + 0.5f);
			}
		}
	});
}
