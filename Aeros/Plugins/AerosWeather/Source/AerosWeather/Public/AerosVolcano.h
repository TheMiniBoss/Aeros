// Copyright Aeros. All Rights Reserved.
//
// AAerosVolcano - place it on a mountain (or let it build a fallback cone).
// Cycle: Dormant -> Unrest (earthquake swarm) -> Eruption -> Waning -> Dormant.
// Eruption size follows the Volcanic Explosivity Index (VEI 1-7):
//   * ash column injected into the simulation (mass eruption rate ~ 10^(VEI+2.5) kg/s, plume
//     height from Mastin et al. 2009) -> ash fall, darker skies, volcanic lightning, pyro-convection;
//   * lava flows that follow the real terrain downhill (line traces), burning what they touch;
//   * ballistic volcanic bombs; pyroclastic density currents for VEI >= 4.
// Server authoritative; state replicates, visuals are local.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AerosVolcano.generated.h"

class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UPointLightComponent;
class UNiagaraComponent;
class UAudioComponent;
class UFieldSystemComponent;
class UMaterialInstanceDynamic;

UENUM(BlueprintType)
enum class EAerosVolcanoState : uint8
{
	Dormant,
	Unrest,
	Erupting,
	Waning,
};

UCLASS(Blueprintable, meta = (DisplayName = "Aeros Volcano"))
class AEROSWEATHER_API AAerosVolcano : public AActor
{
	GENERATED_BODY()

public:
	AAerosVolcano();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Server: start an eruption (goes through a short unrest phase first). */
	UFUNCTION(BlueprintCallable, Category = "Aeros|Volcano")
	void StartEruption(int32 VEI = 3);

	UFUNCTION(BlueprintCallable, Category = "Aeros|Volcano")
	void StopEruption();

	UFUNCTION(BlueprintPure, Category = "Aeros|Volcano")
	FVector GetVentLocation() const;

	/** Eruption column height (m) for the current VEI. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Volcano")
	float GetPlumeHeight() const;

	UFUNCTION(BlueprintImplementableEvent, Category = "Aeros|Volcano")
	void OnStateChanged(EAerosVolcanoState NewState, int32 VEI);

	/** Strongest eruption this volcano can produce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano", meta = (ClampMin = "1", ClampMax = "7"))
	int32 MaxVEI = 5;

	/** Erupts on its own (server), on average every RecurrenceDays simulated days. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano")
	bool bAutoErupt = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano", meta = (ClampMin = "0.1"))
	float RecurrenceDays = 5.f;

	/** Vent position relative to the actor (summit crater). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano")
	FVector VentOffset = FVector::ZeroVector;

	/** Real seconds of unrest (earthquake swarm) before the eruption. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano")
	float UnrestSeconds = 30.f;

	/** Real seconds of the explosive / effusive phase (gameplay length). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano")
	float EruptionSeconds = 240.f;

	/** Build a cone mountain at the actor when the level has no terrain here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano")
	bool bFallbackCone = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano")
	float FallbackConeHeightM = 1800.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Volcano")
	float FallbackConeRadiusM = 6000.f;

	UPROPERTY(ReplicatedUsing = OnRep_State, BlueprintReadOnly, Category = "Aeros|Volcano")
	EAerosVolcanoState State = EAerosVolcanoState::Dormant;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Volcano")
	int32 CurrentVEI = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Volcano")
	float StateTime = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Volcano")
	TObjectPtr<UStaticMeshComponent> Cone;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Volcano")
	TObjectPtr<UNiagaraComponent> PlumeVFX;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Volcano")
	TObjectPtr<UPointLightComponent> VentGlow;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Volcano")
	TObjectPtr<UAudioComponent> RoarAudio;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Volcano")
	TObjectPtr<UFieldSystemComponent> FieldSystem;

	/** Fallback visuals: lava blobs, bombs, plume puffs, pyroclastic cloud. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Volcano")
	TObjectPtr<UInstancedStaticMeshComponent> LavaISM;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Volcano")
	TObjectPtr<UInstancedStaticMeshComponent> RockISM;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Volcano")
	TObjectPtr<UInstancedStaticMeshComponent> AshISM;

protected:
	UFUNCTION() void OnRep_State();

private:
	struct FLavaFront { FVector Pos; int32 Segments = 0; bool bActive = true; };
	struct FBomb { FVector Pos; FVector Vel; bool bActive = false; };
	struct FPuff { FVector Pos; float Scale = 1.f; float Age = 0.f; };

	void SetState(EAerosVolcanoState NewState);
	void ServerTick(float DeltaSeconds);
	void UpdateLava(float DeltaSeconds);
	void UpdateBombs(float DeltaSeconds);
	void UpdatePyroclastic(float DeltaSeconds);
	void UpdatePlumeVisual(float DeltaSeconds);
	void Burn(const FVector& Where, float RadiusCm, float DamagePerSecond, float DeltaSeconds, TSubclassOf<class UDamageType> Type);
	bool GroundBelow(const FVector& From, FVector& OutHit) const;

	TArray<FLavaFront> LavaFronts;
	TArray<FTransform> LavaInstances;
	TArray<FBomb> Bombs;
	TArray<FTransform> BombTransforms;
	TArray<FPuff> Puffs;
	TArray<FTransform> PuffTransforms;
	float LavaTimer = 0.f;
	float BombTimer = 0.f;
	float SwarmTimer = 0.f;
	float PyroRadius = -1.f;
	float PyroTimer = 0.f;
	float AutoTimer = 0.f;
	double LastAutoCheckDay = -1.0;
	FRandomStream Rand;

	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> LavaMID;
	UPROPERTY(Transient) TArray<TObjectPtr<UPointLightComponent>> LavaLights;
};
