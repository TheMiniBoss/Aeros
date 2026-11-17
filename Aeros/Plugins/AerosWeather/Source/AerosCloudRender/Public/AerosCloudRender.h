// Copyright Aeros. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ContainerAllocationPolicies.h"

class FTextureResource;
class FTextureRenderTargetResource;

/** One lightning flash lighting the clouds this frame. */
struct FAerosCloudFlash
{
	FVector3f PosKm = FVector3f::ZeroVector;   // world, Unreal axes, km
	float Intensity = 0.f;                     // 0-1, as reported by the lightning actors
};

/** One rotating storm drawn by the renderer: its wall cloud (a lowered base turning under the updraft) and the
 *  striations of its rotating updraft, both growing with the mesocyclone (the voxel grid has the rest). */
struct FAerosCloudStorm
{
	FVector3f UpdraftKm = FVector3f::ZeroVector;  // world km, Unreal axes, z = ground height (as the voxel grid)
	float BaseKm = 1.f;                           // cloud base above the ground
	float TopKm = 10.f;                           // top above the ground
	float TowerRadiusKm = 3.f;
	FVector2f LeanKm = FVector2f::ZeroVector;     // tower lean per km of height (xy)
	FVector2f WallKm = FVector2f::ZeroVector;     // wall cloud centre, world km xy
	float WallRadiusKm = 2.f;
	float Rotation = 0.f;                         // 0-1, organisation of the mesocyclone
	float SpinPhase = 0.f;                        // radians, wrapped (sign = rotation sense)
	float Striation = 0.f;                        // 0-1: LP strongly striated, HP hardly (hidden in rain)
	float Fade = 1.f;                             // 0-1, fades in / out of the rendered set
};

/** One tornado drawn inside the clouds (condensation funnel, wall cloud, dust whirl), see Core/AerosFunnel.h. */
struct FAerosCloudTornado
{
	FVector3f GroundKm = FVector3f::ZeroVector;   // world km, Unreal axes, z = ground height
	float CloudBaseKm = 1.f;                      // above the ground
	FVector4f AxisKm = FVector4f(0.f, 0.f, 0.f, 0.f);   // axis = cubic Bezier in xy over t = h / base: P1.xy, P2.xy (offsets, km)
	FVector2f TopKm = FVector2f::ZeroVector;      // P3.xy offset (P0 = the ground point)
	float VmaxMs = 0.f;
	float CoreRadiusM = 80.f;
	float K = 1.f;
	float RopeOut = 0.f;                          // 0-1
	float SpinPhase = 0.f;                        // radians, wrapped (sign = rotation sense)
	int32 NumSubVortices = 0;
	float SubPhase = 0.f;
	FVector3f DustTint = FVector3f(0.45f, 0.38f, 0.3f);
	float Fade = 1.f;                             // 0-1, fades in / out of the rendered set
};

/**
 * Everything the custom cloud renderer needs from the simulation, sent once per frame from the
 * game thread. Same sources and units as the old cloud material parameters.
 */
struct FAerosCloudRenderParams
{
	FTextureResource* WeatherMap = nullptr;   // RGBA: cover, base / 5 km, top / 16 km, rain
	FTextureResource* ShapeMap = nullptr;     // RGBA: anvil cover, updraft, gust, convection
	FTextureResource* LayerMapA = nullptr;    // RGBA: mid cover, mid base / 10 km, mid form (As 0 .. Ac 1), low form (St 0 .. Sc 0.5 .. Cu 1)
	FTextureResource* LayerMapB = nullptr;    // RGBA: high cover, high base / 16 km, high form (Cs 0 .. Ci 0.5 .. Cc 1), mid top / 10 km
	FVector2f HighWindDir = FVector2f(1.f, 0.f);   // world xy, unit: cirrus streaks along the upper wind
	FTextureResource* VolumePrev = nullptr;   // RG8 voxel grids (R density, G darkness / rain)
	FTextureResource* VolumeCur = nullptr;
	FVector4f DomainCm = FVector4f(0.f, 0.f, 32000000.f, 32000000.f);   // xy centre, zw size (cm)
	FVector4f RectPrevCm = FVector4f(0.f, 0.f, 8000000.f, 16.f);        // xy centre (cm), z size (cm), w height (km)
	FVector4f RectCurCm = FVector4f(0.f, 0.f, 8000000.f, 16.f);
	float VolumeBlend = 1.f;
	float VolumeWeight = 0.f;
	FVector3f NoiseOffsetKm = FVector3f::ZeroVector;
	FVector3f MotionKmS = FVector3f::ZeroVector;   // mean storm motion per real second (reprojection)
	FVector3f DriftKmS = FVector3f::ZeroVector;    // drift of the cumulus field / noise per real second (reprojection)
	TArray<FAerosCloudFlash, TInlineAllocator<8>> Flashes;
	// Cloud shadow map (R16F transmittance towards the sun, UAV): rendered every 4th frame and when
	// the rect moves; the sun's light function M_AerosCloudShadow reads it over the same rect.
	TArray<FAerosCloudTornado, TInlineAllocator<4>> Tornadoes;   // at most 4, nearest to the viewer
	TArray<FAerosCloudStorm, TInlineAllocator<4>> Storms;        // rotating storms, at most 4, nearest to the viewer
	FTextureRenderTargetResource* ShadowTarget = nullptr;
	FVector4f ShadowRectCm = FVector4f(0.f, 0.f, 10000000.f, 10000000.f);   // xy centre, zw size (cm)
};

namespace AerosCloudRender
{
	/** Game thread: hands the latest parameters to the renderer (copied to the render thread). */
	AEROSCLOUDRENDER_API void SetParams(const FAerosCloudRenderParams& Params);

	/** True when r.AerosClouds selects the custom renderer (the engine VolumetricCloud must be hidden). */
	AEROSCLOUDRENDER_API bool IsEnabled();
}
