// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "WeatherTypes.h"

class AEROS_API UWeatherSimulationThread : public FRunnable
{
public:
    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    virtual void Exit() override;

protected:
    void ComputePrecipitationType(FWeatherCell& Cell);
    bool CheckTornadoFormation(const FWeatherCell& Cell, float WindShear);
    float CalculateSRH(float WindShear) const;

private:
    FRunnableThread* Thread;
    bool bRunning;
};
