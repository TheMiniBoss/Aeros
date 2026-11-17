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
#include "AerosTypes.h"
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

struct FAerosCloudTornado;

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

	/** EF rating now: it climbs category by category up to the peak, then comes back down (Aeros::Funnel::LifeWindMs). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	int32 GetEFRating() const { return EFRating; }

	/** EF rating it will reach at its peak. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	int32 GetPeakEFRating() const { return PeakEFRating; }

	/** Peak ground wind now (m/s). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	float GetCurrentWind() const;

	/** +1 while it intensifies, 0 at its peak, -1 while it weakens. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	int32 GetEFTrend() const;

	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	float GetCoreRadius() const { return CoreRadius; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Tornado")
	int32 GetParentStormId() const { return ParentStormId; }

	/** Where a storm's tornado stands: just behind the updraft (~1 km) on the mesocyclone's side (right of the
	 *  motion in the northern hemisphere, Side = 1; left in the southern one, Side = -1), under the wall cloud
	 *  the renderer draws there. 3.4 km away (as before) it was often out of the tower, under clear sky. */
	static FVector AnchorFor(const FAerosStormData& Storm, const FVector& FallbackMotion, float Side = 1.f)
	{
		FVector Motion = Storm.Velocity.Size2D() > 1.0 ? Storm.Velocity.GetSafeNormal2D() : FallbackMotion.GetSafeNormal2D();
		if (Motion.IsNearlyZero()) Motion = FVector(1.0, 0.0, 0.0);
		const FVector Right(-Motion.Y, Motion.X, 0.0);
		return Storm.UpdraftLocation - Motion * 100000.0 + Right * (50000.0 * double(Side));
	}

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") int32 ParentStormId = -1;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") int32 EFRating = 0;          // now
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") int32 PeakEFRating = 0;      // at its peak
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Tornado") float MaxWind = 50.f;        // m/s, at its peak
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

	/** What the custom cloud renderer draws for this tornado (funnel model inputs and axis), for a cloud
	 *  base height (m above the ground) and the condensation calibration K. */
	FAerosCloudTornado MakeRenderDescriptor(float CloudBaseM, float K) const;

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
	// Custom cloud renderer: the funnel is drawn in the clouds, the meshes are hidden meanwhile
	bool bMeshesHiddenForClouds = false;
	TArray<bool> MeshVisibilityBeforeClouds;
	float RenderSpin = 0.f;      // radians, visual rotation of the funnel striations
	float RenderSubPhase = 0.f;  // radians, orbit of the sub-vortices
	float SpinSign = 1.f;        // cyclonic: counter-clockwise in the north

	float ParentCheckTimer = 0.f;
	// Stays under its parent's mesocyclone (behind the updraft, on its right flank, where it was born):
	// the storm moves in simulated time, so at x60 a tornado on its own track was left under clear sky.
	bool bHasAnchor = false;
	FVector Anchor = FVector::ZeroVector;          // cm, world (z unused)
	FVector AnchorVelocity = FVector::ZeroVector;  // cm per real second
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
