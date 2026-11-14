// Copyright Aeros. All Rights Reserved.

#include "AerosStormManager.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosWeatherModule.h"
#include "AerosStormSystem.h"
#include "AerosLightning.h"
#include "AerosTornado.h"
#include "AerosHurricane.h"
#include "Core/AerosPhysics.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

void UAerosStormManager::Initialize(UAerosWeatherSubsystem* InOwner)
{
	Owner = InOwner;
}

void UAerosStormManager::Shutdown()
{
	ClearActors();
	Owner = nullptr;
}

void UAerosStormManager::ClearActors()
{
	if (!Owner || !Owner->HasAuthority()) return;
	auto DestroyAll = [](auto& Array)
	{
		auto Copy = Array;
		for (auto& A : Copy) if (IsValid(A)) A->Destroy();
		Array.Reset();
	};
	DestroyAll(StormActors);
	DestroyAll(Tornadoes);
	DestroyAll(Hurricanes);
	LastTornadoCheck.Reset();
}

bool UAerosStormManager::IsNearAnyPlayer(const FVector& Location, float RadiusKm) const
{
	const UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World) return false;
	const double R2 = FMath::Square(RadiusKm * 100000.0);
	bool bAnyPlayer = false;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		const APawn* P = PC ? PC->GetPawn() : nullptr;
		if (!P) continue;
		bAnyPlayer = true;
		if (FVector::DistSquared2D(P->GetActorLocation(), Location) < R2) return true;
	}
	if (!bAnyPlayer) return FVector::DistSquared2D(Owner->GetViewerLocation(), Location) < R2;
	return false;
}

void UAerosStormManager::Tick(float DeltaTime)
{
	if (!Owner) return;
	UpdateTimer += DeltaTime;
	if (UpdateTimer < 0.25f) return;
	UpdateTimer = 0.f;

	if (Owner->HasAuthority())
	{
		UpdateStormActors();
		UpdateTornadoGenesis();
		UpdateHurricanes();
	}
}

// =====================================================================
//  Storm actors (server, LOD by distance to players)
// =====================================================================

AAerosStormSystem* UAerosStormManager::FindStormActor(int32 StormId) const
{
	for (const TObjectPtr<AAerosStormSystem>& A : StormActors)
	{
		if (IsValid(A) && A->GetStormId() == StormId) return A;
	}
	return nullptr;
}

void UAerosStormManager::UpdateStormActors()
{
	FAerosSnapshotPtr Snap = Owner->GetSnapshot();
	UWorld* World = Owner->GetWorld();
	if (!Snap || !World) return;
	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();

	TSet<int32> Wanted;
	for (const FAerosStormData& S : Snap->Storms)
	{
		const bool bImportant = S.Intensity >= Settings->StormActorMinIntensity || S.Type == EAerosStormType::Supercell;
		if (!bImportant || !IsNearAnyPlayer(S.Location, Settings->StormActorRadiusKm)) continue;
		Wanted.Add(S.Id);

		AAerosStormSystem* Actor = FindStormActor(S.Id);
		if (!Actor)
		{
			UClass* Cls = Settings->StormActorClass.LoadSynchronous();
			if (!Cls) Cls = AAerosStormSystem::StaticClass();
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Actor = World->SpawnActor<AAerosStormSystem>(Cls, FTransform(S.Location), Params);
		}
		if (Actor) Actor->ServerUpdateFromSim(S);
	}

	for (int32 K = StormActors.Num() - 1; K >= 0; --K)
	{
		AAerosStormSystem* A = StormActors[K];
		if (!IsValid(A)) { StormActors.RemoveAt(K); continue; }
		if (!Wanted.Contains(A->GetStormId())) A->Destroy();
	}
}

// =====================================================================
//  Tornado genesis (deterministic roll, roadmap ~15% when critical)
// =====================================================================

void UAerosStormManager::UpdateTornadoGenesis()
{
	FAerosSnapshotPtr Snap = Owner->GetSnapshot();
	if (!Snap) return;
	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();
	if (Settings->TornadoFrequency <= 0.f) return;

	for (const FAerosStormData& S : Snap->Storms)
	{
		const bool bCritical = (S.Mesocyclone > 0.5f && S.STP > 0.5f) || (S.CAPE > 2500.f && S.SRH03 > 250.f && S.Mesocyclone > 0.3f);
		if (!bCritical) continue;

		const int64 CheckIndex = int64(Snap->SimTime / Settings->TornadoCheckInterval);
		int64& Last = LastTornadoCheck.FindOrAdd(S.Id, -1);
		if (Last == CheckIndex) continue;
		Last = CheckIndex;

		bool bHasTornado = false;
		for (const TObjectPtr<AAerosTornado>& T : Tornadoes)
		{
			if (IsValid(T) && T->GetParentStormId() == S.Id) { bHasTornado = true; break; }
		}
		if (bHasTornado || !IsNearAnyPlayer(S.Location, Settings->StormActorRadiusKm)) continue;

		const int32 Seed = Owner->GetSimulationSettings().Seed;
		const float Roll = Aeros::Phys::Hash01(uint32(Seed), uint32(S.Id), uint32(CheckIndex), 0x70AD0);
		const float Chance = FMath::Clamp(0.15f * Settings->TornadoFrequency * FMath::Clamp(S.STP, 0.5f, 3.f), 0.f, 0.9f);
		if (Roll < Chance)
		{
			SpawnTornado(S, Aeros::Phys::Hash01(uint32(Seed), uint32(S.Id), uint32(CheckIndex), 1),
			                Aeros::Phys::Hash01(uint32(Seed), uint32(S.Id), uint32(CheckIndex), 2));
		}
	}
}

AAerosTornado* UAerosStormManager::SpawnTornado(const FAerosStormData& Storm, float RandA, float RandB)
{
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World || !Owner->HasAuthority()) return nullptr;
	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();

	UClass* Cls = Settings->TornadoActorClass.LoadSynchronous();
	if (!Cls) Cls = AAerosTornado::StaticClass();

	// Tornado sits under the rear flank of the updraft (mesocyclone)
	const FVector Motion = Storm.Velocity.GetSafeNormal2D();
	const FVector Location = Storm.UpdraftLocation - Motion * 300000.0 + FVector(-Motion.Y, Motion.X, 0.0) * 150000.0;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AAerosTornado* T = World->SpawnActorDeferred<AAerosTornado>(Cls, FTransform(Location), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!T) return nullptr;

	// Strength: most tornadoes are weak (EF0-EF1), violent ones are rare even in a high-STP
	// environment. RandA^4 skews the roll toward weak; STP caps how strong it can get.
	const float Potential = FMath::Clamp(Storm.STP / 4.f, 0.25f, 1.f);
	const float MaxWind = FMath::Clamp(30.f + 75.f * FMath::Pow(RandA, 4.f) * Potential, 30.f, 105.f);
	const float Core = FMath::Lerp(40.f, 250.f, FMath::Clamp((MaxWind - 30.f) / 70.f, 0.f, 1.f)) * (0.7f + 0.6f * RandB);
	const float Life = 120.f + 900.f * RandB * FMath::Clamp(Storm.STP / 2.f, 0.3f, 1.f);
	const FVector TrackVel = Storm.Velocity.Size2D() > 1.0 ? Storm.Velocity : FVector(10, 0, 0);
	T->ServerInitialize(Storm.Id, MaxWind, Core, Life, TrackVel, int32(RandA * 100000.f));
	T->FinishSpawning(FTransform(Location));

	UE_LOG(LogAeros, Log, TEXT("Aeros: tornado EF%d (%.0f m/s, core %.0f m) under storm %d"), T->GetEFRating(), MaxWind, Core, Storm.Id);
	return T;
}

AAerosTornado* UAerosStormManager::ForceTornado(int32 StormId)
{
	FAerosSnapshotPtr Snap = Owner ? Owner->GetSnapshot() : nullptr;
	if (!Snap || !Owner->HasAuthority()) return nullptr;

	const FAerosStormData* Best = nullptr;
	for (const FAerosStormData& S : Snap->Storms)
	{
		if (StormId >= 0 ? S.Id == StormId : (!Best || S.Intensity + S.Mesocyclone > Best->Intensity + Best->Mesocyclone)) Best = &S;
	}
	if (Best)
	{
		FAerosStormData S = *Best;
		S.STP = FMath::Max(S.STP, 1.5f);
		return SpawnTornado(S, FMath::FRandRange(0.75f, 1.f), FMath::FRand()); // a forced tornado should be worth watching
	}

	// No storm: spawn near the viewer so the feature can be tested anywhere
	FAerosStormData Fake;
	Fake.Id = -1;
	Fake.STP = 2.f;
	Fake.Velocity = FVector(12, -4, 0);
	Fake.UpdraftLocation = Owner->GetViewerLocation() + FVector(300000.0, 0, 0);
	return SpawnTornado(Fake, 0.6f, 0.6f);
}

// =====================================================================
//  Hurricanes
// =====================================================================

void UAerosStormManager::UpdateHurricanes()
{
	FAerosSnapshotPtr Snap = Owner->GetSnapshot();
	UWorld* World = Owner->GetWorld();
	if (!Snap || !World) return;

	TSet<int32> Alive;
	for (const FAerosCycloneData& C : Snap->Cyclones)
	{
		Alive.Add(C.Id);
		AAerosHurricane* H = nullptr;
		for (const TObjectPtr<AAerosHurricane>& Existing : Hurricanes)
		{
			if (IsValid(Existing) && Existing->CycloneData.Id == C.Id) { H = Existing; break; }
		}
		if (!H)
		{
			UClass* Cls = UAerosWeatherSettings::Get()->HurricaneActorClass.LoadSynchronous();
			if (!Cls) Cls = AAerosHurricane::StaticClass();
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			H = World->SpawnActor<AAerosHurricane>(Cls, FTransform(C.Location), Params);
		}
		if (H) H->ServerUpdateFromSim(C);
	}
	for (int32 K = Hurricanes.Num() - 1; K >= 0; --K)
	{
		AAerosHurricane* H = Hurricanes[K];
		if (!IsValid(H)) { Hurricanes.RemoveAt(K); continue; }
		if (!Alive.Contains(H->CycloneData.Id)) H->Destroy();
	}
}

// =====================================================================
//  Lightning (cosmetic, every machine)
// =====================================================================

void UAerosStormManager::HandleLightning(const FAerosLightningStrike& Strike)
{
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World || World->GetNetMode() == NM_DedicatedServer) return;
	const float Range = UAerosWeatherSettings::Get()->LightningVisualRangeKm * 100000.f;
	const FVector Viewer = Owner->GetViewerLocation();
	if (FVector::DistSquared2D(Strike.Location, Viewer) > FMath::Square(Range)) return;

	// At most ~4 flashes per real second (accelerated time shows hundreds per second: actor spam and a
	// sky whitened by overlapping flashes). Close strikes (< 8 km) may borrow ahead: they matter most.
	const double Now = World->GetRealTimeSeconds();
	FlashTokens = FMath::Min(FlashTokens + float(Now - LastFlashRefill) * 4.f, 3.f);
	LastFlashRefill = Now;
	const bool bClose = FVector::DistSquared2D(Strike.Location, Viewer) < FMath::Square(800000.0);
	if (FlashTokens < (bClose ? -1.f : 1.f)) return;
	FlashTokens -= 1.f;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	if (AAerosLightningFlash* Flash = World->SpawnActor<AAerosLightningFlash>(Params))
	{
		Flash->Fire(Strike, Viewer);
		static int32 Spawned = 0;
		if (++Spawned % 25 == 1) UE_LOG(LogTemp, Display, TEXT("Aeros lightning: %d flashes shown so far"), Spawned);
	}
}

// =====================================================================
//  Queries / registration
// =====================================================================

FVector UAerosStormManager::GetVortexWindAt(const FVector& WorldLocation) const
{
	FVector Sum = FVector::ZeroVector;
	for (const TObjectPtr<AAerosTornado>& T : Tornadoes)
	{
		if (IsValid(T)) Sum += T->GetWindAt(WorldLocation);
	}
	return Sum;
}

bool UAerosStormManager::IsInsideTornado(const FVector& WorldLocation, float RadiusMultiplier) const
{
	for (const TObjectPtr<AAerosTornado>& T : Tornadoes)
	{
		if (IsValid(T) && FVector::Dist2D(T->GetActorLocation(), WorldLocation) < T->GetCoreRadius() * 100.f * RadiusMultiplier) return true;
	}
	return false;
}

TArray<AAerosTornado*> UAerosStormManager::GetTornadoes() const
{
	TArray<AAerosTornado*> Out;
	for (const TObjectPtr<AAerosTornado>& T : Tornadoes) if (IsValid(T)) Out.Add(T);
	return Out;
}

void UAerosStormManager::RegisterStormActor(AAerosStormSystem* Actor) { StormActors.AddUnique(Actor); }
void UAerosStormManager::UnregisterStormActor(AAerosStormSystem* Actor) { StormActors.Remove(Actor); }
void UAerosStormManager::RegisterTornado(AAerosTornado* Tornado) { Tornadoes.AddUnique(Tornado); }
void UAerosStormManager::UnregisterTornado(AAerosTornado* Tornado) { Tornadoes.Remove(Tornado); }
void UAerosStormManager::RegisterHurricane(AAerosHurricane* Hurricane) { Hurricanes.AddUnique(Hurricane); }
void UAerosStormManager::UnregisterHurricane(AAerosHurricane* Hurricane) { Hurricanes.Remove(Hurricane); }
