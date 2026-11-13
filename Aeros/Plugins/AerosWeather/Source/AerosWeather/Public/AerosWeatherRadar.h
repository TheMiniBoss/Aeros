// Copyright Aeros. All Rights Reserved.
//
// AAerosWeatherRadar - roadmap "AWeatherRadar": a rotating Doppler radar that scans the
// simulation and paints a texture for the UI (green = rain, red = intense, magenta = hail /
// tornado). The beam sweeps like a real radar: only the pixels it crosses are refreshed.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AerosWeatherRadar.generated.h"

class UTexture2D;

UENUM(BlueprintType)
enum class EAerosRadarProduct : uint8
{
	Reflectivity  UMETA(DisplayName = "Base Reflectivity (dBZ)"),
	Velocity      UMETA(DisplayName = "Radial Velocity (m/s)"),
	PrecipType    UMETA(DisplayName = "Precipitation Type"),
	Hazards       UMETA(DisplayName = "Hazards (flood, dust, ash, sea level)"),
};

UCLASS(Blueprintable)
class AEROSWEATHER_API AAerosWeatherRadar : public AActor
{
	GENERATED_BODY()

public:
	AAerosWeatherRadar();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintPure, Category = "Aeros|Radar")
	UTexture2D* GetRadarTexture() const { return RadarTexture; }

	/** Current beam azimuth, degrees clockwise from north. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Radar")
	float GetBeamAzimuth() const { return BeamAzimuth; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Radar")
	FVector GetRadarLocation() const;

	UFUNCTION(BlueprintCallable, Category = "Aeros|Radar")
	void SetProduct(EAerosRadarProduct InProduct);

	/** NWS-style reflectivity colour scale. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Radar")
	static FLinearColor ReflectivityColor(float DBZ);

	/** World location -> radar UV (0-1). Returns false if out of range. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Radar")
	bool WorldToRadarUV(const FVector& World, FVector2D& OutUV) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Radar", meta = (ClampMin = "10", ClampMax = "500"))
	float RangeKm = 160.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Aeros|Radar", meta = (ClampMin = "64", ClampMax = "1024"))
	int32 Resolution = 256;

	/** Antenna rotation speed (deg/s, real time). Real WSR-88D: ~18-30 deg/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Radar")
	float SweepDegreesPerSecond = 120.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Radar")
	EAerosRadarProduct Product = EAerosRadarProduct::Reflectivity;

	/** Keep the radar centred on the local viewer (handheld / vehicle radar). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Radar")
	bool bFollowViewer = true;

private:
	void EnsureTexture();
	void ScanSector(float FromDeg, float ToDeg);
	FColor PixelColor(const class FAerosSnapshot& Snap, const FVector& World) const;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> RadarTexture;

	TArray<FColor> Pixels;
	TArray<float> PixelAzimuth;  // degrees from north, -1 outside range
	float BeamAzimuth = 0.f;
	float UploadTimer = 0.f;
	FVector ScanCenter = FVector::ZeroVector;
};
