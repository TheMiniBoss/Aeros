// Copyright Aeros. All Rights Reserved.
//
// AAerosTornado - spawned by the storm manager under a mesocyclone (roadmap: CAPE > 2500,
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
class UProceduralMeshComponent;

/** Visible shape of the condensation funnel (storm-spotter classification). */
UENUM(BlueprintType)
enum class EAerosTornadoShape : uint8
{
	Rope,         // long thin twisting tube (often the dying stage)
	Needle,       // thin, straight, tapering
	Cone,         // V-shaped, wide under the cloud, narrow at the ground
	Stovepipe,    // cylinder, same width top to bottom
	Concave,      // concave sides: flared top, thin lower half
	Hourglass,    // pinched in the middle
	Wedge,        // wider than tall, debris-laden (violent)
	Multivortex,  // wedge with several sub-vortices orbiting inside
};

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

	/** Fallback: rotating wall cloud under the storm base, where the funnel hangs from. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Tornado")
	TObjectPtr<UStaticMeshComponent> FallbackWallCloud;

	/** Fallback: deformable funnel tube (section 0) + sub-vortices (sections 1..N), rebuilt every frame. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Tornado")
	TObjectPtr<UProceduralMeshComponent> FunnelMesh;

	/** Shape picked from the EF rating and the seed (same on every machine). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	EAerosTornadoShape GetShape() const { return Shape; }

	/** Debug / art direction: force a shape (-1 = automatic). Command line: -AerosTornadoShape=0..7. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Tornado")
	int32 ShapeOverride = -1;

	/** Fallback: dust / debris cloud swirling around the base of the funnel. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Tornado")
	TObjectPtr<UStaticMeshComponent> FallbackDustCloud;

protected:
	void ApplyPhysicalEffects(float DeltaSeconds);
	void UpdateFallbackVisuals(float DeltaSeconds);
	void UpdateGround();
	void UpdateFunnelMesh(float DeltaSeconds);
	void BuildTube(int32 Section, const TArray<FVector>& Axis, const TArray<float>& Radius, const TArray<float>& Alpha, float Roughness);
	static EAerosTornadoShape PickShape(int32 InEFRating, int32 InSeed);
	float ShapeProfile(EAerosTornadoShape InShape, float T) const;

private:
	struct FDebris { float RadiusFrac, Height, Phase, Speed, Scale; };
	TArray<FDebris> Debris;
	TArray<FTransform> DebrisTransforms;
	float PhysicsTimer = 0.f;
	float GroundTimer = 0.f;
	float GroundZ = 0.f;
	float VisualTime = 0.f;
	bool bUseFallback = false;

	float ParentCheckTimer = 0.f;
	EAerosTornadoShape Shape = EAerosTornadoShape::Cone;
	int32 NumSubVortices = 0;
	TArray<int32> TubeSectionsCreated;
	TArray<FVector> TubeVerts;
	TArray<FVector> TubeNormals;
	TArray<FVector2D> TubeUVs;
	TArray<FLinearColor> TubeColors;
	TArray<int32> TubeTris;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> TubeMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SubTubeMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FunnelMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> WallCloudMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DustCloudMID;
};
