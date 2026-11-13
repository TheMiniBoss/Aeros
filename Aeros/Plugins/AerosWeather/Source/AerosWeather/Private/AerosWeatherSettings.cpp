// Copyright Aeros. All Rights Reserved.

#include "AerosWeatherSettings.h"
#include "Materials/MaterialParameterCollection.h"

UAerosWeatherSettings::UAerosWeatherSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Aeros Weather");
	// Created by Content/Python/aeros_setup.py (missing asset = simply not used)
	WeatherParameterCollection = TSoftObjectPtr<UMaterialParameterCollection>(FSoftObjectPath(TEXT("/AerosWeather/Materials/MPC_AerosWeather.MPC_AerosWeather")));
}
