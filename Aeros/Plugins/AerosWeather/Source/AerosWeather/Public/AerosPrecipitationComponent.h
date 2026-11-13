// Copyright Aeros. All Rights Reserved.
//
// UAerosPrecipitationComponent — local precipitation around the viewer (roadmap LOD: rain
// is only instanced near the player; far away the weather map / fog carry it).
// Put it on the player pawn or camera. Drives Niagara systems from the project settings
// (User.Intensity, User.Wind, User.HailSize). Without Niagara assets it falls back to a
// cheap instanced-mesh renderer so the plugin works out of the box.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "AerosTypes.h"
#include "AerosPrecipitationComponent.generated.h"

class UNiagaraComponent;
class UAudioComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;

UCLASS(ClassGroup = (Aeros), meta = (BlueprintSpawnableComponent))
class AEROSWEATHER_API UAerosPrecipitationComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UAerosPrecipitationComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Radius of the precipitation volume around the owner (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros")
	float Radius = 3000.f;

	/** Fallback renderer particle budget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros", meta = (ClampMin = "0", ClampMax = "20000"))
	int32 MaxFallbackParticles = 4000;

	/** Multiplier on visual density. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros")
	float DensityScale = 1.f;

	/** Hail impact sounds per second at full intensity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros")
	float HailSoundsPerSecond = 12.f;

	UFUNCTION(BlueprintPure, Category = "Aeros")
	FAerosWeatherSample GetLocalWeather() const { return Local; }

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Aeros")
	TObjectPtr<UNiagaraComponent> RainVFX;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Aeros")
	TObjectPtr<UNiagaraComponent> SnowVFX;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Aeros")
	TObjectPtr<UNiagaraComponent> HailVFX;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Aeros")
	TObjectPtr<UAudioComponent> RainAudio;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Aeros")
	TObjectPtr<UInstancedStaticMeshComponent> FallbackRain;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Aeros")
	TObjectPtr<UInstancedStaticMeshComponent> FallbackSolid; // snow + hail

	UPROPERTY(EditAnywhere, Category = "Aeros")
	TObjectPtr<UStaticMesh> RainMesh;

	UPROPERTY(EditAnywhere, Category = "Aeros")
	TObjectPtr<UStaticMesh> SolidMesh;

private:
	struct FParticle { FVector Pos; FVector Vel; float Size; bool bSolid; bool bAlive; };

	void UpdateNiagara();
	void UpdateFallback(float DeltaTime);
	void Respawn(FParticle& P, bool bSolid, bool bAnyHeight);
	void UpdateGroundHeight();
	void CreateSubComponents();

	FAerosWeatherSample Local;
	TArray<FParticle> Particles;
	TArray<FTransform> RainTransforms;
	TArray<FTransform> SolidTransforms;
	float GroundZ = 0.f;
	float GroundTimer = 0.f;
	float HailSoundAccum = 0.f;
	bool bNiagara = false;
	FRandomStream Rand;
};
