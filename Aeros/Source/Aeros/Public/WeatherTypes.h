// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "WeatherTypes.generated.h"

USTRUCT()
struct FWeatherCell
{
	GENERATED_BODY()

	UPROPERTY(meta = (Units = "Celsius"))					float Temperature;
	UPROPERTY(meta = (ClampMin = "0.0", ClampMax = "1.0"))	float Humidity; // 0.0-1.0
	UPROPERTY(meta = (ForceUnits = "hPa"))					float Pressure;
	UPROPERTY()												FVector2D WindVector; // Speed and Direction
	UPROPERTY(meta = (ForceUnits = "mm/h"))					float CAPE; // Convective Available Potential Energy
	UPROPERTY()												float PrecipitationIntensity;
	UPROPERTY(meta = (Units = "MetersPerSecond"))			float UpdraftVelocity;
	UPROPERTY(meta = (Units = "Meters"))					float FreezingLevelAltitude;
	UPROPERTY(meta = (Units = "Millimeters"))				float HailstoneDiameter; // 0 = no Hail

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
