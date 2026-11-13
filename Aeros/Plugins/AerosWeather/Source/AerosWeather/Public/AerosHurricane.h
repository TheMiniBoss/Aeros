// Copyright Aeros. All Rights Reserved.
//
// AAerosHurricane - roadmap "Ouragan": macroscopic entity, cyclonic flow around a low-pressure eye.
// The wind / rain / pressure fields live in the simulation (Holland vortex). This actor is the
// replicated, gameplay-facing handle (category, eye position, events, optional VFX).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AerosTypes.h"
#include "AerosHurricane.generated.h"

class UAudioComponent;
class UNiagaraComponent;

UCLASS(Blueprintable)
class AEROSWEATHER_API AAerosHurricane : public AActor
{
	GENERATED_BODY()

public:
	AAerosHurricane();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	void ServerUpdateFromSim(const FAerosCycloneData& Data);

	UFUNCTION(BlueprintImplementableEvent, Category = "Aeros|Hurricane")
	void OnCategoryChanged(int32 OldCategory, int32 NewCategory);

	UPROPERTY(ReplicatedUsing = OnRep_Cyclone, BlueprintReadOnly, Category = "Aeros|Hurricane")
	FAerosCycloneData CycloneData;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Hurricane")
	TObjectPtr<UAudioComponent> WindAudio;

protected:
	UFUNCTION() void OnRep_Cyclone();

private:
	int32 LastCategory = -1;
	FVector Smoothed = FVector::ZeroVector;
	bool bHasLocation = false;
};
