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

/**
 * Everything the custom cloud renderer needs from the simulation, sent once per frame from the
 * game thread. Same sources and units as the old cloud material parameters.
 */
struct FAerosCloudRenderParams
{
	FTextureResource* WeatherMap = nullptr;   // RGBA: cover, base / 5 km, top / 16 km, rain
	FTextureResource* ShapeMap = nullptr;     // RGBA: anvil cover, updraft, gust, convection
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
