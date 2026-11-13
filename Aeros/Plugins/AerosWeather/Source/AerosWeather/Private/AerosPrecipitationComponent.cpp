// Copyright Aeros. All Rights Reserved.

#include "AerosPrecipitationComponent.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "Core/AerosPhysics.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Components/AudioComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

DECLARE_CYCLE_STAT(TEXT("Aeros Precipitation"), STAT_AerosPrecip, STATGROUP_Aeros);

UAerosPrecipitationComponent::UAerosPrecipitationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	SetIsReplicatedByDefault(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Cylinder.Succeeded()) RainMesh = Cylinder.Object;
	if (Sphere.Succeeded()) SolidMesh = Sphere.Object;
}

void UAerosPrecipitationComponent::CreateSubComponents()
{
	AActor* Owner = GetOwner();
	auto MakeNiagara = [this, Owner](const TCHAR* Name)
	{
		UNiagaraComponent* C = NewObject<UNiagaraComponent>(Owner, Name);
		C->SetAutoActivate(false);
		C->SetupAttachment(this);
		C->RegisterComponent();
		return C;
	};
	RainVFX = MakeNiagara(TEXT("AerosRainVFX"));
	SnowVFX = MakeNiagara(TEXT("AerosSnowVFX"));
	HailVFX = MakeNiagara(TEXT("AerosHailVFX"));

	RainAudio = NewObject<UAudioComponent>(Owner, TEXT("AerosRainAudio"));
	RainAudio->SetAutoActivate(false);
	RainAudio->SetupAttachment(this);
	RainAudio->RegisterComponent();

	auto MakeISM = [this, Owner](const TCHAR* Name, UStaticMesh* Mesh)
	{
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(Owner, Name);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetUsingAbsoluteScale(true);
		if (Mesh) C->SetStaticMesh(Mesh);
		C->SetupAttachment(this);
		C->RegisterComponent();
		return C;
	};
	FallbackRain = MakeISM(TEXT("AerosFallbackRain"), RainMesh);
	FallbackSolid = MakeISM(TEXT("AerosFallbackSolid"), SolidMesh);
}

void UAerosPrecipitationComponent::BeginPlay()
{
	Super::BeginPlay();
	Rand.Initialize(GetUniqueID());

	// Purely cosmetic: skip on dedicated servers
	if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
	{
		SetComponentTickEnabled(false);
		return;
	}

	CreateSubComponents();
	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();
	if (UNiagaraSystem* S = Settings->RainSystem.LoadSynchronous()) { RainVFX->SetAsset(S); bNiagara = true; }
	if (UNiagaraSystem* S = Settings->SnowSystem.LoadSynchronous()) { SnowVFX->SetAsset(S); bNiagara = true; }
	if (UNiagaraSystem* S = Settings->HailSystem.LoadSynchronous()) { HailVFX->SetAsset(S); bNiagara = true; }
	if (USoundBase* Snd = Settings->RainLoopSound.LoadSynchronous()) RainAudio->SetSound(Snd);

	if (!bNiagara && Settings->bUseFallbackVisuals)
	{
		const int32 N = MaxFallbackParticles;
		Particles.SetNum(N);
		RainTransforms.Init(FTransform(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector), N);
		SolidTransforms.Init(FTransform(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector), N);
		for (FParticle& P : Particles) P.bAlive = false;
		FallbackRain->AddInstances(RainTransforms, false, true);
		FallbackSolid->AddInstances(SolidTransforms, false, true);
		if (UMaterialInstanceDynamic* M = FallbackRain->CreateDynamicMaterialInstance(0)) M->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.55f, 0.6f, 0.68f));
		if (UMaterialInstanceDynamic* M = FallbackSolid->CreateDynamicMaterialInstance(0)) M->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.95f, 0.97f, 1.f));
	}
	else
	{
		FallbackRain->SetVisibility(false);
		FallbackSolid->SetVisibility(false);
	}
}

void UAerosPrecipitationComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	Super::EndPlay(Reason);
}

void UAerosPrecipitationComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	SCOPE_CYCLE_COUNTER(STAT_AerosPrecip);

	const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub || !Sub->IsSimulationRunning()) return;
	Local = Sub->GetWeatherAtLocation(GetComponentLocation());

	GroundTimer -= DeltaTime;
	if (GroundTimer <= 0.f) { GroundTimer = 0.5f; UpdateGroundHeight(); }

	const float Intensity = FMath::Clamp(FMath::Sqrt(Local.PrecipitationRate / 50.f), 0.f, 1.f);

	if (bNiagara) UpdateNiagara();
	else if (Particles.Num() > 0) UpdateFallback(DeltaTime);

	if (RainAudio->Sound)
	{
		const bool bRainy = Local.PrecipitationType == EAerosPrecipType::Rain || Local.PrecipitationType == EAerosPrecipType::FreezingRain || Local.PrecipitationType == EAerosPrecipType::Hail;
		const float Vol = bRainy ? Intensity : 0.f;
		if (Vol > 0.02f && !RainAudio->IsPlaying()) RainAudio->Play();
		if (RainAudio->IsPlaying()) RainAudio->SetVolumeMultiplier(FMath::Max(Vol, 0.001f));
	}

	// Hail impacts: sound on the ground around the listener
	if (Local.PrecipitationType == EAerosPrecipType::Hail && Local.HailDiameter > 0.f)
	{
		HailSoundAccum += DeltaTime * HailSoundsPerSecond * Intensity;
		if (USoundBase* Hit = UAerosWeatherSettings::Get()->HailImpactSound.LoadSynchronous())
		{
			while (HailSoundAccum >= 1.f)
			{
				HailSoundAccum -= 1.f;
				const FVector Offset(Rand.FRandRange(-1500.f, 1500.f), Rand.FRandRange(-1500.f, 1500.f), 0.f);
				const FVector Loc = FVector(GetComponentLocation().X, GetComponentLocation().Y, GroundZ) + Offset;
				UGameplayStatics::PlaySoundAtLocation(this, Hit, Loc, FMath::Clamp(Local.HailDiameter / 40.f, 0.2f, 1.f), Rand.FRandRange(0.85f, 1.2f));
			}
		}
		else
		{
			HailSoundAccum = 0.f;
		}
	}
}

void UAerosPrecipitationComponent::UpdateNiagara()
{
	const float Intensity = FMath::Clamp(FMath::Sqrt(Local.PrecipitationRate / 50.f), 0.f, 1.f) * DensityScale;
	auto Drive = [this](UNiagaraComponent* C, float Value)
	{
		if (!C || !C->GetAsset()) return;
		if (Value > 0.005f && !C->IsActive()) C->Activate();
		if (Value <= 0.005f && C->IsActive()) C->Deactivate();
		C->SetVariableFloat(TEXT("Intensity"), Value);
		C->SetVariableVec3(TEXT("Wind"), Local.Wind);
		C->SetVariableFloat(TEXT("HailSize"), Local.HailDiameter);
	};
	const EAerosPrecipType T = Local.PrecipitationType;
	Drive(RainVFX, (T == EAerosPrecipType::Rain || T == EAerosPrecipType::FreezingRain || T == EAerosPrecipType::Hail) ? Intensity : (T == EAerosPrecipType::Sleet ? Intensity * 0.5f : 0.f));
	Drive(SnowVFX, (T == EAerosPrecipType::Snow) ? Intensity : (T == EAerosPrecipType::Sleet ? Intensity * 0.5f : 0.f));
	Drive(HailVFX, T == EAerosPrecipType::Hail ? Intensity : 0.f);
}

void UAerosPrecipitationComponent::UpdateGroundHeight()
{
	FHitResult Hit;
	const FVector L = GetComponentLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosPrecipGround), false, GetOwner());
	if (GetWorld()->LineTraceSingleByChannel(Hit, L, L - FVector(0, 0, 500000.0), ECC_WorldStatic, Params))
	{
		GroundZ = float(Hit.ImpactPoint.Z);
	}
	else
	{
		GroundZ = float(L.Z) - 20000.f;
	}
}

void UAerosPrecipitationComponent::Respawn(FParticle& P, bool bSolid, bool bAnyHeight)
{
	const FVector C = GetComponentLocation();
	const float A = Rand.FRandRange(0.f, 2.f * PI);
	const float R = Radius * FMath::Sqrt(Rand.FRand());
	const float Top = float(C.Z) + 1500.f;
	const float Z = bAnyHeight ? Rand.FRandRange(FMath::Max(GroundZ, float(C.Z) - 1500.f), Top) : Top + Rand.FRandRange(0.f, 300.f);
	P.Pos = FVector(C.X + FMath::Cos(A) * R, C.Y + FMath::Sin(A) * R, Z);
	P.bSolid = bSolid;
	P.bAlive = true;
	P.Size = Rand.FRandRange(0.8f, 1.2f);
}

void UAerosPrecipitationComponent::UpdateFallback(float DeltaTime)
{
	const EAerosPrecipType T = Local.PrecipitationType;
	const float Intensity = FMath::Clamp(FMath::Sqrt(Local.PrecipitationRate / 50.f), 0.f, 1.f) * DensityScale;
	const int32 Active = FMath::Clamp(int32(Particles.Num() * Intensity), 0, Particles.Num());
	const bool bSnow = T == EAerosPrecipType::Snow;
	const bool bHail = T == EAerosPrecipType::Hail;
	const bool bMixed = T == EAerosPrecipType::Sleet;
	const FVector Center = GetComponentLocation();
	const FVector Wind = Local.Wind * 100.0; // cm/s
	const float HailMM = FMath::Max(Local.HailDiameter, 5.f);
	const float HailVt = Aeros::Phys::HailTerminalVelocity(HailMM) * 100.f;
	const float Time = GetWorld()->GetTimeSeconds();
	const FTransform Hidden(FQuat::Identity, FVector(Center.X, Center.Y, -1e7), FVector::ZeroVector);

	for (int32 K = 0; K < Particles.Num(); ++K)
	{
		FParticle& P = Particles[K];
		if (K >= Active)
		{
			P.bAlive = false;
			RainTransforms[K] = Hidden;
			SolidTransforms[K] = Hidden;
			continue;
		}

		// Hail is mixed into heavy rain: ~1 stone per 6 drops
		const bool bWantSolid = bSnow || (bMixed && (K & 1)) || (bHail && (K % 6 == 0));
		if (!P.bAlive || P.bSolid != bWantSolid) Respawn(P, bWantSolid, true);

		FVector Vel;
		if (!P.bSolid)          Vel = FVector(0, 0, -900.f) + Wind * 0.8;
		else if (bSnow || bMixed) Vel = FVector(FMath::Sin(Time * 1.7f + K) * 40.f, FMath::Cos(Time * 1.3f + K) * 40.f, -110.f) + Wind;
		else                    Vel = FVector(0, 0, -HailVt) + Wind * 0.5;
		P.Pos += Vel * DeltaTime;

		// Wrap around the viewer so the volume follows the camera
		FVector D = P.Pos - Center;
		if (FMath::Abs(D.X) > Radius) P.Pos.X -= FMath::Sign(D.X) * 2.f * Radius;
		if (FMath::Abs(D.Y) > Radius) P.Pos.Y -= FMath::Sign(D.Y) * 2.f * Radius;
		if (P.Pos.Z < GroundZ) Respawn(P, P.bSolid, false);

		if (!P.bSolid)
		{
			const FVector Dir = Vel.GetSafeNormal();
			RainTransforms[K] = FTransform(FRotationMatrix::MakeFromZ(Dir).ToQuat(), P.Pos, FVector(0.012f, 0.012f, 0.45f * P.Size));
			SolidTransforms[K] = Hidden;
		}
		else
		{
			const float S = (bSnow || bMixed) ? 0.018f * P.Size : HailMM * 0.001f * P.Size;
			SolidTransforms[K] = FTransform(FQuat::Identity, P.Pos, FVector(S));
			RainTransforms[K] = Hidden;
		}
	}
	FallbackRain->BatchUpdateInstancesTransforms(0, RainTransforms, true, true, true);
	FallbackSolid->BatchUpdateInstancesTransforms(0, SolidTransforms, true, true, true);
}
