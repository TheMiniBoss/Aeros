// Fill out your copyright notice in the Description page of Project Settings.


#include "WeatherSimulationThread.h"

bool UWeatherSimulationThread::Init()
{
	return false;
}

uint32 UWeatherSimulationThread::Run()
{
	return uint32();
}

void UWeatherSimulationThread::Stop()
{
}

void UWeatherSimulationThread::Exit()
{
}

void UWeatherSimulationThread::ComputePrecipitationType(FWeatherCell& Cell)
{
	if (Cell.PrecipitationIntensity > 0.0f && Cell.CAPE > 100.0f)
	{
		// Speed of the updraft derivated of CAPE
		Cell.UpdraftVelocity = FMath::Sqrt(2.0f * Cell.CAPE) * 0.5f;

		if (Cell.UpdraftVelocity > 15.0f) // More than 15 m/s = hail risk
		{
			Cell.HailstoneDiameter = (Cell.UpdraftVelocity - 15.0f) * 2.5f;
		}
		else
		{
			Cell.HailstoneDiameter = 0.0f;
		}
	}
}

bool UWeatherSimulationThread::CheckTornadoFormation(const FWeatherCell& Cell, float WindShear)
{
	float SRH = CalculateSRH(WindShear);
	if (Cell.CAPE > 2500.0f && SRH > 250.0f) return FMath::FRand() > 0.85f; // 15% if condition crtical
	return false;
}

float UWeatherSimulationThread::CalculateSRH(float WindShear) const
{
	const float k = 0.5f;  // coefficient example
	float SRH = WindShear * k;
	return SRH;
}
