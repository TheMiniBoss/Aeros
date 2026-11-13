// Copyright Aeros. All Rights Reserved.
//
// FAerosCloudVolume - pure C++ (no UObject): turns a simulation snapshot into a 3D cloud density
// grid around the viewer, like the voxel clouds of Nubis 3. The simulation is 2.5D, so the 3D
// anatomy of every storm is synthesised from its physical fields:
//   tower (LCL -> EL, leaning downshear), overshooting top, anvil spread by the 500 hPa wind with
//   mammatus underneath, precipitation core, wall cloud, flanking line, shelf cloud on the gust
//   front, and stratiform decks. Fair-weather cumulus are left to the cloud shader (finer than voxels).
//
// Layout of the RG8 output: index = 2 * (X + Y * NX + Z * NX * NY).
//   X grows with world +X (east), Y with world +Y (south), Z with altitude.
//   R = density 0-255, G = darkness 0-255 (precipitation, deep towers).

#pragma once

#include "CoreMinimal.h"

class FAerosSnapshot;

struct AEROSWEATHER_API FAerosCloudVolumeDesc
{
	int32 NX = 256, NY = 256, NZ = 48;
	float SizeKm = 80.f;    // horizontal size (square)
	float HeightKm = 16.f;  // from the ground (z = 0) up
	FVector Center = FVector::ZeroVector; // world cm, Z ignored

	float VoxelKmXY() const { return SizeKm / NX; }
	float VoxelKmZ() const { return HeightKm / NZ; }
};

class AEROSWEATHER_API FAerosCloudVolume
{
public:
	/** Fills OutRG8 (resized to NX * NY * NZ * 2). Safe to call from a worker thread. */
	static void Build(const FAerosSnapshot& Snap, const FAerosCloudVolumeDesc& Desc, TArray<uint8>& OutRG8);

	/** Grid centre for a viewer: snapped to StepKm so the grid does not swim with the camera. */
	static FVector SnapCenter(const FVector& Viewer, float StepKm = 10.f);

	/** Nearest-voxel density (0-1) at a world location and altitude (km). 0 outside the grid. */
	static float SampleDensity(const TArray<uint8>& RG8, const FAerosCloudVolumeDesc& Desc, const FVector& World, float AltKm);

	/** Gust-front strength (0-1) of a simulation cell: surface convergence next to heavy rain (shelf clouds). */
	static float GustFront(const FAerosSnapshot& Snap, int32 I, int32 J);
};
