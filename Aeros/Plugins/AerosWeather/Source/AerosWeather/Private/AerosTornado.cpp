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
}
