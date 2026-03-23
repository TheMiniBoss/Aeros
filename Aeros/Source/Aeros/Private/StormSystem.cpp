// Fill out your copyright notice in the Description page of Project Settings.


#include "StormSystem.h"
#include "Components/PrimitiveComponent.h"
#include "NiagaraComponent.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include <Kismet/GameplayStatics.h>

// Sets default values
AStormSystem::AStormSystem()
{
	bReplicates = true;
	NetUpdateFrequency = 2.0f;
	NetCullDistanceSquared = 10000000000.0f;

	StormVFX = CreateDefaultSubobject<UNiagaraComponent>("StormVFX");
	RootComponent = StormVFX;
}

// Called when the game starts or when spawned
void AStormSystem::BeginPlay()
{
	Super::BeginPlay();
	
	if (HasAuthority())
	{
		GetWorldTimerManager().SetTimer(DamageTimerHandle, this, &AStormSystem::ProcessExtremeHailDamage, 3.0f, true);
	}
}

// Called every frame
void AStormSystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void AStormSystem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AStormSystem, CurrentStormType);
	DOREPLIFETIME(AStormSystem, Intensity);
	DOREPLIFETIME(AStormSystem, HailDiameter);

}

void AStormSystem::OnRep_StormData()
{
	if (StormVFX)
	{
		StormVFX->SetFloatParameter(FName("User.HailDiameter"), HailDiameter);
		StormVFX->SetFloatParameter(FName("User.RainIntensity"), Intensity);
	}
}

void AStormSystem::ProcessExtremeHailDamage()
{
	if (HailDiameter > 30.0f)
	{
		float DamageRadius = 5000.0f;
		float DamageAmount = HailDiameter * 0.5f;

		TArray<AActor*> IgnoredActors;
		UGameplayStatics::ApplyRadialDamage(this, DamageAmount, GetActorLocation(), DamageRadius, UDamageType::StaticClass(), IgnoredActors, this, nullptr, true);
	}
}

