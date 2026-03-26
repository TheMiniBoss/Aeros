// Fill out your copyright notice in the Description page of Project Settings.

#include "WeatherSubsystem.h"
#include "WeatherRadar.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Misc/ScopeLock.h"

// ============================================================
//  INIT / DEINIT
// ============================================================

void UWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Optimized defaults
    ActiveConfig.GridNX = 64;
    ActiveConfig.GridNY = 64;
    ActiveConfig.GridNZ = 30;
    ActiveConfig.DX_km = 1.0f;
    ActiveConfig.DZ_m = 200.f;
    ActiveConfig.DT_s = 4.f;
    ActiveConfig.BigDT_s = 20.f;

    ActiveConfig.MicroScheme = EMicrophysicsScheme::Morrison;
    ActiveConfig.ConvScheme = EConvectionScheme::Explicit;
    ActiveConfig.bEnableCoriolis = true;
    ActiveConfig.bEnableTurbulence = true;
    ActiveConfig.bEnableRadiation = true;
    ActiveConfig.Latitude_deg = 45.f;

    DomainShiftThreshold_cm = ActiveConfig.DX_km * 100000.f * (ActiveConfig.GridNX / 4);

    CreateSimThread(ActiveConfig);

    Radar = NewObject<UWeatherRadar>(this);
    Radar->Initialise(this);

    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UWeatherSubsystem::TickWrapper), 0.f);
}

bool UWeatherSubsystem::TickWrapper(float DeltaTime)
{
    Tick(DeltaTime);
    return true;
}

void UWeatherSubsystem::Deinitialize()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    DestroySimThread();
    Super::Deinitialize();
}

void UWeatherSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);
}

// ============================================================
//  TICK
// ============================================================

void UWeatherSubsystem::Tick(float DeltaTime)
{
    AccumTime += DeltaTime * SimulationSpeed;

    // ── Non-blocking render view (ZERO lock, ZERO copy) ──
    if (SimThread)
    {
        CachedRenderView = SimThread->GetRenderView();

        if (Radar) Radar->OnNewSimulationData(CachedRenderView, ActiveConfig);
    }

    // ── Low-rate column update (pull from sim thread) ────
    if (SimThread)
    {
        int32 CNX, CNY;
        TArray<FAtmosphericColumn> NewCols;
        if (SimThread->TryGetColumns(NewCols, CNX, CNY))
        {
            FScopeLock Lock(&ColumnCacheLock);
            ColumnCache = MoveTemp(NewCols);
            ColumnCacheNX = CNX;
            ColumnCacheNY = CNY;
        }

        // Pull storms too
        TArray<FStormCell> NewStorms;
        if (SimThread->TryGetStorms(NewStorms))
        {
            FScopeLock Lock(&StormLock);
            // Check for new broadcasts by comparing with old list roughly
            for (const auto& N : NewStorms)
            {
                bool bFound = false;
                for (const auto& O : TrackedStorms)
                {
                    if (O.TrackID == N.TrackID) { bFound = true; break; }
                }
                if (!bFound) OnStormCellDetected.Broadcast(N, true);
            }
            TrackedStorms = MoveTemp(NewStorms);
        }
    }

    // ── Radar tick ───────────────────────────────────────
    if (Radar) Radar->Tick(DeltaTime);

    // ── Phenomenon broadcast (every 5s game time) ────────
    LastPhenomenonCheck += DeltaTime;
    if (LastPhenomenonCheck > 5.f)
    {
        LastPhenomenonCheck = 0.f;
        EWeatherPhenomenon Current = GetCurrentPhenomenon(LastPlayerPos);
        if (Current != LastBroadcastPhenomenon)
        {
            LastBroadcastPhenomenon = Current;
            OnPhenomenonChanged.Broadcast(Current);
        }
    }
}

// ============================================================
//  CONFIGURATION
// ============================================================

void UWeatherSubsystem::SetConfiguration(const FWeatherSimConfig& NewConfig)
{
    ActiveConfig = NewConfig;
    FSimCommand Cmd;
    Cmd.Type = FSimCommand::EType::SetConfig;
    Cmd.Config = NewConfig;
    if (SimThread) SimThread->SendCommand(Cmd);
}

void UWeatherSubsystem::SetMicrophysicsScheme(EMicrophysicsScheme Scheme)
{
    ActiveConfig.MicroScheme = Scheme;

    // Automatically adjust convection scheme based on resolution (WRF guide rule)
    if (ActiveConfig.DX_km >= 10.f)
        ActiveConfig.ConvScheme = EConvectionScheme::Parameterised;
    else if (ActiveConfig.DX_km <= 3.f)
        ActiveConfig.ConvScheme = EConvectionScheme::Explicit;
    else
        ActiveConfig.ConvScheme = EConvectionScheme::GrayZone;

    SetConfiguration(ActiveConfig);
}

void UWeatherSubsystem::SetConvectionScheme(EConvectionScheme Scheme)
{
    ActiveConfig.ConvScheme = Scheme;
    SetConfiguration(ActiveConfig);
}

// ============================================================
//  ATMOSPHERIC QUERIES
// ============================================================

bool UWeatherSubsystem::GetAtmosphereAtLocation(const FVector& WorldPos,
                                                FAtmosphericCell& OutCell) const
{
    const auto& View = CachedRenderView;
    if (!View.Grid) return false;

    FVector Local = WorldPos - DomainOrigin;
    float DX_cm = ActiveConfig.DX_km * 100000.f * FWeatherDoubleBuffer::RenderDownsample;
    float DZ_cm = ActiveConfig.DZ_m * 100.f * FWeatherDoubleBuffer::RenderDownsample;

    float fx = Local.X / DX_cm;
    float fy = Local.Y / DX_cm;
    float fz = Local.Z / DZ_cm;

    int32 x0 = FMath::FloorToInt(fx);
    int32 y0 = FMath::FloorToInt(fy);
    int32 z0 = FMath::FloorToInt(fz);
    float tx = fx - x0, ty = fy - y0, tz = fz - z0;

    auto Lerp8 = [&](auto Getter) {
        float c000 = Getter(View.At(x0, y0, z0));
        float c100 = Getter(View.At(x0 + 1, y0, z0));
        float c010 = Getter(View.At(x0, y0 + 1, z0));
        float c110 = Getter(View.At(x0 + 1, y0 + 1, z0));
        float c001 = Getter(View.At(x0, y0, z0 + 1));
        float c101 = Getter(View.At(x0 + 1, y0, z0 + 1));
        float c011 = Getter(View.At(x0, y0 + 1, z0 + 1));
        float c111 = Getter(View.At(x0 + 1, y0 + 1, z0 + 1));
        float c0 = FMath::Lerp(FMath::Lerp(c000, c100, tx), FMath::Lerp(c010, c110, tx), ty);
        float c1 = FMath::Lerp(FMath::Lerp(c001, c101, tx), FMath::Lerp(c011, c111, tx), ty);
        return FMath::Lerp(c0, c1, tz);
    };

    OutCell.U = Lerp8([](const FWeatherRenderCell& c) {return c.U; });
    OutCell.V = Lerp8([](const FWeatherRenderCell& c) {return c.V; });
    OutCell.W = Lerp8([](const FWeatherRenderCell& c) {return c.W; });
    OutCell.Temperature = Lerp8([](const FWeatherRenderCell& c) {return c.Temperature; });
    OutCell.RelativeHumidity = Lerp8([](const FWeatherRenderCell& c) {return c.RelativeHumidity; });
    OutCell.Ql = Lerp8([](const FWeatherRenderCell& c) {return c.CloudFraction; });
    OutCell.Reflectivity_dBZ = Lerp8([](const FWeatherRenderCell& c) {return c.Reflectivity_dBZ; });
    OutCell.TKE = Lerp8([](const FWeatherRenderCell& c) {return c.TKE; });
    OutCell.VortZ = Lerp8([](const FWeatherRenderCell& c) {return c.VortZ; });
    return true;
}

bool UWeatherSubsystem::GetColumnDiagnostics(const FVector& WorldPos,
                                             FAtmosphericColumn& OutColumn) const
{
    FScopeLock Lock(&ColumnCacheLock);
    if (ColumnCache.IsEmpty() || ColumnCacheNX <= 0 || ColumnCacheNY <= 0) return false;
    FVector Local = WorldPos - DomainOrigin;
    float DX_cm = ActiveConfig.DX_km * 100000.f;
    int32 Cx = FMath::Clamp(FMath::RoundToInt(Local.X / DX_cm), 0, ColumnCacheNX - 1);
    int32 Cy = FMath::Clamp(FMath::RoundToInt(Local.Y / DX_cm), 0, ColumnCacheNY - 1);
    OutColumn = ColumnCache[Cx + Cy * ColumnCacheNX];
    return true;
}

FVector UWeatherSubsystem::GetWindAtLocation(const FVector& WorldPos) const
{
    FAtmosphericCell Cell;
    if (GetAtmosphereAtLocation(WorldPos, Cell)) return FVector(Cell.U, Cell.V, Cell.W);
    return FVector(ActiveConfig.MeanWind.X, ActiveConfig.MeanWind.Y, 0.f);
}

float UWeatherSubsystem::GetTemperatureAtLocation(const FVector& WorldPos) const
{
    FAtmosphericCell Cell;
    if (GetAtmosphereAtLocation(WorldPos, Cell)) return Cell.Temperature;
    return WeatherConst::T0;
}

float UWeatherSubsystem::GetPressureAtLocation(const FVector& WorldPos) const
{
    return WeatherConst::P0; // Simplified for render performance 
}

float UWeatherSubsystem::GetRelativeHumidityAtLocation(const FVector& WorldPos) const
{
    FAtmosphericCell Cell;
    if (GetAtmosphereAtLocation(WorldPos, Cell)) return Cell.RelativeHumidity;
    return 0.5f;
}

float UWeatherSubsystem::GetPrecipitationRateAtLocation(const FVector& WorldPos) const
{
    FAtmosphericColumn Col;
    if (GetColumnDiagnostics(WorldPos, Col)) return Col.PrecipRate;
    return 0.f;
}

bool UWeatherSubsystem::IsHailPossibleAtLocation(const FVector& WorldPos) const
{
    FAtmosphericColumn Col;
    if (GetColumnDiagnostics(WorldPos, Col)) return Col.SHIP > 1.f && Col.CAPE > 1000.f;
    return false;
}

EWeatherPhenomenon UWeatherSubsystem::GetCurrentPhenomenon(const FVector& WorldPos) const
{
    FAtmosphericColumn Col;
    if (GetColumnDiagnostics(WorldPos, Col)) return Col.ClassifyPhenomenon();
    return EWeatherPhenomenon::Clear;
}

float UWeatherSubsystem::GetCloudCoverFraction(const FVector& WorldPos) const
{
    FAtmosphericCell Cell;
    if (GetAtmosphereAtLocation(WorldPos, Cell)) return Cell.Ql;
    return 0.f;
}

float UWeatherSubsystem::GetReflectivity_dBZ(const FVector& WorldPos) const
{
    FAtmosphericCell Cell;
    if (GetAtmosphereAtLocation(WorldPos, Cell)) return Cell.Reflectivity_dBZ;
    return -32.f;
}

float UWeatherSubsystem::GetTKE(const FVector& WorldPos) const
{
    FAtmosphericCell Cell;
    if (GetAtmosphereAtLocation(WorldPos, Cell)) return Cell.TKE;
    return 0.f;
}

// ============================================================
//  STORM QUERIES
// ============================================================

TArray<FStormCell> UWeatherSubsystem::GetAllDetectedStorms() const
{
    FScopeLock Lock(&StormLock);
    return TrackedStorms;
}

bool UWeatherSubsystem::GetNearestStorm(const FVector& WorldPos, FStormCell& OutStorm, float MaxRadius_cm) const
{
    FScopeLock Lock(&StormLock);
    float Best = MaxRadius_cm * MaxRadius_cm;
    bool bFound = false;
    for (const auto& Storm : TrackedStorms)
    {
        float D2 = FVector::DistSquared(Storm.Position, WorldPos);
        if (D2 < Best) { Best = D2; OutStorm = Storm; bFound = true; }
    }
    return bFound;
}

int32 UWeatherSubsystem::GetMesocycloneCount() const
{
    FScopeLock Lock(&StormLock);
    int32 Count = 0;
    for (const auto& S : TrackedStorms)
        if (S.bIsMesocyclone) ++Count;
    return Count;
}

// ============================================================
//  INTERVENTIONS
// ============================================================

void UWeatherSubsystem::InjectThermalBubble(const FVector& WorldPos, float Strength)
{
    if (!SimThread) return;
    FSimCommand Cmd; Cmd.Type = FSimCommand::EType::InjectPerturbation;
    Cmd.WorldPos = WorldPos; Cmd.Strength = Strength; SimThread->SendCommand(Cmd);
}
void UWeatherSubsystem::InjectMoistureAnomaly(const FVector& WorldPos, float DeltaQv)
{
    InjectThermalBubble(WorldPos, -DeltaQv);
}
void UWeatherSubsystem::InjectVorticity(const FVector& WorldPos, float Strength)
{
    if (!SimThread) return;
    FSimCommand CmdA; CmdA.Type = FSimCommand::EType::InjectPerturbation;
    CmdA.WorldPos = WorldPos + FVector(200.f * 100.f, 0.f, 0.f); CmdA.Strength = Strength * 0.5f;
    SimThread->SendCommand(CmdA);
    FSimCommand CmdB; CmdB.Type = FSimCommand::EType::InjectPerturbation;
    CmdB.WorldPos = WorldPos + FVector(-200.f * 100.f, 0.f, 0.f); CmdB.Strength = -Strength * 0.5f;
    SimThread->SendCommand(CmdB);
}
void UWeatherSubsystem::ResetSimulation()
{
    if (!SimThread) return;
    FSimCommand Cmd; Cmd.Type = FSimCommand::EType::Reset; SimThread->SendCommand(Cmd);
}

// ============================================================
//  RADAR
// ============================================================

void UWeatherSubsystem::SetRadarLocation(const FVector& WorldPos)
{
    if (Radar) Radar->SetLocation(WorldPos);
}

FRadarVolumeScan UWeatherSubsystem::GetLatestVolumeScan() const
{
    if (Radar) return Radar->GetLatestVolumeScan();
    return FRadarVolumeScan{};
}

// ============================================================
//  INFINITE WORLD — domain shifting
// ============================================================

void UWeatherSubsystem::UpdatePlayerLocation(const FVector& PlayerWorldPos)
{
    LastPlayerPos = PlayerWorldPos;
    UpdateDomainOrigin(PlayerWorldPos);
}

void UWeatherSubsystem::UpdateDomainOrigin(const FVector& PlayerPos)
{
    FVector Offset = PlayerPos - (DomainOrigin + FVector(ActiveConfig.GridNX * 0.5f * ActiveConfig.DX_km * 100000.f, ActiveConfig.GridNY * 0.5f * ActiveConfig.DX_km * 100000.f, 0.f));
    if (FMath::Abs(Offset.X) > DomainShiftThreshold_cm || FMath::Abs(Offset.Y) > DomainShiftThreshold_cm)
    {
        float DX_cm = ActiveConfig.DX_km * 100000.f;
        DomainOrigin.X += FMath::RoundToInt(Offset.X / DX_cm) * DX_cm;
        DomainOrigin.Y += FMath::RoundToInt(Offset.Y / DX_cm) * DX_cm;
        ActiveConfig.WorldOrigin = DomainOrigin;
        FSimCommand Cmd; Cmd.Type = FSimCommand::EType::SetConfig; Cmd.Config = ActiveConfig;
        if (SimThread) SimThread->SendCommand(Cmd);
    }
}

float UWeatherSubsystem::GetSimulationTime() const
{
    return SimThread ? SimThread->GetSimulationTime() : 0.f;
}

// ============================================================
//  PRIVATE HELPERS
// ============================================================

void UWeatherSubsystem::CreateSimThread(const FWeatherSimConfig& Cfg)
{
    DestroySimThread();
    SimThread = MakeUnique<FWeatherSimulationThread>(Cfg);
}

void UWeatherSubsystem::DestroySimThread()
{
    SimThread.Reset();
}