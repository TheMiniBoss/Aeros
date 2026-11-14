// Copyright Aeros. All Rights Reserved.

#include "AerosWeatherSubsystem.h"
#include "AerosWeatherModule.h"
#include "AerosWeatherSettings.h"
#include "AerosSimulationThread.h"
#include "AerosStormManager.h"
#include "AerosCloudManager.h"
#include "AerosWeatherNetState.h"
#include "AerosWeatherController.h"
#include "AerosTornado.h"
#include "AerosHazardManager.h"
#include "AerosEarthquake.h"
#include "AerosVolcano.h"
#include "AerosLiveWeather.h"
#include "Core/AerosPhysics.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarAerosDebug(
	TEXT("aeros.Debug"), 0,
	TEXT("Aeros debug draw. 0 = off, 1 = storms, 2 = storms + grid (wind arrows coloured by reflectivity)."),
	ECVF_Cheat);

UAerosWeatherSubsystem::UAerosWeatherSubsystem() = default;
UAerosWeatherSubsystem::~UAerosWeatherSubsystem() = default; // FAerosSimulationThread is complete here

UAerosWeatherSubsystem* UAerosWeatherSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<UAerosWeatherSubsystem>() : nullptr;
}

bool UAerosWeatherSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) return false;
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UAerosWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	StormManager = NewObject<UAerosStormManager>(this);
	CloudManager = NewObject<UAerosCloudManager>(this);
	HazardManager = NewObject<UAerosHazardManager>(this);
	LiveWeather = NewObject<UAerosLiveWeather>(this);
	StormManager->Initialize(this);
	CloudManager->Initialize(this);
	HazardManager->Initialize(this);
	LiveWeather->Initialize(this);
	ActiveSettings = UAerosWeatherSettings::Get()->DefaultSimulation;
}

void UAerosWeatherSubsystem::Deinitialize()
{
	StopSimulation();
	if (StormManager) StormManager->Shutdown();
	if (CloudManager) CloudManager->Shutdown();
	if (HazardManager) HazardManager->Shutdown();
	if (LiveWeather) LiveWeather->Shutdown();
	Super::Deinitialize();
}

void UAerosWeatherSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!HasAuthority()) return; // clients wait for the replicated net state

	bool bHasController = false;
	for (TActorIterator<AAerosWeatherController> It(&InWorld); It; ++It)
	{
		bHasController = true;
		break;
	}
	if (!bHasController && UAerosWeatherSettings::Get()->bAutoStart && !IsSimulationRunning())
	{
		StartSimulation(UAerosWeatherSettings::Get()->DefaultSimulation);
	}
}

TStatId UAerosWeatherSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UAerosWeatherSubsystem, STATGROUP_Aeros);
}

bool UAerosWeatherSubsystem::HasAuthority() const
{
	const UWorld* World = GetWorld();
	return World && World->GetNetMode() != NM_Client;
}

// =====================================================================
//  Lifecycle
// =====================================================================

void UAerosWeatherSubsystem::StartSimulation(const FAerosSimulationSettings& Settings)
{
	StopSimulation();

	ActiveSettings = Settings;
	SimThread = MakeUnique<FAerosSimulationThread>(ActiveSettings);
	if (UAerosWeatherSettings::Get()->bUseWorkerThread)
	{
		SimThread->Start();
	}
	TargetTime = 0.0;
	DisplayTime = 0.0;
	Current = SimThread->GetLatestSnapshot();
	Previous = Current;
	LastLightningSeq = 0;
	PendingStrikes.Reset();
	KnownStorms.Reset();
	KnownCyclones.Reset();
	TerrainCenter = FVector(1e30, 0, 0); // forces a terrain sampling pass
	TerrainCursor = -1;

	UWorld* World = GetWorld();
	if (World && HasAuthority() && World->GetNetMode() != NM_Standalone)
	{
		if (!NetState)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			NetState = World->SpawnActor<AAerosWeatherNetState>(Params);
		}
		if (NetState) NetState->ServerInitialize(ActiveSettings);
	}

	UE_LOG(LogAeros, Log, TEXT("Aeros simulation started: %dx%d cells, %.1f km, scenario %d, seed %d, %s."),
		ActiveSettings.GridSizeX, ActiveSettings.GridSizeY, ActiveSettings.CellSizeKm, int32(ActiveSettings.Scenario),
		ActiveSettings.Seed, SimThread->IsThreaded() ? TEXT("worker thread") : TEXT("game thread"));
}

void UAerosWeatherSubsystem::StopSimulation()
{
	if (StormManager) StormManager->ClearActors();
	SimThread.Reset(); // joins the worker
	Current.Reset();
	Previous.Reset();
}

bool UAerosWeatherSubsystem::IsSimulationRunning() const
{
	return SimThread.IsValid();
}

// =====================================================================
//  Tick
// =====================================================================

void UAerosWeatherSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!SimThread) return;

	RecenterCooldown -= DeltaTime;
	UpdateViewer();
	UpdateClock(DeltaTime);
	SimThread->SetTargetTime(TargetTime);
	SimThread->TickSynchronous(); // no-op when threaded
	PullSnapshot();
	MaybeFollowViewer();

	UpdateTerrainSampling();
	if (StormManager) StormManager->Tick(DeltaTime);
	if (HazardManager) HazardManager->Tick(DeltaTime);
	if (CloudManager) CloudManager->Tick(DeltaTime);
	if (LiveWeather) LiveWeather->Tick(DeltaTime);
	BroadcastEvents();

	// ---- Debug draw ---------------------------------------------------------------
	const int32 Debug = CVarAerosDebug.GetValueOnGameThread();
	if (Debug > 0 && Current && Current->IsValid())
	{
		UWorld* World = GetWorld();
		for (const FAerosStormData& S : Current->Storms)
		{
			const FColor Col = S.Type == EAerosStormType::Supercell ? FColor::Magenta : (S.MaxHail > 0.f ? FColor::Yellow : FColor::Green);
			DrawDebugCircle(World, S.Location + FVector(0, 0, 50000), S.RadiusKm * 100000.f, 32, Col, false, -1.f, 0, 400.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			DrawDebugString(World, S.Location + FVector(0, 0, 60000), FString::Printf(TEXT("Storm %d  %.0f dBZ  W %.0f  hail %.0f mm"), S.Id, S.MaxReflectivity, S.MaxUpdraft, S.MaxHail), nullptr, Col, 0.f);
		}
		if (Debug > 1)
		{
			float GX, GY;
			Current->WorldToGrid(ViewerLocation, GX, GY);
			const int32 R = 12;
			for (int32 J = int32(GY) - R; J <= int32(GY) + R; J += 2)
			{
				for (int32 I = int32(GX) - R; I <= int32(GX) + R; I += 2)
				{
					if (I < 0 || J < 0 || I >= Current->NX || J >= Current->NY) continue;
					const FAerosSnapCell& C = Current->At(I, J);
					const FVector P = Current->GridToWorld(I + 0.5f, J + 0.5f) + FVector(0, 0, ViewerLocation.Z + 2000.0);
					const FVector W = FAerosSnapshot::SimToWorldDir(C.Us, C.Vs);
					const uint8 Level = uint8(FMath::Clamp((C.DBZ + 10.f) / 75.f, 0.f, 1.f) * 255.f);
					DrawDebugDirectionalArrow(World, P, P + W * 3000.0, 2000.f, FColor(Level, 255 - Level, 64), false, -1.f, 0, 150.f);
				}
			}
		}
	}
}

void UAerosWeatherSubsystem::UpdateViewer()
{
	const UWorld* World = GetWorld();
	if (!World) return;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		if (!PC) continue;
		if (PC->IsLocalController() && PC->PlayerCameraManager)
		{
			ViewerLocation = PC->PlayerCameraManager->GetCameraLocation();
			return;
		}
		if (const APawn* Pawn = PC->GetPawn())
		{
			ViewerLocation = Pawn->GetActorLocation();
			return;
		}
	}
}

void UAerosWeatherSubsystem::UpdateClock(float DeltaTime)
{
	const double SimNow = SimThread->GetSimTime();
	if (!bPaused)
	{
		TargetTime += double(DeltaTime) * ActiveSettings.TimeScale;
	}
	// Do not run away when the CPU cannot keep up: hold the clock back
	TargetTime = FMath::Min(TargetTime, SimNow + FMath::Max<double>(ActiveSettings.MaxLagSeconds, SimThread->GetStepSeconds() * 2.0));
}

void UAerosWeatherSubsystem::PullSnapshot()
{
	FAerosSnapshotPtr Latest = SimThread->GetLatestSnapshot();
	if (Latest && (!Current || Latest.Get() != Current.Get()))
	{
		Previous = Current ? Current : Latest;
		Current = Latest;
	}
	if (!Current) return;

	// Display one step behind the target so we can blend between two published states
	const double Lo = Previous ? Previous->SimTime : Current->SimTime;
	const double Hi = Current->SimTime;
	DisplayTime = FMath::Clamp(TargetTime - SimThread->GetStepSeconds(), Lo, Hi);
	SnapshotAlpha = Hi > Lo ? float((DisplayTime - Lo) / (Hi - Lo)) : 1.f;
}

void UAerosWeatherSubsystem::MaybeFollowViewer()
{
	if (!ActiveSettings.bFollowPlayer || !HasAuthority() || !Current || RecenterCooldown > 0.f) return;
	const double Half = 0.25 * FMath::Min(Current->NX, Current->NY) * Current->CellM * 100.0;
	if (FMath::Abs(ViewerLocation.X - Current->DomainCenter.X) > Half || FMath::Abs(ViewerLocation.Y - Current->DomainCenter.Y) > Half)
	{
		RecenterDomain(ViewerLocation);
		RecenterCooldown = 5.f;
	}
}

void UAerosWeatherSubsystem::BroadcastEvents()
{
	if (!Current) return;

	// Storms formed / dissipated
	TSet<int32> Now;
	for (const FAerosStormData& S : Current->Storms)
	{
		Now.Add(S.Id);
		if (!KnownStorms.Contains(S.Id) && S.Intensity > 0.2f)
		{
			KnownStorms.Add(S.Id);
			OnStormFormed.Broadcast(S);
		}
	}
	for (auto It = KnownStorms.CreateIterator(); It; ++It)
	{
		if (!Now.Contains(*It))
		{
			OnStormDissipated.Broadcast(*It);
			It.RemoveCurrent();
		}
	}

	for (const FAerosCycloneData& C : Current->Cyclones)
	{
		if (!KnownCyclones.Contains(C.Id))
		{
			KnownCyclones.Add(C.Id);
			OnCycloneFormed.Broadcast(C);
		}
	}

	// Lightning (deterministic, identical on all machines). Each strike carries its own simulated
	// time inside the physics step: queue it and fire it when the displayed clock reaches it,
	// so flashes come at random moments instead of in bursts once per step.
	for (const FAerosLightningStrike& L : Current->Lightning)
	{
		if (L.Sequence > LastLightningSeq)
		{
			LastLightningSeq = L.Sequence;
			PendingStrikes.Add(L);
		}
	}
	PendingStrikes.Sort([](const FAerosLightningStrike& A, const FAerosLightningStrike& B) { return A.SimTime < B.SimTime; });
	int32 Fired = 0;
	while (Fired < PendingStrikes.Num() && PendingStrikes[Fired].SimTime <= DisplayTime)
	{
		const FAerosLightningStrike& L = PendingStrikes[Fired++];
		// Strikes far behind the clock (pause, big time jumps) are not worth showing
		if (DisplayTime - L.SimTime > 60.0) continue;
		OnLightningStrike.Broadcast(L);
		if (StormManager) StormManager->HandleLightning(L);
	}
	if (Fired > 0) PendingStrikes.RemoveAt(0, Fired);
	if (PendingStrikes.Num() > 512) PendingStrikes.RemoveAt(0, PendingStrikes.Num() - 512);

	// Local phenomenon change
	PhenomenonCheckTimer -= GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.016f;
	if (PhenomenonCheckTimer <= 0.f)
	{
		PhenomenonCheckTimer = 1.f;
		const EAerosPhenomenon NewPh = GetPhenomenonAtLocation(ViewerLocation);
		if (NewPh != ViewerPhenomenon)
		{
			const EAerosPhenomenon Old = ViewerPhenomenon;
			ViewerPhenomenon = NewPh;
			OnPhenomenonChanged.Broadcast(Old, NewPh);
		}
	}
}

// =====================================================================
//  Commands (network aware)
// =====================================================================

void UAerosWeatherSubsystem::EnqueueCommand(TUniqueFunction<void(FAerosSimulation&)>&& Command)
{
	if (SimThread) SimThread->Enqueue(MoveTemp(Command));
}

void UAerosWeatherSubsystem::IssueCommand(uint8 Type, const FVector& Location, const FVector4& Params)
{
	if (!SimThread) return;
	if (!HasAuthority())
	{
		UE_LOG(LogAeros, Warning, TEXT("Aeros: weather commands must be issued on the server."));
		return;
	}
	double ApplyAt = -1.0;
	if (NetState)
	{
		// Schedule slightly in the future so clients apply it on the same step
		ApplyAt = SimThread->GetSimTime() + SimThread->GetStepSeconds() * 2.0;
		NetState->ServerAddCommand(Type, Location, Params, ApplyAt);
	}
	ApplyNetCommand(Type, Location, Params, ApplyAt);
}

void UAerosWeatherSubsystem::ApplyNetCommand(uint8 Type, const FVector& Location, const FVector4& Params, double ApplyAt)
{
	if (!SimThread) return;
	FAerosSimCommand Cmd;
	switch (static_cast<EAerosNetCommand>(Type))
	{
	case EAerosNetCommand::SetScenario:
	{
		const EAerosScenario Sc = static_cast<EAerosScenario>(int32(Params.X));
		ActiveSettings.Scenario = Sc;
		Cmd = [Sc](FAerosSimulation& Sim) { Sim.SetScenario(Sc); };
		break;
	}
	case EAerosNetCommand::CustomSynoptic:
	{
		const FAerosSynopticState S = ActiveSettings.CustomSynoptic;
		ActiveSettings.Scenario = EAerosScenario::Custom;
		Cmd = [S](FAerosSimulation& Sim) { Sim.SetScenario(EAerosScenario::Custom, &S); };
		break;
	}
	case EAerosNetCommand::ThermalBubble:
		Cmd = [Location, Params](FAerosSimulation& Sim) { Sim.InjectThermal(Location, float(Params.X), float(Params.Y), float(Params.Z)); };
		break;
	case EAerosNetCommand::Hurricane:
		Cmd = [Location, Params](FAerosSimulation& Sim) { Sim.SpawnCyclone(Location, float(Params.X), float(Params.Y)); };
		break;
	case EAerosNetCommand::Recenter:
		Cmd = [Location](FAerosSimulation& Sim) { Sim.Recenter(Location); };
		break;
	case EAerosNetCommand::SetClock:
		Cmd = [Params](FAerosSimulation& Sim) { Sim.SetClock(int32(Params.X), float(Params.Y)); };
		break;
	case EAerosNetCommand::WaterRegions:
	{
		const TArray<FAerosWaterRegion> Regions = ActiveSettings.WaterRegions;
		Cmd = [Regions](FAerosSimulation& Sim) { Sim.SetWaterRegions(Regions); };
		break;
	}
	case EAerosNetCommand::DesertRegions:
	{
		const TArray<FAerosDesertRegion> Regions = ActiveSettings.DesertRegions;
		Cmd = [Regions](FAerosSimulation& Sim) { Sim.SetDesertRegions(Regions); };
		break;
	}
	case EAerosNetCommand::InjectRain:
		Cmd = [Location, Params](FAerosSimulation& Sim) { Sim.InjectRain(Location, float(Params.X), float(Params.Y)); };
		break;
	case EAerosNetCommand::InjectOutflow:
		Cmd = [Location, Params](FAerosSimulation& Sim) { Sim.InjectOutflow(Location, float(Params.X), float(Params.Y)); };
		break;
	case EAerosNetCommand::Tsunami:
		Cmd = [Location, Params](FAerosSimulation& Sim) { Sim.TriggerTsunami(Location, float(Params.X)); };
		OnTsunami.Broadcast(Location, float(Params.X));
		break;
	case EAerosNetCommand::VolcanicEmission:
		Cmd = [Location, Params](FAerosSimulation& Sim) { Sim.AddVolcanicEmitter(Location, float(Params.X), float(Params.Y)); };
		break;
	case EAerosNetCommand::SetLatitude:
	{
		const float Lat = float(Params.X);
		ActiveSettings.LatitudeDeg = Lat;
		Cmd = [Lat](FAerosSimulation& Sim) { Sim.SetLatitude(Lat); };
		break;
	}
	default:
		return;
	}
	SimThread->Enqueue(MoveTemp(Cmd), ApplyAt);
}

void UAerosWeatherSubsystem::RegisterNetState(AAerosWeatherNetState* InNetState)
{
	if (NetState && NetState != InNetState) return;
	NetState = InNetState;
	if (!HasAuthority() && NetState)
	{
		ApplyNetState(*NetState);
	}
}

void UAerosWeatherSubsystem::ApplyNetState(const AAerosWeatherNetState& State)
{
	if (HasAuthority()) return;
	NetState = const_cast<AAerosWeatherNetState*>(&State);

	if (!SimThread || State.Epoch != AppliedEpoch)
	{
		AppliedEpoch = State.Epoch;
		StartSimulation(State.Settings);
		NetState->LastAppliedSequence = 0;
	}
	ActiveSettings.TimeScale = State.Settings.TimeScale;
	ActiveSettings.CustomSynoptic = State.Settings.CustomSynoptic;
	ActiveSettings.WaterRegions = State.Settings.WaterRegions;
	ActiveSettings.DesertRegions = State.Settings.DesertRegions;
	ActiveSettings.SeismicZones = State.Settings.SeismicZones;
	bPaused = State.bServerPaused;

	// Follow the server clock; fast-forward on join (commands are timed, so the replay is exact)
	if (FMath::Abs(TargetTime - State.ServerSimTime) > SimThread->GetStepSeconds() * 2.0)
	{
		TargetTime = State.ServerSimTime;
	}

	for (const FAerosNetCommandData& C : State.Commands)
	{
		if (C.Sequence > NetState->LastAppliedSequence)
		{
			ApplyNetCommand(C.Type, C.Location, C.Params, C.ApplyAt);
			NetState->LastAppliedSequence = C.Sequence;
		}
	}
}

// =====================================================================
//  Public API
// =====================================================================

void UAerosWeatherSubsystem::SetTimeScale(float NewTimeScale)
{
	ActiveSettings.TimeScale = FMath::Clamp(NewTimeScale, 0.f, 100000.f);
}

FAerosDateTime UAerosWeatherSubsystem::GetDateTime() const
{
	return Current ? Current->Clock : FAerosDateTime();
}

void UAerosWeatherSubsystem::SetDateTime(int32 DayOfYear, float Hour)
{
	IssueCommand(uint8(EAerosNetCommand::SetClock), FVector::ZeroVector, FVector4(DayOfYear, Hour, 0, 0));
}

void UAerosWeatherSubsystem::SetScenario(EAerosScenario Scenario)
{
	IssueCommand(uint8(EAerosNetCommand::SetScenario), FVector::ZeroVector, FVector4(float(int32(Scenario)), 0, 0, 0));
}

void UAerosWeatherSubsystem::SetCustomSynoptic(const FAerosSynopticState& State)
{
	if (!HasAuthority()) return;
	ActiveSettings.CustomSynoptic = State;
	if (NetState) NetState->ServerSetSettings(ActiveSettings);
	IssueCommand(uint8(EAerosNetCommand::CustomSynoptic), FVector::ZeroVector, FVector4(0, 0, 0, 0));
}

void UAerosWeatherSubsystem::InjectThermalBubble(FVector WorldLocation, float RadiusKm, float DeltaTemperature, float DeltaMoisture_gkg)
{
	IssueCommand(uint8(EAerosNetCommand::ThermalBubble), WorldLocation, FVector4(RadiusKm, DeltaTemperature, DeltaMoisture_gkg * 0.001f, 0));
}

void UAerosWeatherSubsystem::SpawnHurricane(FVector WorldLocation, float CentralPressure, float RadiusMaxWindKm)
{
	IssueCommand(uint8(EAerosNetCommand::Hurricane), WorldLocation, FVector4(CentralPressure, RadiusMaxWindKm, 0, 0));
}

void UAerosWeatherSubsystem::SetWaterRegions(const TArray<FAerosWaterRegion>& Regions)
{
	if (!HasAuthority()) return;
	ActiveSettings.WaterRegions = Regions;
	if (NetState) NetState->ServerSetSettings(ActiveSettings);
	IssueCommand(uint8(EAerosNetCommand::WaterRegions), FVector::ZeroVector, FVector4(0, 0, 0, 0));
}

void UAerosWeatherSubsystem::RecenterDomain(FVector WorldLocation)
{
	IssueCommand(uint8(EAerosNetCommand::Recenter), WorldLocation, FVector4(0, 0, 0, 0));
}

void UAerosWeatherSubsystem::SetDesertRegions(const TArray<FAerosDesertRegion>& Regions)
{
	if (!HasAuthority()) return;
	ActiveSettings.DesertRegions = Regions;
	if (NetState) NetState->ServerSetSettings(ActiveSettings);
	IssueCommand(uint8(EAerosNetCommand::DesertRegions), FVector::ZeroVector, FVector4(0, 0, 0, 0));
}

void UAerosWeatherSubsystem::SetLatitude(float LatitudeDeg)
{
	IssueCommand(uint8(EAerosNetCommand::SetLatitude), FVector::ZeroVector, FVector4(FMath::Clamp(LatitudeDeg, -89.f, 89.f), 0, 0, 0));
}

AAerosEarthquake* UAerosWeatherSubsystem::TriggerEarthquake(FVector Epicenter, float Magnitude, float DepthKm)
{
	return HazardManager ? HazardManager->SpawnEarthquake(Epicenter, Magnitude, DepthKm) : nullptr;
}

void UAerosWeatherSubsystem::TriggerTsunami(FVector Source, float Magnitude)
{
	IssueCommand(uint8(EAerosNetCommand::Tsunami), Source, FVector4(Magnitude, 0, 0, 0));
}

void UAerosWeatherSubsystem::InjectRain(FVector WorldLocation, float RadiusKm, float Millimetres)
{
	IssueCommand(uint8(EAerosNetCommand::InjectRain), WorldLocation, FVector4(RadiusKm, Millimetres, 0, 0));
}

void UAerosWeatherSubsystem::InjectOutflow(FVector WorldLocation, float RadiusKm, float DeltaTemperature)
{
	IssueCommand(uint8(EAerosNetCommand::InjectOutflow), WorldLocation, FVector4(RadiusKm, FMath::Min(DeltaTemperature, 0.f), 0, 0));
}

void UAerosWeatherSubsystem::StartVolcanicEmission(FVector Vent, float MassRateKgPerSecond, float DurationSeconds)
{
	IssueCommand(uint8(EAerosNetCommand::VolcanicEmission), Vent, FVector4(MassRateKgPerSecond, DurationSeconds, 0, 0));
}

float UAerosWeatherSubsystem::GetGroundShakingAt(FVector WorldLocation) const
{
	return HazardManager ? HazardManager->GetShakingAt(WorldLocation) : 0.f;
}

TArray<AAerosVolcano*> UAerosWeatherSubsystem::GetVolcanoes() const
{
	return HazardManager ? HazardManager->GetVolcanoes() : TArray<AAerosVolcano*>();
}

bool UAerosWeatherSubsystem::FindNearestOcean(FVector From, FVector& OutCoast, FVector& OutOffshore, float OffshoreKm) const
{
	if (!Current || !Current->IsValid()) return false;
	float GX, GY;
	Current->WorldToGrid(From, GX, GY);
	const int32 NX = Current->NX, NY = Current->NY;

	// Only large water bodies count as ocean: lakes (terrain hollows below sea level are water cells
	// too) must not receive tsunamis or hurricanes. Size of each 4-connected water body, by flood fill.
	TArray<int32> Body;
	Body.Init(-1, NX * NY);
	TArray<int32> BodySize, Stack;
	int32 Largest = 0;
	for (int32 Seed = 0; Seed < NX * NY; ++Seed)
	{
		if (Body[Seed] >= 0 || !Current->Cells[Seed].bWater) continue;
		const int32 Id = BodySize.Add(0);
		Body[Seed] = Id;
		Stack.Reset();
		Stack.Add(Seed);
		while (Stack.Num() > 0)
		{
			const int32 K = Stack.Pop(EAllowShrinking::No);
			++BodySize[Id];
			const int32 I = K % NX, J = K / NX;
			const int32 Next[4] = { I > 0 ? K - 1 : -1, I < NX - 1 ? K + 1 : -1, J > 0 ? K - NX : -1, J < NY - 1 ? K + NX : -1 };
			for (const int32 N : Next)
			{
				if (N >= 0 && Body[N] < 0 && Current->Cells[N].bWater) { Body[N] = Id; Stack.Add(N); }
			}
		}
		Largest = FMath::Max(Largest, BodySize[Id]);
	}
	// 150 cells = 600 km2 at 2 km; a small domain whose only water is small keeps its largest body
	const int32 MinOceanCells = FMath::Min(150, Largest);

	int32 Best = -1;
	float BestD2 = TNumericLimits<float>::Max();
	for (int32 J = 0; J < NY; ++J)
	{
		for (int32 I = 0; I < NX; ++I)
		{
			const int32 K = I + J * NX;
			if (Body[K] < 0 || BodySize[Body[K]] < MinOceanCells) continue;
			const float D2 = FMath::Square(I + 0.5f - GX) + FMath::Square(J + 0.5f - GY);
			if (D2 < BestD2) { BestD2 = D2; Best = I + J * Current->NX; }
		}
	}
	if (Best < 0) return false;
	OutCoast = Current->GridToWorld(Best % Current->NX + 0.5f, Best / Current->NX + 0.5f);
	const FVector Dir = (OutCoast - From).GetSafeNormal2D();
	OutOffshore = OutCoast + (Dir.IsNearlyZero() ? FVector(1, 0, 0) : Dir) * OffshoreKm * 100000.0;
	return true;
}

void UAerosWeatherSubsystem::ResampleTerrain()
{
	TerrainCenter = FVector(1e30, 0, 0);
	TerrainCursor = -1;
}

void UAerosWeatherSubsystem::UpdateTerrainSampling()
{
	if (!ActiveSettings.bSampleTerrain || !Current || !Current->IsValid() || !SimThread) return;
	UWorld* World = GetWorld();
	if (!World) return;

	// (Re)start when the domain moved
	if (TerrainCursor < 0 && !Current->DomainCenter.Equals(TerrainCenter, 1.0))
	{
		TerrainCenter = Current->DomainCenter;
		TerrainNX = Current->NX;
		TerrainNY = Current->NY;
		TerrainCellCm = Current->CellM * 100.f;
		TerrainHeights.SetNumUninitialized(TerrainNX * TerrainNY);
		TerrainCursor = 0;
	}
	if (TerrainCursor < 0) return;

	// Time-sliced: a few thousand traces per frame, no hitch
	const int32 Total = TerrainNX * TerrainNY;
	const int32 End = FMath::Min(TerrainCursor + 3000, Total);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosTerrain), false);
	for (int32 K = TerrainCursor; K < End; ++K)
	{
		const float GX = K % TerrainNX + 0.5f, GY = K / TerrainNX + 0.5f;
		const double X = TerrainCenter.X + (GX - TerrainNX * 0.5f) * TerrainCellCm;
		const double Y = TerrainCenter.Y - (GY - TerrainNY * 0.5f) * TerrainCellCm;
		FHitResult Hit;
		float Height = 1.f; // nothing below: low flat land
		if (World->LineTraceSingleByChannel(Hit, FVector(X, Y, 1500000.0), FVector(X, Y, -1500000.0), ECC_WorldStatic, Params))
		{
			Height = float(Hit.ImpactPoint.Z - ActiveSettings.SeaLevelZ) * 0.01f;
		}
		TerrainHeights[K] = Height;
	}
	TerrainCursor = End;
	if (TerrainCursor >= Total)
	{
		TerrainCursor = -1;
		const TArray<float> Heights = TerrainHeights;
		const FVector Center = TerrainCenter;
		SimThread->Enqueue([Heights, Center](FAerosSimulation& Sim) { Sim.SetElevation(Heights, Center); });
	}
}

void UAerosWeatherSubsystem::RequestRealWeather(float Latitude, float Longitude, const FString& PlaceName, bool bApplyLocalTime, bool bKeepUpdating)
{
	if (LiveWeather && HasAuthority()) LiveWeather->Request(Latitude, Longitude, PlaceName, bApplyLocalTime, bKeepUpdating);
}

void UAerosWeatherSubsystem::StopRealWeather()
{
	if (LiveWeather) LiveWeather->Stop();
}

AAerosTornado* UAerosWeatherSubsystem::ForceTornado(int32 StormId)
{
	return StormManager ? StormManager->ForceTornado(StormId) : nullptr;
}

FAerosWeatherSample UAerosWeatherSubsystem::GetWeatherAtLocation(FVector WorldLocation) const
{
	if (!Current) return FAerosWeatherSample();
	FAerosWeatherSample S = Current->Sample(WorldLocation);
	if (Previous && Previous.Get() != Current.Get() && SnapshotAlpha < 1.f)
	{
		S = FAerosSnapshot::LerpSample(Previous->Sample(WorldLocation), S, SnapshotAlpha);
	}
	if (StormManager)
	{
		S.Wind += StormManager->GetVortexWindAt(WorldLocation);
		S.WindSpeed = float(S.Wind.Size());
		if (StormManager->IsInsideTornado(WorldLocation, 3.f)) S.Phenomenon = EAerosPhenomenon::Tornado;
	}
	if (HazardManager && HazardManager->GetShakingAt(WorldLocation) > 0.05f) S.Phenomenon = EAerosPhenomenon::Earthquake;
	return S;
}

FVector UAerosWeatherSubsystem::GetWindAtLocation(FVector WorldLocation) const
{
	return GetWeatherAtLocation(WorldLocation).Wind;
}

EAerosPhenomenon UAerosWeatherSubsystem::GetPhenomenonAtLocation(FVector WorldLocation) const
{
	return GetWeatherAtLocation(WorldLocation).Phenomenon;
}

TArray<FAerosStormData> UAerosWeatherSubsystem::GetStorms() const
{
	return Current ? Current->Storms : TArray<FAerosStormData>();
}

TArray<FAerosCycloneData> UAerosWeatherSubsystem::GetCyclones() const
{
	return Current ? Current->Cyclones : TArray<FAerosCycloneData>();
}

bool UAerosWeatherSubsystem::GetNearestStorm(FVector WorldLocation, FAerosStormData& OutStorm, float MaxDistanceKm) const
{
	if (!Current) return false;
	double Best = FMath::Square(MaxDistanceKm * 100000.0);
	bool bFound = false;
	for (const FAerosStormData& S : Current->Storms)
	{
		const double D = FVector::DistSquared2D(S.Location, WorldLocation);
		if (D < Best) { Best = D; OutStorm = S; bFound = true; }
	}
	return bFound;
}

TArray<AAerosTornado*> UAerosWeatherSubsystem::GetActiveTornadoes() const
{
	return StormManager ? StormManager->GetTornadoes() : TArray<AAerosTornado*>();
}

FAerosSynopticState UAerosWeatherSubsystem::GetSynopticState() const
{
	return Current ? Current->Synoptic : FAerosSynopticState();
}

FAerosSimStats UAerosWeatherSubsystem::GetStats() const
{
	return Current ? Current->Stats : FAerosSimStats();
}

UTexture2D* UAerosWeatherSubsystem::GetWeatherMapTexture() const
{
	return CloudManager ? CloudManager->GetWeatherMap() : nullptr;
}

UTexture2D* UAerosWeatherSubsystem::GetSurfaceMapTexture() const
{
	return CloudManager ? CloudManager->GetSurfaceMap() : nullptr;
}

UTexture2D* UAerosWeatherSubsystem::GetHazardMapTexture() const
{
	return CloudManager ? CloudManager->GetHazardMap() : nullptr;
}

FVector4 UAerosWeatherSubsystem::GetDomainRect() const
{
	if (!Current) return FVector4(0, 0, 1, 1);
	return FVector4(Current->DomainCenter.X, Current->DomainCenter.Y, Current->NX * Current->CellM * 100.0, Current->NY * Current->CellM * 100.0);
}

// =====================================================================
//  Console commands
// =====================================================================

namespace
{
	UAerosWeatherSubsystem* SubFromWorld(UWorld* World)
	{
		return World ? World->GetSubsystem<UAerosWeatherSubsystem>() : nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs CmdScenario(
		TEXT("aeros.Scenario"),
		TEXT("aeros.Scenario <0-10>: 0 Climate, 1 Fair, 2 SummerStorms, 3 Supercells, 4 Winter, 5 Tropical, 6 FrontalRain, 7 Custom, 8 Blizzard, 9 Desert, 10 Monsoon"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World); Sub && Args.Num() > 0)
				Sub->SetScenario(static_cast<EAerosScenario>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 10)));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTimeScale(
		TEXT("aeros.TimeScale"), TEXT("aeros.TimeScale <simulated seconds per real second>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World); Sub && Args.Num() > 0)
				Sub->SetTimeScale(FCString::Atof(*Args[0]));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTime(
		TEXT("aeros.Time"), TEXT("aeros.Time <dayOfYear> <hour>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World); Sub && Args.Num() > 1)
				Sub->SetDateTime(FCString::Atoi(*Args[0]), FCString::Atof(*Args[1]));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdBubble(
		TEXT("aeros.Bubble"), TEXT("aeros.Bubble [deltaT]: thermal bubble 15 km in front of the camera"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World))
			{
				FVector Loc = Sub->GetViewerLocation();
				if (APlayerController* PC = World->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
				{
					Loc += PC->PlayerCameraManager->GetCameraRotation().Vector().GetSafeNormal2D() * 1500000.0;
				}
				Sub->InjectThermalBubble(Loc, 6.f, Args.Num() > 0 ? FCString::Atof(*Args[0]) : 3.f, 2.f);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdHurricane(
		TEXT("aeros.Hurricane"), TEXT("aeros.Hurricane [centralPressure]: spawn a hurricane 150 km west of the viewer"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World))
				Sub->SpawnHurricane(Sub->GetViewerLocation() - FVector(15000000.0, 0, 0), Args.Num() > 0 ? FCString::Atof(*Args[0]) : 960.f, 35.f);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdQuake(
		TEXT("aeros.Quake"), TEXT("aeros.Quake [magnitude] [distanceKm]: earthquake near the viewer (server)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World))
			{
				const float Km = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 15.f;
				Sub->TriggerEarthquake(Sub->GetViewerLocation() + FVector(Km * 100000.0, 0, 0), Args.Num() > 0 ? FCString::Atof(*Args[0]) : 6.5f, 10.f);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTsunami(
		TEXT("aeros.Tsunami"), TEXT("aeros.Tsunami [magnitude]: submarine earthquake off the nearest coast"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UAerosWeatherSubsystem* Sub = SubFromWorld(World);
			FVector Coast, Offshore;
			if (Sub && Sub->FindNearestOcean(Sub->GetViewerLocation(), Coast, Offshore, 70.f))
				Sub->TriggerEarthquake(Offshore, Args.Num() > 0 ? FCString::Atof(*Args[0]) : 9.f, 20.f);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdFlood(
		TEXT("aeros.Flood"), TEXT("aeros.Flood [mm]: torrential rain over the viewer"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World))
				Sub->InjectRain(Sub->GetViewerLocation(), 15.f, Args.Num() > 0 ? FCString::Atof(*Args[0]) : 250.f);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdHaboob(
		TEXT("aeros.Haboob"), TEXT("aeros.Haboob: gust front 20 km away blowing toward the viewer"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World))
				Sub->InjectOutflow(Sub->GetViewerLocation() + FVector(-2000000.0, 0, 0), 20.f, -12.f);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdEruption(
		TEXT("aeros.Eruption"), TEXT("aeros.Eruption [VEI]: erupt the nearest volcano"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UAerosWeatherSubsystem* Sub = SubFromWorld(World);
			if (!Sub) return;
			AAerosVolcano* Best = nullptr;
			for (AAerosVolcano* V : Sub->GetVolcanoes())
				if (!Best || FVector::DistSquared(V->GetActorLocation(), Sub->GetViewerLocation()) < FVector::DistSquared(Best->GetActorLocation(), Sub->GetViewerLocation())) Best = V;
			if (Best) Best->StartEruption(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 4);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdLive(
		TEXT("aeros.Live"), TEXT("aeros.Live <latitude> <longitude> [name]: project the real current weather of a place (Open-Meteo)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World); Sub && Args.Num() > 1)
				Sub->RequestRealWeather(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), Args.Num() > 2 ? Args[2] : TEXT("Custom"), true, true);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTornado(
		TEXT("aeros.Tornado"), TEXT("aeros.Tornado [stormId]: force a tornado (server)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World))
				Sub->ForceTornado(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : -1);
		}));
}
