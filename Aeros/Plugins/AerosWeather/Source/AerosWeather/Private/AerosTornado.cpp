// Copyright Aeros. All Rights Reserved.

#include "AerosTornado.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosStormManager.h"
#include "AerosStormSystem.h"
#include "Core/AerosPhysics.h"
#include "Core/AerosSnapshot.h"
#include "AerosAssetUtils.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Field/FieldSystemComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"
#include "ProceduralMeshComponent.h"
#include "Misc/CommandLine.h"

AAerosTornado::AAerosTornado()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(true);
	SetNetUpdateFrequency(10.f);

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	FunnelVFX = CreateDefaultSubobject<UNiagaraComponent>(TEXT("FunnelVFX"));
	FunnelVFX->SetupAttachment(RootComponent);
	FunnelVFX->SetAutoActivate(false);

	RoarAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("Roar"));
	RoarAudio->SetupAttachment(RootComponent);
	RoarAudio->SetAutoActivate(false);

	FieldSystem = CreateDefaultSubobject<UFieldSystemComponent>(TEXT("FieldSystem"));
	FieldSystem->SetupAttachment(RootComponent);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	FallbackFunnel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FallbackFunnel"));
	FallbackFunnel->SetupAttachment(RootComponent);
	FallbackFunnel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FallbackFunnel->SetCastShadow(false);
	FallbackFunnel->SetVisibility(false);
	if (ConeMesh.Succeeded()) FallbackFunnel->SetStaticMesh(ConeMesh.Object);

	FallbackDebris = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("FallbackDebris"));
	FallbackDebris->SetupAttachment(RootComponent);
	FallbackDebris->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FallbackDebris->SetCastShadow(false);
	FallbackDebris->SetVisibility(false);
	if (CubeMesh.Succeeded()) FallbackDebris->SetStaticMesh(CubeMesh.Object);

	FunnelMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FunnelMesh"));
	FunnelMesh->SetupAttachment(RootComponent);
	FunnelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FunnelMesh->SetCastShadow(false);
	FunnelMesh->SetVisibility(false);

	FallbackWallCloud = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FallbackWallCloud"));
	FallbackDustCloud = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FallbackDustCloud"));
	for (UStaticMeshComponent* C : { FallbackWallCloud.Get(), FallbackDustCloud.Get() })
	{
		C->SetupAttachment(RootComponent);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetVisibility(false);
		if (SphereMesh.Succeeded()) C->SetStaticMesh(SphereMesh.Object); // flattened spheres: soft cloud blobs
	}
}

void AAerosTornado::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAerosTornado, ParentStormId);
	DOREPLIFETIME(AAerosTornado, EFRating);
	DOREPLIFETIME(AAerosTornado, MaxWind);
	DOREPLIFETIME(AAerosTornado, CoreRadius);
	DOREPLIFETIME(AAerosTornado, Lifetime);
	DOREPLIFETIME(AAerosTornado, Age);
	DOREPLIFETIME(AAerosTornado, TrackVelocity);
	DOREPLIFETIME(AAerosTornado, Seed);
}

void AAerosTornado::ServerInitialize(int32 InStormId, float InMaxWind, float InCoreRadiusM, float InLifetime, const FVector& InVelocity, int32 InSeed)
{
	ParentStormId = InStormId;
	MaxWind = InMaxWind;
	EFRating = Aeros::Phys::EFScale(InMaxWind);
	CoreRadius = InCoreRadiusM;
	Lifetime = InLifetime;
	TrackVelocity = InVelocity;
	Seed = InSeed;
	Age = 0.f;
}

void AAerosTornado::BeginPlay()
{
	Super::BeginPlay();
	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();

	if (UNiagaraSystem* Sys = Settings->TornadoSystem.LoadSynchronous())
	{
		FunnelVFX->SetAsset(Sys);
		FunnelVFX->Activate();
	}
	else if (Settings->bUseFallbackVisuals)
	{
		bUseFallback = true;
		FallbackFunnel->SetVisibility(true);
		FallbackDebris->SetVisibility(true);
		// Soft translucent condensation funnel + wall cloud + dust cloud (material from aeros_setup.py)
		UMaterialInterface* Soft = Aeros::LoadOptional(TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/AerosWeather/Materials/M_AerosShaft.M_AerosShaft"))));
		if (Soft)
		{
			FallbackFunnel->SetMaterial(0, Soft);
			FallbackWallCloud->SetMaterial(0, Soft);
			FallbackDustCloud->SetMaterial(0, Soft);
			FallbackWallCloud->SetVisibility(true);
			FallbackDustCloud->SetVisibility(true);
			WallCloudMID = FallbackWallCloud->CreateDynamicMaterialInstance(0);
			DustCloudMID = FallbackDustCloud->CreateDynamicMaterialInstance(0);
		}
		FunnelMID = FallbackFunnel->CreateDynamicMaterialInstance(0);
		if (FunnelMID) FunnelMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.18f, 0.17f, 0.16f));
		if (WallCloudMID) WallCloudMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.1f, 0.11f, 0.13f));
		if (DustCloudMID) DustCloudMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.3f, 0.24f, 0.17f));

		// Deformable funnel (replaces the rigid cone when its material exists)
		int32 ForcedShape = ShapeOverride;
		FParse::Value(FCommandLine::Get(), TEXT("AerosTornadoShape="), ForcedShape);
		Shape = ForcedShape >= 0 && ForcedShape <= int32(EAerosTornadoShape::Multivortex) ? static_cast<EAerosTornadoShape>(ForcedShape) : PickShape(EFRating, Seed);
		NumSubVortices = Shape == EAerosTornadoShape::Multivortex ? 3 + int32(uint32(Seed) % 3u) : 0;
		if (UMaterialInterface* TubeMat = Aeros::LoadOptional(TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/AerosWeather/Materials/M_AerosTornadoFunnel.M_AerosTornadoFunnel")))))
		{
			TubeMID = UMaterialInstanceDynamic::Create(TubeMat, this);
			SubTubeMID = UMaterialInstanceDynamic::Create(TubeMat, this);
			FunnelMesh->SetVisibility(true);
			FallbackFunnel->SetVisibility(false);
		}
		float StartAge = -1.f; // debug: -AerosTornadoAge=0.5 starts half way through the life cycle
		if (HasAuthority() && FParse::Value(FCommandLine::Get(), TEXT("AerosTornadoAge="), StartAge)) Age = FMath::Clamp(StartAge, 0.f, 0.99f) * Lifetime;

		FRandomStream Rand(Seed);
		const int32 Count = 180;
		Debris.SetNum(Count);
		DebrisTransforms.SetNum(Count);
		for (int32 K = 0; K < Count; ++K)
		{
			FDebris& D = Debris[K];
			D.RadiusFrac = Rand.FRandRange(0.6f, 3.f);
			D.Height = FMath::Pow(Rand.FRand(), 2.f) * 350.f; // m, most debris low
			D.Phase = Rand.FRandRange(0.f, 2.f * PI);
			D.Speed = Rand.FRandRange(0.7f, 1.1f);
			D.Scale = Rand.FRandRange(0.004f, 0.02f) * FMath::Max(CoreRadius, 30.f);
			DebrisTransforms[K] = FTransform::Identity;
		}
		FallbackDebris->AddInstances(DebrisTransforms, false, true);
	}

	if (USoundBase* Snd = Settings->TornadoLoopSound.LoadSynchronous())
	{
		RoarAudio->SetSound(Snd);
		RoarAudio->Play();
	}

	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (Sub->GetStormManager()) Sub->GetStormManager()->RegisterTornado(this);
		Sub->OnTornadoTouchdown.Broadcast(this);
	}
	UpdateGround();
}

void AAerosTornado::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (Sub->GetStormManager()) Sub->GetStormManager()->UnregisterTornado(this);
	}
	Super::EndPlay(Reason);
}

float AAerosTornado::GetStrength() const
{
	const float T = Lifetime > 0.f ? Age / Lifetime : 1.f;
	const float SpinUp = Aeros::Phys::SmoothStep01(T / 0.12f);
	const float RopeOut = 1.f - Aeros::Phys::SmoothStep01((T - 0.75f) / 0.25f);
	return SpinUp * RopeOut;
}

FVector AAerosTornado::GetWindAt(const FVector& WorldLocation) const
{
	const FVector C = GetActorLocation();
	const FVector D = WorldLocation - C;
	const float R = FMath::Max(float(D.Size2D()) * 0.01f, 1.f);
	const float Rc = FMath::Max(CoreRadius, 5.f);
	if (R > Rc * 10.f) return FVector::ZeroVector;
	const float HeightM = float(WorldLocation.Z - GroundZ) * 0.01f;
	if (HeightM > 1500.f) return FVector::ZeroVector;

	const float Strength = GetStrength();
	const float Vt = MaxWind * Strength * (R < Rc ? R / Rc : Rc / R);

	float Hemi = 1.f;
	if (const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		Hemi = Sub->GetSimulationSettings().LatitudeDeg < 0.f ? -1.f : 1.f;
	}
	// Counter-clockwise seen from above in the northern hemisphere (world -Y = north)
	const FVector Radial = FVector(D.X, D.Y, 0.0) / (R * 100.0);
	const FVector Tangent = FVector(Radial.Y, -Radial.X, 0.0) * Hemi;
	const float Inflow = 0.35f * Vt * (HeightM < 100.f ? 1.f : 0.3f);
	const float Up = R < 2.f * Rc ? 0.5f * MaxWind * Strength * (1.f - FMath::Square(R / (2.f * Rc))) : 0.f;

	const float Ground = HeightM < 0.f ? 1.f : FMath::Clamp(1.f - HeightM / 1500.f, 0.f, 1.f);
	return (Tangent * Vt - Radial * Inflow + FVector(0, 0, Up)) * Ground;
}

void AAerosTornado::UpdateGround()
{
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosTornadoGround), false, this);
	const FVector L = GetActorLocation();
	if (GetWorld() && GetWorld()->LineTraceSingleByObjectType(Hit, FVector(L.X, L.Y, L.Z + 500000.0), FVector(L.X, L.Y, L.Z - 2000000.0), FCollisionObjectQueryParams(ECC_WorldStatic), Params))
	{
		GroundZ = float(Hit.ImpactPoint.Z);
	}
	SetActorLocation(FVector(L.X, L.Y, GroundZ));
}

void AAerosTornado::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	VisualTime += DeltaSeconds;

	if (HasAuthority())
	{
		// Lifetime is tuned for the default x60 clock: age faster when time runs faster,
		// otherwise tornadoes pile up at high time scales.
		const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
		const float TimeFactor = Sub ? FMath::Max(1.f, Sub->GetTimeScale() / 60.f) : 1.f;
		Age += DeltaSeconds * TimeFactor;

		// A tornado cannot outlive its parent storm (or drift away from it)
		ParentCheckTimer += DeltaSeconds;
		if (Sub && ParentStormId >= 0 && ParentCheckTimer > 1.f)
		{
			ParentCheckTimer = 0.f;
			bool bParentNear = false;
			if (FAerosSnapshotPtr Snap = Sub->GetSnapshot())
			{
				for (const FAerosStormData& S : Snap->Storms)
				{
					if (S.Id == ParentStormId) { bParentNear = FVector::Dist2D(S.UpdraftLocation, GetActorLocation()) < 2000000.0; break; }
				}
			}
			if (!bParentNear) Age = FMath::Max(Age, Lifetime * 0.8f); // rope out over the last 20 %
		}
		// Track: storm motion + slow meander (looping paths are common)
		const FVector Perp(-TrackVelocity.Y, TrackVelocity.X, 0.0);
		const float Meander = FMath::Sin(Age * 0.05f + Seed) * 0.35f;
		const FVector Vel = TrackVelocity + Perp * Meander;
		SetActorLocation(GetActorLocation() + Vel * 100.0 * DeltaSeconds);

		GroundTimer += DeltaSeconds;
		if (GroundTimer > 0.5f) { GroundTimer = 0.f; UpdateGround(); }

		ApplyPhysicalEffects(DeltaSeconds);

		if (Age >= Lifetime)
		{
			Destroy();
			return;
		}
	}

	const float Strength = GetStrength();
	if (FunnelVFX && FunnelVFX->GetAsset())
	{
		FunnelVFX->SetVariableFloat(TEXT("Strength"), Strength);
		FunnelVFX->SetVariableFloat(TEXT("CoreRadius"), CoreRadius);
		FunnelVFX->SetVariableFloat(TEXT("MaxWind"), MaxWind);
	}
	if (RoarAudio && RoarAudio->IsPlaying()) RoarAudio->SetVolumeMultiplier(FMath::Max(Strength, 0.05f));
	if (bUseFallback) UpdateFallbackVisuals(DeltaSeconds);
}

void AAerosTornado::ApplyPhysicalEffects(float DeltaSeconds)
{
	PhysicsTimer += DeltaSeconds;
	if (PhysicsTimer < 0.05f) return;
	const float Step = PhysicsTimer;
	PhysicsTimer = 0.f;

	UWorld* World = GetWorld();
	const float Strength = GetStrength();
	if (!World || Strength <= 0.01f) return;

	const FVector Center = GetActorLocation();
	const float Radius = FMath::Max(CoreRadius * 4.f, 150.f) * 100.f;

	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams ObjParams;
	ObjParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	ObjParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjParams.AddObjectTypesToQuery(ECC_Pawn);
	ObjParams.AddObjectTypesToQuery(ECC_Vehicle);
	ObjParams.AddObjectTypesToQuery(ECC_Destructible);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosTornado), false, this);
	World->OverlapMultiByObjectType(Overlaps, Center + FVector(0, 0, Radius * 0.5), FQuat::Identity, ObjParams,
		FCollisionShape::MakeCapsule(Radius, Radius), Params);

	TSet<AActor*> Damaged;
	for (const FOverlapResult& O : Overlaps)
	{
		UPrimitiveComponent* Prim = O.GetComponent();
		AActor* Actor = O.GetActor();
		if (!Prim || !Actor) continue;

		const FVector Loc = Prim->GetComponentLocation();
		const FVector Wind = GetWindAt(Loc);
		const float WindSpeed = float(Wind.Size());

		if (Prim->IsSimulatingPhysics())
		{
			// Aerodynamic drag: F = 0.5 rho Cd A |v_rel| v_rel
			const FVector BodyVel = Prim->GetPhysicsLinearVelocity() * 0.01;
			const FVector Rel = Wind - BodyVel;
			const FVector Extent = Prim->Bounds.BoxExtent * 0.01; // m
			const float Area = float(4.0 * FMath::Max(Extent.X * Extent.Z, Extent.Y * Extent.Z));
			const float Mass = FMath::Max(Prim->GetMass(), 1.f);
			FVector Accel = 0.5 * 1.2 * 1.0 * Area * Rel.Size() * Rel / Mass * ForceScale;
			if (Accel.Size() > MaxAcceleration) Accel = Accel.GetSafeNormal() * MaxAcceleration;
			Prim->AddForce(Accel * 100.0, NAME_None, true);
		}
		else if (ACharacter* Char = Cast<ACharacter>(Actor))
		{
			if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
			{
				if (WindSpeed > 38.f && !Damaged.Contains(Actor)) // EF1+ lifts people
				{
					Char->LaunchCharacter(Wind * 100.0 * 0.25 + FVector(0, 0, 300.0), false, false);
				}
				else
				{
					Move->AddForce(Wind * 100.0 * Move->Mass * 0.4);
				}
			}
		}

		if (WindSpeed > 30.f && !Damaged.Contains(Actor))
		{
			Damaged.Add(Actor);
			UGameplayStatics::ApplyDamage(Actor, (WindSpeed - 30.f) * 0.4f * Step * 20.f, nullptr, this, UAerosWindDamageType::StaticClass());
		}
	}

	// Chaos: break Geometry Collections in the core, pull debris inward
	if (FieldSystem && UAerosWeatherSettings::Get()->bEnableChaosDestruction)
	{
		const float Core = CoreRadius * 100.f;
		FieldSystem->ApplyStrainField(true, Center, Core * 1.5f, 200000.f * (EFRating + 1) * Strength, 1);
		FieldSystem->ApplyRadialVectorFalloffForce(true, Center, Core * 3.f, -MaxWind * Strength * 2000.f);
	}
}

void AAerosTornado::UpdateFallbackVisuals(float DeltaSeconds)
{
	const float Strength = GetStrength();
	const float Rc = FMath::Max(CoreRadius, 20.f);

	// Funnel: cone tip down, widening into the wall cloud. Visual floor so it reads from far away.
	const float Vis = FMath::Max(Strength, 0.25f);
	const float HeightM = 1200.f;
	const float TopRadiusM = FMath::Max(Rc * 4.f, 250.f);
	FallbackFunnel->SetRelativeRotation(FRotator(180.f, VisualTime * 90.f, 0.f));
	// Engine cone: radius 50 cm, height 100 cm, pivot at centre
	const float XY = TopRadiusM * 2.f * Vis + 0.01f;
	FallbackFunnel->SetRelativeScale3D(FVector(XY, XY, HeightM));
	FallbackFunnel->SetRelativeLocation(FVector(0, 0, HeightM * 50.f));
	if (FunnelMID) FunnelMID->SetScalarParameterValue(TEXT("Opacity"), 0.85f * Vis);

	// Wall cloud: wide, dark, slowly rotating lowered base (engine sphere: 1 m diameter)
	const float WallM = TopRadiusM * 3.f;
	FallbackWallCloud->SetRelativeScale3D(FVector(WallM * 2.f, WallM * 2.f, 350.f));
	FallbackWallCloud->SetRelativeLocation(FVector(0, 0, (HeightM + 150.f) * 100.f));
	FallbackWallCloud->SetRelativeRotation(FRotator(0.f, VisualTime * 6.f, 0.f));
	if (WallCloudMID) WallCloudMID->SetScalarParameterValue(TEXT("Opacity"), 0.7f);

	// Dust cloud: brown skirt at the ground, grows with the wind
	const float DustM = Rc * 3.f * (0.5f + Strength);
	FallbackDustCloud->SetRelativeScale3D(FVector(DustM * 2.f, DustM * 2.f, 200.f * (0.4f + Strength)));
	FallbackDustCloud->SetRelativeLocation(FVector(0, 0, 100.f * 100.f * (0.4f + Strength)));
	if (DustCloudMID) DustCloudMID->SetScalarParameterValue(TEXT("Opacity"), 0.6f * Strength);

	float Hemi = 1.f;
	if (const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		Hemi = Sub->GetSimulationSettings().LatitudeDeg < 0.f ? -1.f : 1.f;
	}
	const FVector Base = GetActorLocation();
	for (int32 K = 0; K < Debris.Num(); ++K)
	{
		FDebris& D = Debris[K];
		const float R = Rc * D.RadiusFrac * (0.6f + 0.4f * Strength);
		const float Vt = MaxWind * Strength * (D.RadiusFrac < 1.f ? D.RadiusFrac : 1.f / D.RadiusFrac);
		D.Phase += Hemi * Vt / FMath::Max(R, 1.f) * D.Speed * DeltaSeconds;
		const float H = D.Height * (0.4f + 0.6f * Strength) + 20.f * FMath::Sin(VisualTime * 2.f + D.Phase);
		const FVector P = Base + FVector(FMath::Cos(D.Phase) * R * 100.f, -FMath::Sin(D.Phase) * R * 100.f, H * 100.f);
		const float S = D.Scale * (Strength > 0.05f ? 1.f : 0.f);
		DebrisTransforms[K] = FTransform(FRotator(D.Phase * 57.f, D.Phase * 31.f, 0.f), P, FVector(S));
	}
	FallbackDebris->BatchUpdateInstancesTransforms(0, DebrisTransforms, true, true, true);
	if (FunnelMesh->IsVisible()) UpdateFunnelMesh(DeltaSeconds); // last: it also places the wall cloud
}

// =====================================================================
//  Deformable funnel (procedural tube)
// =====================================================================

EAerosTornadoShape AAerosTornado::PickShape(int32 InEFRating, int32 InSeed)
{
	// Spotter statistics: weak tornadoes are ropes / needles / cones, violent ones wedges / stovepipes
	const float U = Aeros::Phys::Hash01(uint32(InSeed), 0x5Au, 0x7Eu, 3u);
	if (InEFRating <= 1) return U < 0.3f ? EAerosTornadoShape::Rope : (U < 0.65f ? EAerosTornadoShape::Needle : EAerosTornadoShape::Cone);
	if (InEFRating <= 3)
	{
		if (U < 0.35f) return EAerosTornadoShape::Cone;
		if (U < 0.6f) return EAerosTornadoShape::Stovepipe;
		if (U < 0.75f) return EAerosTornadoShape::Concave;
		if (U < 0.85f) return EAerosTornadoShape::Hourglass;
		return EAerosTornadoShape::Wedge;
	}
	return U < 0.45f ? EAerosTornadoShape::Wedge : (U < 0.7f ? EAerosTornadoShape::Stovepipe : EAerosTornadoShape::Multivortex);
}

float AAerosTornado::ShapeProfile(EAerosTornadoShape InShape, float T) const
{
	// Relative radius from the ground (T = 0) to the cloud base (T = 1)
	switch (InShape)
	{
	case EAerosTornadoShape::Rope:        return 0.12f + 0.05f * T;
	case EAerosTornadoShape::Needle:      return 0.14f + 0.36f * FMath::Pow(T, 1.5f);
	case EAerosTornadoShape::Cone:        return 0.18f + 0.82f * FMath::Pow(T, 1.2f);
	case EAerosTornadoShape::Stovepipe:   return 0.85f + 0.15f * T * T * T;
	case EAerosTornadoShape::Concave:     return 0.14f + 0.86f * T * T * T;
	case EAerosTornadoShape::Hourglass:   return 0.4f + 0.6f * FMath::Pow(FMath::Abs(T - 0.45f) / 0.55f, 1.3f);
	case EAerosTornadoShape::Wedge:       return 0.9f + 0.25f * T;
	case EAerosTornadoShape::Multivortex: return 0.95f + 0.2f * T;
	default:                              return 0.5f;
	}
}

void AAerosTornado::BuildTube(int32 Section, const TArray<FVector>& Axis, const TArray<float>& Radius, const TArray<float>& Alpha, float Roughness)
{
	const int32 Seg = 32;
	const int32 Rings = Axis.Num();
	if (Rings < 2) return;
	TubeVerts.Reset();
	TubeNormals.Reset();
	TubeUVs.Reset();
	TubeColors.Reset();

	for (int32 K = 0; K < Rings; ++K)
	{
		// Local frame around the (bent) axis
		const FVector Tangent = (Axis[FMath::Min(K + 1, Rings - 1)] - Axis[FMath::Max(K - 1, 0)]).GetSafeNormal();
		FVector Side = FVector::CrossProduct(Tangent, FVector::ForwardVector);
		if (Side.SizeSquared() < 1e-4) Side = FVector::CrossProduct(Tangent, FVector::RightVector);
		Side.Normalize();
		const FVector Side2 = FVector::CrossProduct(Tangent, Side);
		for (int32 S = 0; S <= Seg; ++S)
		{
			const float A = 2.f * PI * float(S) / float(Seg);
			const FVector Dir = Side * FMath::Cos(A) + Side2 * FMath::Sin(A);
			// Lumpy, turning silhouette (condensation is never a perfect cylinder)
			const float Wobble = 1.f + Roughness * (0.6f * FMath::Sin(3.f * A + VisualTime * 1.7f + K * 0.4f) + 0.4f * FMath::Sin(7.f * A - VisualTime * 2.3f + K * 0.9f));
			TubeVerts.Add(Axis[K] + Dir * Radius[K] * Wobble);
			TubeNormals.Add(Dir);
			TubeUVs.Add(FVector2D(float(S) / Seg, float(K) / (Rings - 1)));
			TubeColors.Add(FLinearColor(1.f, 1.f, 1.f, Alpha[K]));
		}
	}

	if (!TubeSectionsCreated.Contains(Section))
	{
		TubeTris.Reset();
		for (int32 K = 0; K + 1 < Rings; ++K)
		{
			for (int32 S = 0; S < Seg; ++S)
			{
				const int32 I0 = K * (Seg + 1) + S, I1 = I0 + 1, I2 = I0 + Seg + 1, I3 = I2 + 1;
				TubeTris.Append({ I0, I2, I1, I1, I2, I3 });
			}
		}
		FunnelMesh->CreateMeshSection_LinearColor(Section, TubeVerts, TubeTris, TubeNormals, TubeUVs, TubeColors, TArray<FProcMeshTangent>(), false);
		FunnelMesh->SetMaterial(Section, Section == 0 ? TubeMID : SubTubeMID);
		TubeSectionsCreated.Add(Section);
	}
	else
	{
		FunnelMesh->UpdateMeshSection_LinearColor(Section, TubeVerts, TubeNormals, TubeUVs, TubeColors, TArray<FProcMeshTangent>());
	}
}

void AAerosTornado::UpdateFunnelMesh(float DeltaSeconds)
{
	using Aeros::Phys::SmoothStep01;
	const float Life = Lifetime > 0.f ? FMath::Clamp(Age / Lifetime, 0.f, 1.f) : 1.f;
	const float Form = SmoothStep01(Life / 0.12f);           // funnel lowers from the cloud
	const float Decay = SmoothStep01((Life - 0.65f) / 0.35f); // rope-out stage
	const float Lift = SmoothStep01((Life - 0.93f) / 0.07f);  // the funnel lifts and dissipates
	const float Strength = GetStrength();

	const float HeightCm = (1000.f + float(uint32(Seed) % 500u)) * 100.f;
	const bool bWide = Shape == EAerosTornadoShape::Wedge || Shape == EAerosTornadoShape::Multivortex;
	const float R0 = FMath::Max(CoreRadius, 20.f) * 100.f * (bWide ? 2.6f : (Shape == EAerosTornadoShape::Stovepipe ? 1.4f : 1.2f));

	// Axis: the top follows the mesocyclone (ahead along the storm motion), the base drags behind:
	// the funnel leans, more and more as it ropes out. Slow sinuous bending, much stronger for ropes.
	FVector Motion = TrackVelocity.GetSafeNormal2D();
	if (Motion.IsNearlyZero()) Motion = FVector(1, 0, 0);
	const FVector Perp(-Motion.Y, Motion.X, 0.0);
	const float Phase = float(uint32(Seed) % 1000u) * 0.01f;
	const FVector Top = FVector(0, 0, HeightCm) + Motion * HeightCm * (0.1f + 0.55f * Decay)
		+ Perp * HeightCm * 0.06f * FMath::Sin(VisualTime * 0.07f + Phase);
	const bool bRope = Shape == EAerosTornadoShape::Rope || Shape == EAerosTornadoShape::Needle;
	// Thin funnels snake around, wide ones (wedge, stovepipe) stay almost straight until they rope out
	const float Bend = (bWide || Shape == EAerosTornadoShape::Stovepipe ? R0 * 0.08f : R0 * 0.35f) + HeightCm * ((bRope ? 0.08f : (bWide ? 0.008f : 0.025f)) + 0.16f * Decay);

	const int32 Rings = 40;
	TArray<FVector> Axis;
	TArray<float> Radius, Alpha;
	Axis.SetNum(Rings);
	Radius.SetNum(Rings);
	Alpha.SetNum(Rings);
	const float Extent = FMath::Lerp(0.3f, 1.f, Form) * (1.f - 0.65f * Lift); // visible fraction from the cloud down
	const float VisibleFrom = 1.f - Extent;
	for (int32 K = 0; K < Rings; ++K)
	{
		const float T = float(K) / float(Rings - 1);
		const float U = 1.f - T;
		// Cubic Bezier with vertical tangents at both ends
		const FVector P1 = FVector(0, 0, HeightCm * 0.4f), P2 = Top - FVector(0, 0, HeightCm * 0.4f);
		const FVector C = 3.f * U * U * T * P1 + 3.f * U * T * T * P2 + T * T * T * Top;
		const FVector Sway = FMath::Sin(PI * T) * (Perp * Bend * FMath::Sin(2.f * PI * 1.3f * T + Phase + VisualTime * 0.35f)
			+ Motion * Bend * 0.6f * FMath::Sin(2.f * PI * 2.1f * T + Phase * 1.7f + VisualTime * 0.27f));
		Axis[K] = C + Sway;
		const float Profile = FMath::Lerp(ShapeProfile(Shape, T), ShapeProfile(EAerosTornadoShape::Rope, T), Decay);
		Radius[K] = FMath::Max(R0 * Profile * (0.55f + 0.45f * Form), 800.f);
		Alpha[K] = FMath::SmoothStep(VisibleFrom, VisibleFrom + 0.1f, T) * (1.f - 0.45f * FMath::SmoothStep(0.85f, 1.f, T));
	}
	BuildTube(0, Axis, Radius, Alpha, bRope ? 0.04f : 0.09f);

	// The wall cloud sits on top of the (leaning) funnel, a lowered rotating part of the storm base
	const float WallCm = FMath::Max(R0 * 3.5f, 60000.f);
	FallbackWallCloud->SetRelativeLocation(Top + FVector(0, 0, 15000.0));
	FallbackWallCloud->SetRelativeScale3D(FVector(WallCm * 2.f / 100.f, WallCm * 2.f / 100.f, 700.f));
	if (WallCloudMID) WallCloudMID->SetScalarParameterValue(TEXT("Opacity"), 0.45f);

	// Sub-vortices: thin tubes orbiting inside a multi-vortex wedge, fading as it weakens
	const float SubFade = FMath::Clamp(1.f - Decay / 0.6f, 0.f, 1.f) * Form;
	for (int32 V = 0; V < NumSubVortices; ++V)
	{
		const int32 SubRings = 20;
		TArray<FVector> SubAxis;
		TArray<float> SubRadius, SubAlpha;
		SubAxis.SetNum(SubRings);
		SubRadius.SetNum(SubRings);
		SubAlpha.SetNum(SubRings);
		const float Orbit = 2.f * PI * float(V) / float(NumSubVortices) + VisualTime * 0.35f;
		for (int32 K = 0; K < SubRings; ++K)
		{
			const float T = 0.7f * float(K) / float(SubRings - 1);
			const int32 MainIndex = FMath::Clamp(int32(T * (Rings - 1)), 0, Rings - 1);
			const float Wander = 1.f + 0.2f * FMath::Sin(VisualTime * 0.9f + V * 2.1f + T * 6.f);
			SubAxis[K] = Axis[MainIndex] + FVector(FMath::Cos(Orbit + T * 1.2f), FMath::Sin(Orbit + T * 1.2f), 0.0) * Radius[MainIndex] * 0.6f * Wander;
			SubRadius[K] = FMath::Max(R0 * 0.11f * (1.f - 0.4f * T / 0.7f), 600.f);
			SubAlpha[K] = SubFade * (1.f - FMath::SmoothStep(0.45f, 0.7f, T)) * FMath::SmoothStep(0.f, 0.05f, T + 0.05f);
		}
		BuildTube(V + 1, SubAxis, SubRadius, SubAlpha, 0.05f);
	}

	// Look: condensation (grey-white) for weak / rope tornadoes, dark debris-laden for violent ones;
	// the streaks turn at the real angular speed of the vortex
	if (TubeMID)
	{
		const float Dark = FMath::Clamp(float(EFRating) / 5.f + (bWide ? 0.3f : 0.f), 0.f, 1.f) * (1.f - 0.7f * Decay);
		TubeMID->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(FLinearColor(0.62f, 0.64f, 0.68f), FLinearColor(0.22f, 0.2f, 0.19f), Dark));
		TubeMID->SetScalarParameterValue(TEXT("Opacity"), (Shape == EAerosTornadoShape::Multivortex ? 0.55f : (bWide ? 0.97f : 0.85f)) * FMath::Max(Strength, 0.3f));
		TubeMID->SetScalarParameterValue(TEXT("SpinTurns"), FMath::Clamp(MaxWind * FMath::Max(Strength, 0.2f) / (2.f * PI * R0 * 0.01f), 0.02f, 1.5f));
		TubeMID->SetScalarParameterValue(TEXT("Streaks"), bRope ? 0.35f : 0.65f);
	}
	if (SubTubeMID)
	{
		SubTubeMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.35f, 0.34f, 0.33f));
		SubTubeMID->SetScalarParameterValue(TEXT("Opacity"), 0.9f);
		SubTubeMID->SetScalarParameterValue(TEXT("SpinTurns"), 1.2f);
		SubTubeMID->SetScalarParameterValue(TEXT("Streaks"), 0.5f);
	}
}
