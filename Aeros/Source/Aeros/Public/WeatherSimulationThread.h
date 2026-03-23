// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "WeatherTypes.h"

class FRunnableThread;
class FEvent;

class AEROS_API FWeatherSimulationThread : public FRunnable
{
    FRunnableThread* Thread;

    // Sémaphore utilisé pour endormir/réveiller le thread proprement
    FEvent* WaitEvent;

    // Variables Thread-Safe
    FThreadSafeBool bStopThread;
    FThreadSafeBool bIsCalculating;

    int GridSizeX;
    int GridSizeY;
    float CellSize;

    // Pointeurs vers les buffers du Subsystem
    TArray<FWeatherCell>* CurrentReadGrid;
    TArray<FWeatherCell>* CurrentWriteGrid;

public:
    FWeatherSimulationThread(int InGridSizeX, int InGridSizeY, float InCellSize);
    virtual ~FWeatherSimulationThread();

    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    virtual void Exit() override;

    void TriggerCalculation(TArray<FWeatherCell>* InReadGrid, TArray<FWeatherCell>* InWriteGrid);
    bool IsCalculating() const { return bIsCalculating; }

private:
    void PerformSimulationTick();
    void ComputeThermodynamics(int Index);
    void ComputeAdvection(int x, int y, int Index);
};
