// Copyright Aeros. All Rights Reserved.
// Immutable, read-only copy of the simulation published by the sim thread.
// The game thread holds a TSharedPtr to it: no lock, no tearing, no race.

#pragma once

#include "CoreMinimal.h"
#include "AerosTypes.h"

/** Slim per-cell render/query data (~150 bytes). */
struct FAerosSnapCell
{
	float T = 15.f, Td = 5.f, RH = 0.5f, P = 1013.f, Tg = 15.f;
	float Us = 0.f, Vs = 0.f, U850 = 0.f, V850 = 0.f;
	float Visibility = 20000.f;
	float CloudCover = 0.f, CloudBase = 1500.f, CloudTop = 2000.f, CloudWater = 0.f;
	float PrecipRate = 0.f, Hail = 0.f, DBZ = -32.f, Lightning = 0.f;
	float Conv = 0.f, W = 0.f, Meso = 0.f;
	float CAPE = 0.f, CIN = 0.f, LCL = 0.f, LFC = 0.f, EL = 0.f, FZL = 3000.f, LI = 0.f;
	float SRH01 = 0.f, SRH03 = 0.f, Shear06 = 0.f, STP = 0.f, SCP = 0.f, SHIP = 0.f;
	float Wet = 0.f, Snow = 0.f, HailCover = 0.f;
	float Soil = 0.4f, Elev = 0.f, Flood = 0.f, FlowU = 0.f, FlowV = 0.f, Eta = 0.f;
	float Dust = 0.f, AshFall = 0.f, AshDepth = 0.f, Blow = 0.f, FeelsLike = 15.f;
	uint8 bWater = 0;
	uint8 PType = 0, Phen = 0;
};

class AEROSWEATHER_API FAerosSnapshot
{
public:
	FAerosSimulationSettings Settings;   // grid + domain centre at publish time
	double SimTime = 0.0;
	uint64 Generation = 0;
	int32 NX = 0, NY = 0;
	float CellM = 2000.f;
	FVector DomainCenter = FVector::ZeroVector;

	TArray<FAerosSnapCell>        Cells;
	TArray<FAerosStormData>       Storms;
	TArray<FAerosCycloneData>     Cyclones;
	TArray<FAerosLightningStrike> Lightning;  // strikes of the last ~2 sim minutes
	FAerosDateTime                Clock;
	FAerosSynopticState           Synoptic;
	FAerosSimStats                Stats;

	bool IsValid() const { return NX > 0 && NY > 0 && Cells.Num() == NX * NY; }

	/** World (cm) -> continuous grid coords. Cell (i,j) spans [i, i+1). Returns false if outside. */
	bool WorldToGrid(const FVector& World, float& OutGX, float& OutGY) const;
	/** Continuous grid coords -> world (cm), Z = 0. */
	FVector GridToWorld(float GX, float GY) const;

	FORCEINLINE const FAerosSnapCell& At(int32 I, int32 J) const
	{
		I = FMath::Clamp(I, 0, NX - 1);
		J = FMath::Clamp(J, 0, NY - 1);
		return Cells[I + J * NX];
	}

	/** Bilinear sample at world location. Outside domain -> nearest edge cell. */
	FAerosWeatherSample Sample(const FVector& World) const;

	/** Blend two samples (temporal interpolation between snapshots). */
	static FAerosWeatherSample LerpSample(const FAerosWeatherSample& A, const FAerosWeatherSample& B, float T);

	/** Nearest-cell read (cheap). */
	const FAerosSnapCell* CellAtWorld(const FVector& World) const;

	static FVector SimToWorldDir(float X, float Y, float Z = 0.f) { return FVector(X, -Y, Z); }
	static FVector2D WorldToSimDir(const FVector& V) { return FVector2D(V.X, -V.Y); }
};

using FAerosSnapshotPtr = TSharedPtr<const FAerosSnapshot, ESPMode::ThreadSafe>;
