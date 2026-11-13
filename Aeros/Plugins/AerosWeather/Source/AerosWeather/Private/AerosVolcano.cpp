// Copyright Aeros. All Rights Reserved.

#include "AerosVolcano.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosHazardManager.h"
#include "AerosStormSystem.h"
#include "AerosAssetUtils.h"
#include "AerosWeatherModule.h"
#include "Components/AudioComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Field/FieldSystemComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

namespace AerosVolcanoInternal
{
	float MassEruptionRate(int32 VEI) { return FMath::Pow(10.f, VEI + 2.5f); } // kg/s
	constexpr int32 MaxBombs = 64;
	constexpr int32 MaxPuffs = 140;
	constexpr int32 MaxLavaInstances = 1500;
}
using namespace AerosVolcanoInternal;

AAerosVolcano::AAerosVolcano()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	SetNetUpdateFrequency(2.f);

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	Cone = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Cone"));
	Cone->SetupAttachment(RootComponent);
	if (ConeMesh.Succeeded()) Cone->SetStaticMesh(ConeMesh.Object);
	Cone->SetVisibility(false);
	Cone->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	PlumeVFX = CreateDefaultSubobject<UNiagaraComponent>(TEXT("PlumeVFX"));
	PlumeVFX->SetupAttachment(RootComponent);
	PlumeVFX->SetAutoActivate(false);

	VentGlow = CreateDefaultSubobject<UPointLightComponent>(TEXT("VentGlow"));
	VentGlow->SetupAttachment(RootComponent);
	VentGlow->SetIntensity(0.f);
	VentGlow->SetLightColor(FLinearColor(1.f, 0.35f, 0.08f));
	VentGlow->SetAttenuationRadius(300000.f);
	VentGlow->SetCastShadows(false);

	RoarAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("Roar"));
	RoarAudio->SetupAttachment(RootComponent);
	RoarAudio->SetAutoActivate(false);

	FieldSystem = CreateDefaultSubobject<UFieldSystemComponent>(TEXT("FieldSystem"));
	FieldSystem->SetupAttachment(RootComponent);

	auto MakeISM = [this](const TCHAR* Name) // SphereMesh is static: no capture needed
	{
		UInstancedStaticMeshComponent* C = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		C->SetupAttachment(RootComponent);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		if (SphereMesh.Succeeded()) C->SetStaticMesh(SphereMesh.Object);
		return C;
	};
	LavaISM = MakeISM(TEXT("LavaISM"));
	RockISM = MakeISM(TEXT("RockISM"));
	AshISM = MakeISM(TEXT("AshISM"));
}

void AAerosVolcano::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAerosVolcano, State);
	DOREPLIFETIME(AAerosVolcano, CurrentVEI);
	DOREPLIFETIME(AAerosVolcano, StateTime);
}

void AAerosVolcano::BeginPlay()
{
	Super::BeginPlay();
	Rand.Initialize(int32(GetActorLocation().X) ^ int32(GetActorLocation().Y));

	if (bFallbackCone)
	{
		// Engine cone: radius 50 cm, height 100 cm, pivot at its centre
		Cone->SetVisibility(true);
		Cone->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Cone->SetCollisionProfileName(TEXT("BlockAll"));
		Cone->SetRelativeScale3D(FVector(FallbackConeRadiusM * 2.f, FallbackConeRadiusM * 2.f, FallbackConeHeightM));
		Cone->SetRelativeLocation(FVector(0, 0, FallbackConeHeightM * 50.f));
		if (UMaterialInstanceDynamic* M = Cone->CreateDynamicMaterialInstance(0)) M->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.08f, 0.07f, 0.065f));
		if (VentOffset.IsNearlyZero()) VentOffset = FVector(0, 0, FallbackConeHeightM * 100.f);
	}
	VentGlow->SetRelativeLocation(VentOffset + FVector(0, 0, 3000));
	PlumeVFX->SetRelativeLocation(VentOffset);

	LavaMID = LavaISM->CreateDynamicMaterialInstance(0);
	if (LavaMID) LavaMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.25f, 0.02f));
	if (UMaterialInstanceDynamic* M = RockISM->CreateDynamicMaterialInstance(0)) M->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.05f, 0.04f, 0.04f));
	if (UMaterialInstanceDynamic* M = AshISM->CreateDynamicMaterialInstance(0)) M->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.18f, 0.17f, 0.16f));

	const FTransform Hidden(FQuat::Identity, FVector(0, 0, -1e7), FVector::ZeroVector);
	Bombs.SetNum(MaxBombs);
	BombTransforms.Init(Hidden, MaxBombs);
	RockISM->AddInstances(BombTransforms, false, true);
	Puffs.Reset();
	PuffTransforms.Init(Hidden, MaxPuffs);
	AshISM->AddInstances(PuffTransforms, false, true);

	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();
	if (UNiagaraSystem* Sys = Aeros::LoadOptional(Settings->VolcanoPlumeSystem)) PlumeVFX->SetAsset(Sys);
	if (USoundBase* Snd = Aeros::LoadOptional(Settings->VolcanoSound)) RoarAudio->SetSound(Snd);

	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (UAerosHazardManager* HM = Sub->GetHazardManager()) HM->RegisterVolcano(this);
	}
}

void AAerosVolcano::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		if (UAerosHazardManager* HM = Sub->GetHazardManager()) HM->UnregisterVolcano(this);
	}
	Super::EndPlay(Reason);
}

FVector AAerosVolcano::GetVentLocation() const
{
	return GetActorTransform().TransformPosition(VentOffset);
}

float AAerosVolcano::GetPlumeHeight() const
{
	if (CurrentVEI <= 0) return 0.f;
	// Mastin et al. (2009): H[km] = 2.0 * V^0.241, V = dense-rock volume rate (m3/s)
	return 2000.f * FMath::Pow(MassEruptionRate(CurrentVEI) / 2500.f, 0.241f);
}

void AAerosVolcano::StartEruption(int32 VEI)
{
	if (!HasAuthority() || State == EAerosVolcanoState::Erupting || State == EAerosVolcanoState::Unrest) return;
	CurrentVEI = FMath::Clamp(VEI, 1, MaxVEI);
	SetState(EAerosVolcanoState::Unrest);
}

void AAerosVolcano::StopEruption()
{
	if (HasAuthority() && State == EAerosVolcanoState::Erupting) SetState(EAerosVolcanoState::Waning);
}

void AAerosVolcano::SetState(EAerosVolcanoState NewState)
{
	State = NewState;
	StateTime = 0.f;
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);

	if (HasAuthority() && Sub && NewState == EAerosVolcanoState::Erupting)
	{
		// Ash column in the atmosphere: lasts EruptionSeconds of real time, in simulated seconds
		const float SimDuration = FMath::Max(EruptionSeconds * FMath::Max(Sub->GetTimeScale(), 1.f), 1800.f);
		Sub->StartVolcanicEmission(GetVentLocation(), MassEruptionRate(CurrentVEI), SimDuration);
		if (UAerosHazardManager* HM = Sub->GetHazardManager()) HM->SpawnEarthquake(GetActorLocation(), 3.f + 0.5f * CurrentVEI, 4.f);
		UE_LOG(LogAeros, Log, TEXT("Aeros: eruption VEI %d, plume %.1f km"), CurrentVEI, GetPlumeHeight() * 0.001f);
	}
	OnRep_State();
}

void AAerosVolcano::OnRep_State()
{
	if (State == EAerosVolcanoState::Erupting)
	{
		// Lava: effusive eruptions pour more lava, explosive ones throw more rock
		LavaFronts.Reset();
		const int32 Fronts = CurrentVEI <= 3 ? 5 : 2;
		FVector Ground;
		for (int32 K = 0; K < Fronts; ++K)
		{
			const float A = Rand.FRandRange(0.f, 2.f * PI);
			const FVector Start = GetVentLocation() + FVector(FMath::Cos(A), FMath::Sin(A), 0.f) * 6000.f;
			if (GroundBelow(Start, Ground)) LavaFronts.Add({ Ground });
		}
		PyroRadius = CurrentVEI >= 4 ? 0.f : -1.f;
		if (PlumeVFX->GetAsset()) PlumeVFX->Activate();
		if (RoarAudio->Sound) RoarAudio->Play();
		if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) Sub->OnVolcanicEruption.Broadcast(this, CurrentVEI);
	}
	else if (State == EAerosVolcanoState::Dormant)
	{
		PlumeVFX->Deactivate();
		RoarAudio->Stop();
		VentGlow->SetIntensity(0.f);
	}
	OnStateChanged(State, CurrentVEI);
}

bool AAerosVolcano::GroundBelow(const FVector& From, FVector& OutHit) const
{
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosVolcanoGround), false);
	if (GetWorld()->LineTraceSingleByChannel(Hit, From + FVector(0, 0, 30000), From - FVector(0, 0, 400000), ECC_WorldStatic, Params))
	{
		OutHit = Hit.ImpactPoint;
		return true;
	}
	return false;
}

void AAerosVolcano::Burn(const FVector& Where, float RadiusCm, float DamagePerSecond, float DeltaSeconds, TSubclassOf<UDamageType> Type)
{
	if (!HasAuthority()) return;
	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams Obj;
	Obj.AddObjectTypesToQuery(ECC_Pawn);
	Obj.AddObjectTypesToQuery(ECC_PhysicsBody);
	Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
	Obj.AddObjectTypesToQuery(ECC_Vehicle);
	GetWorld()->OverlapMultiByObjectType(Overlaps, Where, FQuat::Identity, Obj, FCollisionShape::MakeSphere(RadiusCm));
	TSet<AActor*> Hit;
	for (const FOverlapResult& O : Overlaps)
	{
		AActor* A = O.GetActor();
		if (!A || A == this || Hit.Contains(A)) continue;
		Hit.Add(A);
		UGameplayStatics::ApplyDamage(A, DamagePerSecond * DeltaSeconds, nullptr, this, Type);
	}
}

void AAerosVolcano::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	StateTime += DeltaSeconds;
	if (HasAuthority()) ServerTick(DeltaSeconds);

	const bool bActive = State == EAerosVolcanoState::Erupting || State == EAerosVolcanoState::Waning;
	const float Power = State == EAerosVolcanoState::Erupting ? 1.f : (State == EAerosVolcanoState::Waning ? FMath::Max(0.f, 1.f - StateTime / 60.f) : 0.f);
	VentGlow->SetIntensity(Power * 2.0e7f * (0.8f + 0.2f * FMath::Sin(StateTime * 7.f)));
	if (PlumeVFX->IsActive())
	{
		PlumeVFX->SetVariableFloat(TEXT("Intensity"), Power);
		PlumeVFX->SetVariableFloat(TEXT("PlumeHeight"), GetPlumeHeight());
	}
	if (RoarAudio->IsPlaying()) RoarAudio->SetVolumeMultiplier(FMath::Max(Power, 0.01f));

	if (bActive || LavaFronts.Num() > 0) UpdateLava(DeltaSeconds);
	UpdateBombs(DeltaSeconds);
	UpdatePyroclastic(DeltaSeconds);
	UpdatePlumeVisual(DeltaSeconds);
}

void AAerosVolcano::ServerTick(float DeltaSeconds)
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	switch (State)
	{
	case EAerosVolcanoState::Dormant:
		if (bAutoErupt && Sub)
		{
			const double Day = FMath::FloorToDouble(Sub->GetSimulationTime() / 86400.0);
			if (Day != LastAutoCheckDay)
			{
				LastAutoCheckDay = Day;
				if (Rand.FRand() < 1.f / RecurrenceDays)
				{
					// Small eruptions are far more frequent than big ones (~x3 per VEI step)
					const int32 VEI = FMath::Clamp(1 + int32(-FMath::Loge(FMath::Max(Rand.FRand(), 1e-3f)) / FMath::Loge(3.f)), 1, MaxVEI);
					StartEruption(VEI);
				}
			}
		}
		break;
	case EAerosVolcanoState::Unrest:
		// Volcano-tectonic earthquake swarm as magma rises
		SwarmTimer -= DeltaSeconds;
		if (SwarmTimer <= 0.f && Sub && Sub->GetHazardManager())
		{
			SwarmTimer = Rand.FRandRange(3.f, 8.f);
			const FVector Off(Rand.FRandRange(-1.f, 1.f), Rand.FRandRange(-1.f, 1.f), 0.f);
			Sub->GetHazardManager()->SpawnEarthquake(GetActorLocation() + Off * 400000.0, Rand.FRandRange(2.5f, 3.5f + 0.2f * CurrentVEI), Rand.FRandRange(2.f, 8.f));
		}
		if (StateTime >= UnrestSeconds) SetState(EAerosVolcanoState::Erupting);
		break;
	case EAerosVolcanoState::Erupting:
		if (StateTime >= EruptionSeconds) SetState(EAerosVolcanoState::Waning);
		break;
	case EAerosVolcanoState::Waning:
		if (StateTime >= 60.f)
		{
			SetState(EAerosVolcanoState::Dormant);
			CurrentVEI = 0;
		}
		break;
	}
}

void AAerosVolcano::UpdateLava(float DeltaSeconds)
{
	LavaTimer += DeltaSeconds;
	const bool bStep = LavaTimer >= 1.5f;
	if (bStep) LavaTimer = 0.f;

	for (FLavaFront& F : LavaFronts)
	{
		// The flow front burns what it reaches (server)
		Burn(F.Pos, 1200.f, 60.f, DeltaSeconds, UAerosLavaDamageType::StaticClass());
		if (!bStep || !F.bActive) continue;
		if (State == EAerosVolcanoState::Dormant || F.Segments > 150 || LavaInstances.Num() >= MaxLavaInstances) { F.bActive = false; continue; }

		// Steepest descent over the real terrain (8 directions, 15 m ahead)
		FVector Best = F.Pos;
		float BestZ = float(F.Pos.Z) - 5.f;
		for (int32 D = 0; D < 8; ++D)
		{
			const float A = D * PI / 4.f + Rand.FRandRange(-0.2f, 0.2f);
			FVector Hit;
			if (GroundBelow(F.Pos + FVector(FMath::Cos(A), FMath::Sin(A), 0.f) * 1500.f, Hit) && Hit.Z < BestZ)
			{
				BestZ = float(Hit.Z);
				Best = Hit;
			}
		}
		if (Best.Equals(F.Pos))
		{
			F.bActive = false; // ponds in a depression
			continue;
		}
		F.Pos = Best;
		++F.Segments;
		const FTransform T(FRotator(0, Rand.FRandRange(0.f, 360.f), 0), Best, FVector(Rand.FRandRange(9.f, 14.f), Rand.FRandRange(9.f, 14.f), 2.5f));
		LavaISM->AddInstance(T, true);
		LavaInstances.Add(T);
		if (F.Segments % 20 == 0 && LavaLights.Num() < 10)
		{
			UPointLightComponent* L = NewObject<UPointLightComponent>(this);
			L->SetupAttachment(RootComponent);
			L->RegisterComponent();
			L->SetWorldLocation(Best + FVector(0, 0, 800));
			L->SetLightColor(FLinearColor(1.f, 0.3f, 0.05f));
			L->SetIntensity(3.0e6f);
			L->SetAttenuationRadius(8000.f);
			L->SetCastShadows(false);
			LavaLights.Add(L);
		}
	}
	// Cooling crust after the eruption
	if (State == EAerosVolcanoState::Dormant && LavaMID)
	{
		LavaMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.08f, 0.05f, 0.04f));
		for (UPointLightComponent* L : LavaLights) if (L) L->SetIntensity(0.f);
		LavaFronts.Reset();
	}
}

void AAerosVolcano::UpdateBombs(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	const FVector Vent = GetVentLocation();
	if (State == EAerosVolcanoState::Erupting && CurrentVEI >= 2)
	{
		BombTimer -= DeltaSeconds;
		while (BombTimer <= 0.f)
		{
			BombTimer += 0.8f / CurrentVEI;
			for (FBomb& B : Bombs)
			{
				if (B.bActive) continue;
				const float Az = Rand.FRandRange(0.f, 2.f * PI), El = FMath::DegreesToRadians(Rand.FRandRange(50.f, 85.f));
				const float Speed = Rand.FRandRange(60.f, 90.f + 25.f * CurrentVEI) * 100.f;
				B.Pos = Vent + FVector(0, 0, 500);
				B.Vel = FVector(FMath::Cos(Az) * FMath::Cos(El), FMath::Sin(Az) * FMath::Cos(El), FMath::Sin(El)) * Speed;
				B.bActive = true;
				break;
			}
		}
	}

	static const FTransform Hidden(FQuat::Identity, FVector(0, 0, -1e7), FVector::ZeroVector);
	bool bAny = false;
	for (int32 K = 0; K < Bombs.Num(); ++K)
	{
		FBomb& B = Bombs[K];
		if (!B.bActive) { BombTransforms[K] = Hidden; continue; }
		bAny = true;
		const FVector Prev = B.Pos;
		B.Vel.Z -= 981.f * DeltaSeconds;
		B.Pos += B.Vel * DeltaSeconds;
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosBomb), false, this);
		if (World->LineTraceSingleByChannel(Hit, Prev, B.Pos, ECC_Visibility, Params))
		{
			B.bActive = false;
			BombTransforms[K] = Hidden;
			if (HasAuthority())
			{
				TArray<AActor*> Ignore;
				UGameplayStatics::ApplyRadialDamage(this, 80.f, Hit.ImpactPoint, 600.f, UAerosVolcanicDamageType::StaticClass(), Ignore, this, nullptr, false);
				if (FieldSystem && UAerosWeatherSettings::Get()->bEnableChaosDestruction) FieldSystem->ApplyStrainField(true, Hit.ImpactPoint, 600.f, 900000.f, 1);
			}
			if (USoundBase* Boom = Aeros::LoadOptional(UAerosWeatherSettings::Get()->ExplosionSound))
			{
				if (Rand.FRand() < 0.3f) UGameplayStatics::PlaySoundAtLocation(this, Boom, Hit.ImpactPoint, 0.6f);
			}
			continue;
		}
		BombTransforms[K] = FTransform(FRotator(B.Pos.X, B.Pos.Y, 0), B.Pos, FVector(1.5f));
	}
	if (bAny || State != EAerosVolcanoState::Dormant) RockISM->BatchUpdateInstancesTransforms(0, BombTransforms, true, true, true);
}

void AAerosVolcano::UpdatePyroclastic(float DeltaSeconds)
{
	if (PyroRadius < 0.f) return;
	const float MaxR = 2000.f * CurrentVEI * 100.f; // VEI 4 -> 8 km, VEI 6 -> 12 km
	PyroRadius += 4500.f * DeltaSeconds;             // ~45 m/s density current
	if (PyroRadius > MaxR) { PyroRadius = -1.f; return; }

	// Burning, choking, extremely fast: everything in the moving front is hit (server)
	PyroTimer += DeltaSeconds;
	if (PyroTimer >= 0.25f && HasAuthority())
	{
		PyroTimer = 0.f;
		TArray<FOverlapResult> Overlaps;
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_Pawn);
		Obj.AddObjectTypesToQuery(ECC_PhysicsBody);
		Obj.AddObjectTypesToQuery(ECC_Vehicle);
		const FVector C = GetActorLocation();
		GetWorld()->OverlapMultiByObjectType(Overlaps, C, FQuat::Identity, Obj, FCollisionShape::MakeSphere(PyroRadius));
		for (const FOverlapResult& O : Overlaps)
		{
			AActor* A = O.GetActor();
			if (!A || FVector::Dist2D(A->GetActorLocation(), C) < PyroRadius - 30000.f) continue;
			UGameplayStatics::ApplyDamage(A, 40.f, nullptr, this, UAerosVolcanicDamageType::StaticClass());
			if (UPrimitiveComponent* P = O.GetComponent(); P && P->IsSimulatingPhysics())
			{
				P->AddImpulse((A->GetActorLocation() - C).GetSafeNormal2D() * 3000.f + FVector(0, 0, 500), NAME_None, true);
			}
		}
	}
	// Visual: puffs along the front
	for (int32 K = 0; K < 3 && Puffs.Num() < MaxPuffs; ++K)
	{
		const float A = Rand.FRandRange(0.f, 2.f * PI);
		FVector Ground;
		const FVector P = GetActorLocation() + FVector(FMath::Cos(A), FMath::Sin(A), 0.f) * PyroRadius;
		if (GroundBelow(P, Ground)) Puffs.Add({ Ground + FVector(0, 0, 3000), float(Rand.FRandRange(25.f, 45.f)), 0.f });
	}
}

void AAerosVolcano::UpdatePlumeVisual(float DeltaSeconds)
{
	if (PlumeVFX->GetAsset() && PlumeVFX->IsActive())
	{
		return; // Niagara handles the column
	}
	const bool bErupting = State == EAerosVolcanoState::Erupting;
	const float Height = GetPlumeHeight() * 100.f;
	const FVector Vent = GetVentLocation();
	if (bErupting && Puffs.Num() < MaxPuffs && Rand.FRand() < 0.6f)
	{
		Puffs.Add({ Vent + FVector(Rand.FRandRange(-2000.f, 2000.f), Rand.FRandRange(-2000.f, 2000.f), 0.f), float(Rand.FRandRange(10.f, 20.f)), 0.f });
	}

	FVector Wind = FVector::ZeroVector;
	if (const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) Wind = Sub->GetWeatherAtLocation(Vent).Wind850 * 100.0;

	static const FTransform Hidden(FQuat::Identity, FVector(0, 0, -1e7), FVector::ZeroVector);
	for (int32 K = Puffs.Num() - 1; K >= 0; --K)
	{
		FPuff& P = Puffs[K];
		P.Age += DeltaSeconds;
		const float Rise = P.Pos.Z < Vent.Z + Height ? 4000.f : 0.f; // buoyant column, then umbrella
		P.Pos += FVector(Wind.X, Wind.Y, Rise) * DeltaSeconds * (P.Pos.Z > Vent.Z + Height * 0.8f ? 2.f : 0.3f);
		P.Scale += DeltaSeconds * (P.Pos.Z > Vent.Z + Height * 0.8f ? 6.f : 2.5f);
		if (P.Age > 90.f) Puffs.RemoveAtSwap(K);
	}
	for (int32 K = 0; K < MaxPuffs; ++K)
	{
		PuffTransforms[K] = K < Puffs.Num() ? FTransform(FQuat::Identity, Puffs[K].Pos, FVector(Puffs[K].Scale)) : Hidden;
	}
	if (Puffs.Num() > 0 || bErupting) AshISM->BatchUpdateInstancesTransforms(0, PuffTransforms, true, true, true);
}
