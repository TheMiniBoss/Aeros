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
// Both textures are blended in time between two snapshots, so clouds move smoothly.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AerosCloudManager.generated.h"

class UAerosWeatherSubsystem;
class UTexture2D;
class UMaterialParameterCollection;

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
	static void Upload(UTexture2D* Texture, const TArray<FColor>& Pixels, int32 W, int32 H);

	UPROPERTY(Transient) TObjectPtr<UAerosWeatherSubsystem> Owner;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> WeatherMap;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> SurfaceMap;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> HazardMap;
	UPROPERTY(Transient) TObjectPtr<UMaterialParameterCollection> Collection;

	TArray<FColor> WeatherPixels;
	TArray<FColor> SurfacePixels;
	TArray<FColor> HazardPixels;
	int32 TexW = 0, TexH = 0;
	float Timer = 0.f;
	float Flash = 0.f;
	float PendingFlash = 0.f;
	FVector PendingLocation = FVector::ZeroVector;
	FVector FlashLocation = FVector(0, 0, -1.0e9);
};
