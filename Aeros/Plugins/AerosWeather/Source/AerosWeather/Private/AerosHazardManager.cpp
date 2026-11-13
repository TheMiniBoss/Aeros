// Copyright Aeros. All Rights Reserved.

#include "AerosHazardManager.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosEarthquake.h"
#include "AerosVolcano.h"
#include "AerosStormSystem.h"
#include "Core/AerosPhysics.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"

void UAerosHazardManager::Initialize(UAerosWeatherSubsystem* InOwner)
{
	Owner = InOwner;
}

void UAerosHazardManager::Shutdown()
{
	Earthquakes.Reset();
	Volcanoes.Reset();
	Owner = nullptr;
}

AAerosEarthquake* UAerosHazardManager::SpawnEarthquake(const FVector& Epicenter, float Magnitude, float DepthKm, bool bAftershock)
{
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World || !Owner->HasAuthority()) return nullptr;
	AAerosEarthquake* Q = World->SpawnActorDeferred<AAerosEarthquake>(AAerosEarthquake::StaticClass(), FTransform(FVector(Epicenter.X, Epicenter.Y, 0.0)),
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Q) return nullptr;
	Q->ServerInitialize(Magnitude, DepthKm, bAftershock);
	Q->FinishSpawning(FTransform(FVector(Epicenter.X, Epicenter.Y, 0.0)));
	return Q;
}

float UAerosHazardManager::GetShakingAt(const FVector& Location) const
{
	// Combine as root-sum-square (independent wave trains)
	float Sum2 = 0.f;
	for (const TObjectPtr<AAerosEarthquake>& Q : Earthquakes)
	{
		if (IsValid(Q)) Sum2 += FMath::Square(Q->GetShakingAt(Location));
	}
	return FMath::Sqrt(Sum2);
}

TArray<AAerosVolcano*> UAerosHazardManager::GetVolcanoes() const
{
	TArray<AAerosVolcano*> Out;
	for (const TObjectPtr<AAerosVolcano>& V : Volcanoes) if (IsValid(V)) Out.Add(V);
	return Out;
}

TArray<AAerosEarthquake*> UAerosHazardManager::GetEarthquakes() const
{
	TArray<AAerosEarthquake*> Out;
	for (const TObjectPtr<AAerosEarthquake>& Q : Earthquakes) if (IsValid(Q)) Out.Add(Q);
	return Out;
}

void UAerosHazardManager::RegisterEarthquake(AAerosEarthquake* Quake) { Earthquakes.AddUnique(Quake); }
void UAerosHazardManager::UnregisterEarthquake(AAerosEarthquake* Quake) { Earthquakes.Remove(Quake); }
void UAerosHazardManager::RegisterVolcano(AAerosVolcano* Volcano) { Volcanoes.AddUnique(Volcano); }
void UAerosHazardManager::UnregisterVolcano(AAerosVolcano* Volcano) { Volcanoes.Remove(Volcano); }

void UAerosHazardManager::Tick(float DeltaTime)
{
	if (!Owner) return;
	if (Owner->HasAuthority())
	{
		UpdateSpontaneousSeismicity();
		UpdateFloodPhysics(DeltaTime);
	}
	ModifierTimer -= DeltaTime;
	if (ModifierTimer <= 0.f)
	{
		ModifierTimer = 1.f;
		EnsureCameraModifiers();
	}
}

void UAerosHazardManager::EnsureCameraModifiers()
{
	UWorld* World = Owner->GetWorld();
	if (!World || UAerosWeatherSettings::Get()->CameraShakeScale <= 0.f) return;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC || !PC->IsLocalController() || !PC->PlayerCameraManager) continue;
		if (!PC->PlayerCameraManager->FindCameraModifierByClass(UAerosShakeCameraModifier::StaticClass()))
		{
			PC->PlayerCameraManager->AddNewCameraModifier(UAerosShakeCameraModifier::StaticClass());
		}
	}
}

void UAerosHazardManager::UpdateSpontaneousSeismicity()
{
	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();
	const FAerosSimulationSettings& Sim = Owner->GetSimulationSettings();
	if (!Settings->bSpontaneousEarthquakes || Sim.SeismicZones.Num() == 0) return;

	// One roll per simulated minute per zone (deterministic from the seed)
	const int64 Minute = int64(Owner->GetSimulationTime() / 60.0);
	if (LastSeismicMinute < 0) LastSeismicMinute = Minute;
	const int64 First = FMath::Max(LastSeismicMinute + 1, Minute - 30); // skip backlog at huge time scales
	for (int64 M = First; M <= Minute; ++M)
	{
		for (int32 Z = 0; Z < Sim.SeismicZones.Num(); ++Z)
		{
			const FAerosSeismicZone& Zone = Sim.SeismicZones[Z];
			const float P = Zone.EventsPerDay / 1440.f;
			if (Aeros::Phys::Hash01(uint32(Sim.Seed), uint32(M), uint32(Z), 0x5E15) >= P) continue;
			// Gutenberg-Richter with b = 1 above M4
			const float U = FMath::Max(Aeros::Phys::Hash01(uint32(Sim.Seed), uint32(M), uint32(Z), 0x6A), 1e-4f);
			const float Mag = FMath::Min(4.f - FMath::LogX(10.f, U), Zone.MaxMagnitude);
			const float A = Aeros::Phys::Hash01(uint32(Sim.Seed), uint32(M), uint32(Z), 0x7B) * 2.f * PI;
			const float R = FMath::Sqrt(Aeros::Phys::Hash01(uint32(Sim.Seed), uint32(M), uint32(Z), 0x8C)) * Zone.RadiusKm * 100000.f;
			SpawnEarthquake(Zone.Center + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 0.0), Mag, Zone.DepthKm);
		}
	}
	LastSeismicMinute = Minute;
}

void UAerosHazardManager::UpdateFloodPhysics(float DeltaTime)
{
	FloodTimer += DeltaTime;
	if (FloodTimer < 0.05f) return;
	const float Step = FloodTimer;
	FloodTimer = 0.f;

	UWorld* World = Owner->GetWorld();
	if (!World) return;
	const float Radius = UAerosWeatherSettings::Get()->PhysicsEffectRadiusKm * 100000.f;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr;
		if (!Pawn) continue;
		const FAerosWeatherSample Here = Owner->GetWeatherAtLocation(Pawn->GetActorLocation());
		if (Here.WaterSurfaceZ < -1e6f) continue; // no flood / sea anomaly here
		const double WaterZ = Here.WaterSurfaceZ;

		TArray<FOverlapResult> Overlaps;
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_PhysicsBody);
		Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
		Obj.AddObjectTypesToQuery(ECC_Pawn);
		Obj.AddObjectTypesToQuery(ECC_Vehicle);
		World->OverlapMultiByObjectType(Overlaps, Pawn->GetActorLocation(), FQuat::Identity, Obj, FCollisionShape::MakeSphere(Radius));

		TSet<AActor*> Done;
		for (const FOverlapResult& O : Overlaps)
		{
			UPrimitiveComponent* Prim = O.GetComponent();
			AActor* Actor = O.GetActor();
			if (!Prim || !Actor) continue;
			const FBox Box = Prim->Bounds.GetBox();
			if (Box.Min.Z >= WaterZ) continue;
			const float Submerged = float(FMath::Clamp((WaterZ - Box.Min.Z) / FMath::Max(Box.Max.Z - Box.Min.Z, 1.0), 0.0, 1.0));
			const FVector Current = Here.FloodFlow;

			if (Prim->IsSimulatingPhysics())
			{
				const FVector V = Prim->GetPhysicsLinearVelocity() * 0.01;
				const float Density = Prim->GetMass() / FMath::Max(float(Box.GetVolume()) * 1e-6f, 0.01f); // kg/m3
				const float Buoyancy = FMath::Clamp(1000.f / FMath::Max(Density, 50.f), 0.f, 4.f) * 9.81f * Submerged;
				FVector Accel(0, 0, Buoyancy);
				Accel += (Current - V) * 1.5f * Submerged; // drag toward the current
				Accel.Z -= V.Z * 1.5f * Submerged;         // water damping
				Prim->AddForce(Accel * 100.0, NAME_None, true);
			}
			else if (ACharacter* Char = Cast<ACharacter>(Actor))
			{
				if (Done.Contains(Actor)) continue;
				Done.Add(Actor);
				if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
				{
					Move->AddForce(Current * 100.0 * Move->Mass * 0.8f * Submerged);
				}
				// Fast deep water knocks people over (about 0.5 m at 2 m/s)
				const float Danger = Here.FloodDepth * Current.Size();
				if (Danger > 1.f) UGameplayStatics::ApplyDamage(Char, (Danger - 1.f) * 5.f * Step, nullptr, nullptr, UAerosFloodDamageType::StaticClass());
			}
		}
	}
}
