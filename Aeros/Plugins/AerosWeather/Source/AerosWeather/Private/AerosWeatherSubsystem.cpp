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
	StormManager->Initialize(this);
	CloudManager->Initialize(this);
	ActiveSettings = UAerosWeatherSettings::Get()->DefaultSimulation;
}

void UAerosWeatherSubsystem::Deinitialize()
{
	StopSimulation();
	if (StormManager) StormManager->Shutdown();
	if (CloudManager) CloudManager->Shutdown();
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
	KnownStorms.Reset();
	KnownCyclones.Reset();

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

	if (StormManager) StormManager->Tick(DeltaTime);
	if (CloudManager) CloudManager->Tick(DeltaTime);
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

	// Lightning (deterministic, identical on all machines)
	for (const FAerosLightningStrike& L : Current->Lightning)
	{
		if (L.Sequence > LastLightningSeq)
		{
			LastLightningSeq = L.Sequence;
			OnLightningStrike.Broadcast(L);
			if (StormManager) StormManager->HandleLightning(L);
		}
	}

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
		TEXT("aeros.Scenario <0-7>: 0 Climate, 1 Fair, 2 SummerStorms, 3 Supercells, 4 Winter, 5 Tropical, 6 FrontalRain"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World); Sub && Args.Num() > 0)
				Sub->SetScenario(static_cast<EAerosScenario>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 7)));
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

	FAutoConsoleCommandWithWorldAndArgs CmdTornado(
		TEXT("aeros.Tornado"), TEXT("aeros.Tornado [stormId]: force a tornado (server)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UAerosWeatherSubsystem* Sub = SubFromWorld(World))
				Sub->ForceTornado(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : -1);
		}));
}
