// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WeatherTypes.h"
#include "WeatherSubsystem.generated.h"


UCLASS()
class AEROS_API UWeatherSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Weather")
	float GetTemperatureAtLocation(FVector Location) const;

protected:
	FTimerHandle WeatherTickHandle;
	void OnWeatherTick();

	TArray<FWeatherCell> AtmosphereGrid;

	int32 GridSizeX = 100;
	int32 GridSizeY = 100;
	float CellSize = 10000.f; // 10km per cellule

	void UpdateWeatherRenderTargets();
};
