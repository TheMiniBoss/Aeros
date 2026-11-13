// Copyright Aeros. All Rights Reserved.

#include "AerosWeatherModule.h"

DEFINE_LOG_CATEGORY(LogAeros);

void FAerosWeatherModule::StartupModule()
{
	UE_LOG(LogAeros, Log, TEXT("Aeros Weather module started."));
}

void FAerosWeatherModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FAerosWeatherModule, AerosWeather)
