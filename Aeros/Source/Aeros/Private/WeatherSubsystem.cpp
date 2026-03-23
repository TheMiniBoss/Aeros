// Fill out your copyright notice in the Description page of Project Settings.


#include "WeatherSubsystem.h"

void UWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
}

void UWeatherSubsystem::Deinitialize()
{
}

float UWeatherSubsystem::GetTemperatureAtLocation(FVector Location) const
{
    return 0.0f;
}

void UWeatherSubsystem::OnWeatherTick()
{
}

void UWeatherSubsystem::UpdateWeatherRenderTargets()
{
    //TODO : Update RT texture to the GPU (Clouds, Snow)
}
