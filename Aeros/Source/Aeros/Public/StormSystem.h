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

	UPROPERTY(ReplicatedUsing = OnRep_StormData)
	float Intensity = 0.0f;

	UPROPERTY(ReplicatedUsing = OnRep_StormData)
	EStormType CurrentStormType = EStormType::None;

	UPROPERTY(ReplicatedUsing = OnRep_StormData)
	float HailDiameter = 0.0f;

protected:
	FTimerHandle DamageTimerHandle;
	
public:	
	AStormSystem();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	void ProcessExtremeHailDamage();
	UFUNCTION() void OnRep_StormData();
};
