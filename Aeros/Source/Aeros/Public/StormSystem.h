// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeatherTypes.h"
#include "StormSystem.generated.h"

class UNiagaraComponent;

UCLASS()
class AEROS_API AStormSystem : public AActor
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UNiagaraComponent> StormVFX = nullptr;

	UPROPERTY()
	float StormIntensity = 0.0f;

	UPROPERTY()
	int StormType = 0;

	UPROPERTY()
	FWeatherCell CurrentCellData;
	
public:	
	AStormSystem();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	void ProcessExtremeHailDamage();
};
