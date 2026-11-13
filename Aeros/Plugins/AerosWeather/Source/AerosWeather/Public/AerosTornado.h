// Copyright Aeros. All Rights Reserved.
//
// AAerosTornado — spawned by the storm manager under a mesocyclone (roadmap: CAPE > 2500,
// strong helicity, ~15% chance per check). Sub-grid phenomenon: it lives in real time,
// not in simulation time, so gameplay stays readable whatever the weather time scale.
//
// Wind model: Rankine combined vortex + radial inflow + updraft.
//   r <  Rc : Vt = Vmax * r / Rc
//   r >= Rc : Vt = Vmax * Rc / r

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AerosTornado.generated.h"

class UNiagaraComponent;
class UAudioComponent;
class UFieldSystemComponent;
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;

UCLASS(Blueprintable)
class AEROSWEATHER_API AAerosTornado : public AActor
{
	GENERATED_BODY()

public:
	AAerosTornado();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Server: configure after spawn. */
	void ServerInitialize(int32 InStormId, float InMaxWind, float InCoreRadiusM, float InLifetime, const FVector& InVelocity, int32 InSeed);

	/** Wind (world, m/s) induced by this tornado at a location. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	FVector GetWindAt(const FVector& WorldLocation) const;

	/** 0 -> 1 -> 0 over the life (spin-up, mature, rope-out). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	float GetStrength() const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	int32 GetEFRating() const { return EFRating; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	float GetCoreRadius() const { return CoreRadius; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	int32 GetParentStormId() const { return ParentStormId; }

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") int32 ParentStormId = -1;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") int32 EFRating = 0;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") float MaxWind = 50.f;        // m/s
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") float CoreRadius = 80.f;     // m
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") float Lifetime = 300.f;      // real s
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") float Age = 0.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") FVector TrackVelocity = FVector::ZeroVector; // m/s world
	UPROPERTY(Replicated) int32 Seed = 0;

	/** Physics push strength multiplier (gameplay tuning). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Tornado")
	float ForceScale = 1.f;

	/** Cap on acceleration applied to physics bodies (m/s2), avoids instabilities. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Tornado")
	float MaxAcceleration = 60.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Tornado")
	TObjectPtr<UNiagaraComponent> FunnelVFX;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Tornado")
	TObjectPtr<UAudioComponent> RoarAudio;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Tornado")
	TObjectPtr<UFieldSystemComponent> FieldSystem;

	/** Fallback visuals (no Niagara asset): funnel cone + orbiting debris. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Tornado")
	TObjectPtr<UStaticMeshComponent> FallbackFunnel;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Tornado")
	TObjectPtr<UInstancedStaticMeshComponent> FallbackDebris;

protected:
	void ApplyPhysicalEffects(float DeltaSeconds);
	void UpdateFallbackVisuals(float DeltaSeconds);
	void UpdateGround();

private:
	struct FDebris { float RadiusFrac, Height, Phase, Speed, Scale; };
	TArray<FDebris> Debris;
	TArray<FTransform> DebrisTransforms;
	float PhysicsTimer = 0.f;
	float GroundTimer = 0.f;
	float GroundZ = 0.f;
	float VisualTime = 0.f;
	bool bUseFallback = false;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FunnelMID;
};
