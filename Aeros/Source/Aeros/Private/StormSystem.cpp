// Fill out your copyright notice in the Description page of Project Settings.


#include "StormSystem.h"
#include "Components/PrimitiveComponent.h"
#include "NiagaraComponent.h"
#include <Kismet/GameplayStatics.h>

// Sets default values
AStormSystem::AStormSystem()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	StormVFX = CreateDefaultSubobject<UNiagaraComponent>("StormVFX");
	RootComponent = StormVFX;
}

// Called when the game starts or when spawned
void AStormSystem::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void AStormSystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void AStormSystem::ProcessExtremeHailDamage()
{
	if (CurrentCellData.HailstoneDiameter > 30.0f)
	{
		float DamageRadius = 5000.0f;
		float DamageAmount = CurrentCellData.HailstoneDiameter * 0.5f;

		TArray<AActor*> IgnoredActors;
		UGameplayStatics::ApplyRadialDamage(this, DamageAmount, GetActorLocation(), DamageRadius, UDamageType::StaticClass(), IgnoredActors, this, nullptr, true);

		//TriggerChaosFracturesInRadius(DamageRadius, DamageAmount); TODO Chao Fracture
	}
}

