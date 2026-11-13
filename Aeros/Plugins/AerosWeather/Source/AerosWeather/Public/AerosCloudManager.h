// Copyright Aeros. All Rights Reserved.
//
// UAerosCloudManager - roadmap "UCloudManager": turns the grid into GPU data.
//  * Weather map (RGBA8, one texel per cell): R cloud cover, G cloud base (0-5 km),
//    B cloud top (0-16 km), A precipitation (sqrt-scaled 0-100 mm/h).
//    Sample it in a Volumetric Cloud material with:
//      UV = (WorldXY - DomainCenter) / DomainSize + 0.5
//  * Surface map (RGBA8): R wetness, G snow depth (0-50 cm), B hail cover, A temperature (-40..50 C).
//  * Material Parameter Collection (optional): local values at the viewer for wet / snowy /
//    hail-covered materials, wind, lightning flashes. See README for the parameter names.
//  * Cloud shape map (RGBA8): R anvil cover (spread downwind of deep storms by the 500 hPa wind),
//    G updraft strength (0-30 m/s), B gust front / shelf cloud (surface convergence next to rain),
//    A convective activity. Lets the cloud material draw anvils, towers and shelf clouds.
// Weather / surface / hazard maps are blended in time between two snapshots, so clouds move smoothly.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/AerosCloudVolume.h"
#include "Tasks/Task.h"
#include "AerosCloudManager.generated.h"

class UAerosWeatherSubsystem;
class UTexture2D;
class UMaterialParameterCollection;
class UVolumeTexture;

UCLASS()
class AEROSWEATHER_API UAerosCloudManager : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UAerosWeatherSubsystem* InOwner);
	void Shutdown();
	void Tick(float DeltaTime);

	UTexture2D* GetWeatherMap() const { return WeatherMap; }
	UTexture2D* GetSurfaceMap() const { return SurfaceMap; }
	/** RGBA8: R flood depth (sqrt, 0-5 m), G dust (0-50 g/m2), B ash on ground (sqrt, 0-100 mm), A sea anomaly (-5..5 m). */
	UTexture2D* GetHazardMap() const { return HazardMap; }
	/** RGBA8: R anvil cover, G updraft (0-30 m/s), B gust front / shelf cloud, A convective activity. */
	UTexture2D* GetCloudShapeMap() const { return ShapeMap; }

	/** Debug: writes every channel of the weather and cloud shape maps as greyscale PNGs
	 *  (Saved/AerosDebug/<Prefix>_<channel>.png, north up). */
	void DumpMaps(const FString& Prefix) const;

	/** 3D cloud grid around the viewer (FAerosCloudVolume), double buffered. Index 0 = previous, 1 = current. */
	UVolumeTexture* GetCloudVolume(int32 Index) const { return CloudVolumes[Index == 0 ? 1 - CurVolume : CurVolume]; }
	/** (CenterX cm, CenterY cm, Size cm, Height km) of the previous (0) or current (1) grid. */
	FVector4 GetCloudVolumeRect(int32 Index) const;
	/** 0 = previous grid, 1 = current grid (ramps over 1 s after each upload). */
	float GetCloudVolumeBlend() const { return VolumeBlend; }
	/** 0 after a jump of more than 40 km until the grid at the new place is uploaded, else 1. */
	float GetCloudVolumeWeight() const { return VolumeWeight; }
	/** Debug: grid slices as PNGs (horizontal at 2 km and 10 km, vertical west-east through the centre). */
	void DumpVolumeSlices(const FString& Prefix) const;

	/** Cloud drift (km, Unreal XY) accumulated from the mid-level wind: offsets the 3D cloud noise. */
	FVector GetCloudNoiseOffsetKm() const { return NoiseOffsetKm; }

	/** Current lightning flash brightness at the viewer (0-1). */
	float GetLightningFlash() const { return Flash; }

	/** World location of the brightest strike this frame (the clouds glow around it). */
	FVector GetLightningLocation() const { return FlashLocation; }

	/** Lightning actors report their current brightness seen from the viewer (0-1) and position, every frame. */
	void ReportLightningFlash(float Value, const FVector& Location)
	{
		if (Value > PendingFlash) { PendingFlash = Value; PendingLocation = Location; }
	}

	/** Textures refresh rate (Hz). */
	UPROPERTY(EditAnywhere, Category = "Aeros")
	float TextureUpdateRate = 20.f;

private:
	void EnsureTextures(int32 NX, int32 NY);
	void UpdateTextures();
	void UpdateParameterCollection(float DeltaTime);
	void UpdateShapeMap(const class FAerosSnapshot& Snap);
	void UpdateCloudVolume(float DeltaTime);
	static void Upload(UTexture2D* Texture, const TArray<FColor>& Pixels, int32 W, int32 H);

	UPROPERTY(Transient) TObjectPtr<UAerosWeatherSubsystem> Owner;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> WeatherMap;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> SurfaceMap;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> HazardMap;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> ShapeMap;
	UPROPERTY(Transient) TObjectPtr<UVolumeTexture> CloudVolumes[2];
	UPROPERTY(Transient) TObjectPtr<UMaterialParameterCollection> Collection;

	TArray<FColor> WeatherPixels;
	TArray<FColor> SurfacePixels;
	TArray<FColor> HazardPixels;
	TArray<FColor> ShapePixels;
	TArray<float> AnvilAcc;
	uint64 ShapeGeneration = MAX_uint64;
	float ShapeTimer = 0.f;
	FVector NoiseOffsetKm = FVector::ZeroVector;

	// Voxel grid: one build at a time on a worker task, uploaded into the older texture
	FAerosCloudVolumeDesc VolumeDescs[2];
	int32 CurVolume = 1;
	float VolumeBlend = 1.f;
	float VolumeWeight = 0.f;
	float VolumeTimer = 0.f;
	uint64 VolumeGeneration = MAX_uint64;
	bool bVolumeBuilding = false;
	FAerosCloudVolumeDesc BuildingDesc;
	TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> BuildingData;
	TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> CurVolumeData; // CPU copy of the current grid (debug slices)
	UE::Tasks::FTask VolumeTask;
	int32 TexW = 0, TexH = 0;
	float Timer = 0.f;
	float Flash = 0.f;
	float PendingFlash = 0.f;
	FVector PendingLocation = FVector::ZeroVector;
	FVector FlashLocation = FVector(0, 0, -1.0e9);
};
