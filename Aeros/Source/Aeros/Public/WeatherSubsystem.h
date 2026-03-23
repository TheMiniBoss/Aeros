// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WeatherTypes.h"
#include "WeatherSimulationThread.h"
#include "Engine/Texture2D.h"
#include "WeatherSubsystem.generated.h"


UCLASS()
class AEROS_API UWeatherSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadOnly, Category = "Weather|Radar")
	UTexture2D* RadarTexture;

protected:
	FTimerHandle WeatherTickHandle;

	TArray<FWeatherCell> GridBufferA;
	TArray<FWeatherCell> GridBufferB;

	bool bIsUsingBufferA = true; // indicate current buffer readed by GameThread
	class FWeatherSimulationThread* SimulationThread = nullptr;

	int32 GridSizeX = 128;
	int32 GridSizeY = 128;
	float CellSize = 1000.f; // 1km per cellule

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Weather|Query")
	bool GetTemperatureAtLocation(FVector Location, FWeatherCell& OutWeatherData) const;

protected:
	void OnWeatherTick();
	void UpdateRadarTexture();

};
