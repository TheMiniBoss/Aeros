// Copyright Aeros. All Rights Reserved.
//
// AAerosStormSystem — roadmap "AStormSystem": the gameplay face of one simulated storm.
// Only the macro state replicates (NetUpdateFrequency 1 Hz). The server applies hail damage
// (ApplyRadialDamage + Chaos strain fields). Clients smooth the motion locally.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/DamageType.h"
#include "AerosTypes.h"
#include "AerosStormSystem.generated.h"

class UNiagaraComponent;
class UAudioComponent;
class UFieldSystemComponent;
class UPointLightComponent;

UCLASS()
class AEROSWEATHER_API UAerosHailDamageType : public UDamageType
{
	GENERATED_BODY()
};

UCLASS()
class AEROSWEATHER_API UAerosWindDamageType : public UDamageType
{
	GENERATED_BODY()
};

UCLASS()
class AEROSWEATHER_API UAerosLightningDamageType : public UDamageType
{
	GENERATED_BODY()
};

UCLASS(Blueprintable)
class AEROSWEATHER_API AAerosStormSystem : public AActor
{
	GENERATED_BODY()

public:
	AAerosStormSystem();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Server: refresh from the simulation. */
	void ServerUpdateFromSim(const FAerosStormData& Data);

	UFUNCTION(BlueprintPure, Category = "Aeros|Storm")
	FAerosStormData GetStormData() const { return StormData; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Storm")
	int32 GetStormId() const { return StormData.Id; }

	/** Blueprint hook: called on every machine when replicated data changes. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Aeros|Storm")
	void OnStormUpdated(const FAerosStormData& Data);

	UPROPERTY(ReplicatedUsing = OnRep_StormIntensity, BlueprintReadOnly, Category = "Aeros|Storm")
	float StormIntensity = 0.f;

	UPROPERTY(ReplicatedUsing = OnRep_StormData, BlueprintReadOnly, Category = "Aeros|Storm")
	FAerosStormData StormData;

	/** Altitude (cm) of the actor above the ground (cloud base). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Storm")
	float CloudBaseHeight = 150000.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Storm")
	TObjectPtr<UNiagaraComponent> StormVFX;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Storm")
	TObjectPtr<UAudioComponent> RumbleAudio;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Storm")
	TObjectPtr<UFieldSystemComponent> FieldSystem;

protected:
	UFUNCTION() void OnRep_StormIntensity();
	UFUNCTION() void OnRep_StormData();

	/** Roadmap: destructive hail (> 30 mm) -> radial damage + Chaos fractures. */
	void ProcessExtremeHailDamage();
	void TriggerChaosFracturesInRadius(const FVector& Center, float Radius, float Damage);

private:
	float DamageTimer = 0.f;
	FVector SmoothedLocation = FVector::ZeroVector;
	bool bHasLocation = false;
};

/** Short-lived cosmetic flash + thunder for a lightning strike (local only). */
UCLASS(NotPlaceable, Transient)
class AEROSWEATHER_API AAerosLightningFlash : public AActor
{
	GENERATED_BODY()

public:
	AAerosLightningFlash();
	virtual void Tick(float DeltaSeconds) override;

	void Fire(const FAerosLightningStrike& Strike, const FVector& ViewerLocation);

	UPROPERTY(VisibleAnywhere, Category = "Aeros")
	TObjectPtr<UPointLightComponent> Light;

private:
	float Age = 0.f;
	float Peak = 0.f;
	float ThunderDelay = -1.f;
	float ThunderVolume = 1.f;
	bool bThunderPlayed = true;
};
