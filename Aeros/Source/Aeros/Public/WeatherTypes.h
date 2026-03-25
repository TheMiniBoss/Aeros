// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "WeatherTypes.generated.h"

UENUM(BlueprintType)
enum class EStormType : uint8
{
	None			UMETA(DisplayName = "None"),
	Thunderstorm	UMETA(DisplayName = "Thunderstorm"),
	Supercell		UMETA(DisplayName = "Supercell"),
	Tornado			UMETA(DisplayName = "Tornado"),
	Hurricane		UMETA(DisplayName = "Hurricane")
};

UENUM(BlueprintType)
enum class ERadarMode : uint8
{
	Reflectivity    UMETA(DisplayName = "Réflectivité (Pluie & Grêle)"),
	Velocity        UMETA(DisplayName = "Vélocité Doppler (Vent)"),
	Instability     UMETA(DisplayName = "Thermodynamique (CAPE)")
};

USTRUCT(BlueprintType)
struct FWeatherCell
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Thermodynamics", meta = (Units = "Celsius"))					float Temperature;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Thermodynamics", meta = (ClampMin = "0.0", ClampMax = "1.0"))	float Humidity; // 0.0-1.0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Thermodynamics", meta = (ForceUnits = "hPa"))					float Pressure;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Wind")															FVector2D WindVector; // Speed and Direction m/s
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Energy", meta = (ForceUnits = "mm/h"))							float CAPE; // Convective Available Potential Energy
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Vertical", meta = (Units = "MetersPerSecond"))					float UpdraftVelocity;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Precipitation")													float PrecipitationIntensity; // J/kg
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Precipitation", meta = (Units = "Meters"))						float FreezingLevelAltitude;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Precipitation", meta = (Units = "Millimeters"))					float HailstoneDiameter; // 0 = no Hail

	FWeatherCell()
	{
		Temperature = 15.0f;
		Humidity = 0.5f;
		Pressure = 1013.0f;
		WindVector = FVector2D::ZeroVector;
		CAPE = 0.0f;
		PrecipitationIntensity = 0.0f;
		UpdraftVelocity = 0.0f;
		FreezingLevelAltitude = 2500.0f;
		HailstoneDiameter = 0.0f;
	}
};
