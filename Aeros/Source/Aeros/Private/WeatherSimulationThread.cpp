// Fill out your copyright notice in the Description page of Project Settings.


#include "WeatherSimulationThread.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeLock.h"

// ============================================================
//  CONSTRUCTION / DESTRUCTION
// ============================================================

FWeatherSimulationThread::FWeatherSimulationThread(const FWeatherSimConfig& InConfig)
    : Config(InConfig)
{
    const float DX_m = Config.DX_km * 1000.f;
    InvDX = 1.f / DX_m;
    InvDY = 1.f / DX_m;
    InvDZ = 1.f / Config.DZ_m;

    Thread = FRunnableThread::Create(this, TEXT("WeatherSimThread"), 0,
                                     TPri_BelowNormal);
}

FWeatherSimulationThread::~FWeatherSimulationThread()
{
    bShouldStop = true;
    if (Thread) { Thread->WaitForCompletion(); delete Thread; Thread = nullptr; }
}

// ============================================================
//  FRunnable INTERFACE
// ============================================================

bool FWeatherSimulationThread::Init()
{
    InitialiseGrid();
    ApplyRandomPerturbations();
    bIsRunning = true;
    return true;
}

uint32 FWeatherSimulationThread::Run()
{
    // Initialise double buffer
    DoubleBuffer.Initialise(Config.GridNX, Config.GridNY, Config.GridNZ);

    while (!bShouldStop)
    {
        // --- Process any incoming commands ---
        FSimCommand Cmd;
        while (CommandQueue.Dequeue(Cmd))
        {
            if (Cmd.Type == FSimCommand::EType::SetConfig)
            {
                Config = Cmd.Config;
                InitialiseGrid();
                DoubleBuffer.Initialise(Config.GridNX, Config.GridNY, Config.GridNZ);
            }
            else if (Cmd.Type == FSimCommand::EType::InjectPerturbation)
            {
                int32 Cx, Cy, Cz;
                if (WorldToCell(Cmd.WorldPos, Cx, Cy, Cz))
                {
                    // Warm bubble perturbation
                    for (int32 dz = -3; dz <= 3; ++dz)
                        for (int32 dy = -5; dy <= 5; ++dy)
                            for (int32 dx = -5; dx <= 5; ++dx)
                            {
                                float r2 = dx * dx + dy * dy + dz * dz * 4.f;
                                if (r2 < 30.f)
                                {
                                    auto& Cell = CurrentState.At(Cx + dx, Cy + dy, Cz + dz);
                                    Cell.Theta += Cmd.Strength * FMath::Exp(-r2 / 15.f);
                                    Cell.Qv += 0.002f * FMath::Exp(-r2 / 20.f);
                                }
                            }
                }
            }
            else if (Cmd.Type == FSimCommand::EType::Reset)
            {
                InitialiseGrid();
                ApplyRandomPerturbations();
                SimTime = 0.f;
            }
        }

        // --- Physics time-split: small acoustic DT, large physics DT ---
        const float AcousticDT = Config.DT_s;
        const int32 NumAcoustic = FMath::Max(1, FMath::RoundToInt(Config.BigDT_s / AcousticDT));

        for (int32 i = 0; i < NumAcoustic && !bShouldStop; ++i)
        {
            if (Config.bUseRK4)
                StepRK4(AcousticDT);
            else
            {
                TArray<FAtmosphericCell> Tend;
                Tend.SetNum(CurrentState.Cells.Num());
                ComputeTendencies(CurrentState, Tend, AcousticDT);
                for (int32 idx = 0; idx < CurrentState.Cells.Num(); ++idx)
                {
                    auto& C = CurrentState.Cells[idx];
                    auto& T = Tend[idx];
                    C.U += T.U * AcousticDT; C.V += T.V * AcousticDT; C.W += T.W * AcousticDT;
                    C.Theta += T.Theta * AcousticDT; C.Qv += T.Qv * AcousticDT;
                    C.Ql += T.Ql * AcousticDT; C.Qi += T.Qi * AcousticDT;
                }
            }
            SimTime = SimTime.load() + AcousticDT;
        }

        StepTurbulence(CurrentState, Config.BigDT_s);
        StepMicrophysics(CurrentState, Config.BigDT_s);
        StepSedimentation(CurrentState, Config.BigDT_s);
        StepRadiation(CurrentState, Config.BigDT_s, SimTime.load());
        SolvePressurePoisson(CurrentState);

        for (auto& Cell : CurrentState.Cells) Cell.UpdateDerivedVars();
        ComputeReflectivity(CurrentState);

        // --- Publish SLIM render grid (fast, no big copy) ---
        PublishRenderGrid();

        // --- Low-rate column update (every 2s) ---
        if ((SimTime.load() - LastColumnUpdate) > 2.f)
        {
            ComputeAllColumns(CurrentState);
            {
                FScopeLock Lock(&ColumnMutex);
                CachedColumns = CurrentState.Columns;
                bColumnsReady = true;
            }
            LastColumnUpdate = SimTime.load();
        }

        // --- Low-rate storm detection (every 5s) ---
        if ((SimTime.load() - LastStormUpdate) > 5.f)
        {
            TArray<FStormCell> Storms = DetectStormsInternal(CurrentState);
            {
                FScopeLock Lock(&StormMutex);
                CachedStorms = MoveTemp(Storms);
                bStormsReady = true;
            }
            LastStormUpdate = SimTime.load();
        }

        FPlatformProcess::Sleep(0.001f);
    }

    bIsRunning = false;
    return 0;
}

void FWeatherSimulationThread::Stop() { bShouldStop = true; }
void FWeatherSimulationThread::Exit() { bIsRunning = false; }

// ============================================================
//  PUBLIC API
// ============================================================

void FWeatherSimulationThread::SendCommand(const FSimCommand& Cmd)
{
    CommandQueue.Enqueue(Cmd);
}

// ============================================================
//  GRID INITIALISATION
// ============================================================

void FWeatherSimulationThread::InitialiseGrid()
{
    const int32 N = Config.GridNX * Config.GridNY * Config.GridNZ;
    CurrentState.NX = Config.GridNX;
    CurrentState.NY = Config.GridNY;
    CurrentState.NZ = Config.GridNZ;
    CurrentState.Cells.SetNum(N);
    CurrentState.Columns.SetNum(Config.GridNX * Config.GridNY);

    for (int32 z = 0; z < Config.GridNZ; ++z)
    {
        const float Z_m = GetAltitude(z);
        for (int32 y = 0; y < Config.GridNY; ++y)
            for (int32 x = 0; x < Config.GridNX; ++x)
            {
                auto& Cell = CurrentState.At(x, y, z);
                SetBaseStateProfile(Cell, Z_m);
            }
    }

    // Pre-allocate RK4 buffers
    K1.SetNum(N); K2.SetNum(N); K3.SetNum(N); K4.SetNum(N);
    TempStateA = CurrentState;
    TempStateB = CurrentState;
}

void FWeatherSimulationThread::SetBaseStateProfile(FAtmosphericCell& Cell,
                                                   float Z_m) const
{
    const float LapseRate = 6.5e-3f; // K/m
    float T = WeatherConst::T0 - LapseRate * Z_m;
    float P = WeatherConst::P0 * FMath::Pow(T / WeatherConst::T0,
                                            WeatherConst::g / (WeatherConst::Rd * LapseRate));
    float Rho = P / (WeatherConst::Rd * T);

    Cell.Temperature = T;
    Cell.Pres = P;
    Cell.Rho = Rho;
    Cell.Theta = T * FMath::Pow(WeatherConst::P0 / P, WeatherConst::Kappa);
    Cell.U = Config.MeanWind.X;
    Cell.V = Config.MeanWind.Y;
    Cell.W = 0.f;

    float BL_depth = 2000.f;
    float RH_BL = 0.80f, RH_FT = 0.30f;
    float RH = FMath::Lerp(RH_BL, RH_FT, FMath::Clamp(Z_m / BL_depth, 0.f, 1.f));

    float Es = ClaudiusClapeyron_Es(T);
    float Qs = 0.622f * Es / (P - Es);
    Cell.Qv = FMath::Max(0.f, RH * Qs);
    Cell.Ql = Cell.Qi = 0.f;
}

void FWeatherSimulationThread::ApplyRandomPerturbations()
{
    FRandomStream Rand(42);
    for (int32 z = 0; z < FMath::Min(5, Config.GridNZ); ++z)
        for (int32 y = 0; y < Config.GridNY; ++y)
            for (int32 x = 0; x < Config.GridNX; ++x)
            {
                CurrentState.At(x, y, z).Theta += Rand.FRandRange(-0.1f, 0.1f);
            }
}

// ============================================================
//  PUBLISH RENDER GRID (Lock-Free Optimization)
// ============================================================

void FWeatherSimulationThread::PublishRenderGrid()
{
    auto& WriteGrid = DoubleBuffer.GetWriteBuffer();
    const int32 RNX = DoubleBuffer.GetWriteRNX();
    const int32 RNY = DoubleBuffer.GetWriteRNY();
    const int32 RNZ = DoubleBuffer.GetWriteRNZ();
    const int32 DS = FWeatherDoubleBuffer::RenderDownsample;

    for (int32 rz = 0; rz < RNZ; ++rz)
        for (int32 ry = 0; ry < RNY; ++ry)
            for (int32 rx = 0; rx < RNX; ++rx)
            {
                // Average a DS×DS×DS block of sim cells into one render cell
                float SU = 0, SV = 0, SW = 0, ST = 0, SRH = 0, SCloud = 0, SRain = 0, SdBZ = 0, STKE = 0, SVort = 0, SHail = 0;
                int32 N = 0;

                for (int32 dz = 0; dz < DS; ++dz)
                    for (int32 dy = 0; dy < DS; ++dy)
                        for (int32 dx = 0; dx < DS; ++dx)
                        {
                            int32 simX = FMath::Clamp(rx * DS + dx, 0, CurrentState.NX - 1);
                            int32 simY = FMath::Clamp(ry * DS + dy, 0, CurrentState.NY - 1);
                            int32 simZ = FMath::Clamp(rz * DS + dz, 0, CurrentState.NZ - 1);

                            const auto& C = CurrentState.At(simX, simY, simZ);
                            SU += C.U; SV += C.V; SW += C.W;
                            ST += C.Temperature;
                            SRH += C.RelativeHumidity;
                            SCloud += C.Ql + C.Qi;
                            SRain += C.Rain.MixingRatio * C.Rain.FallSpeed * 3600.f * 1000.f; // mm/hr
                            SdBZ += C.Reflectivity_dBZ;
                            STKE += C.TKE;
                            SVort += C.VortZ;
                            SHail += C.Hail.MixingRatio;
                            ++N;
                        }

                auto& R = WriteGrid[rx + ry * RNX + rz * RNX * RNY];
                float Inv = 1.f / N;
                R.U = SU * Inv;
                R.V = SV * Inv;
                R.W = SW * Inv;
                R.Temperature = ST * Inv;
                R.RelativeHumidity = SRH * Inv;
                R.CloudFraction = FMath::Clamp(SCloud * Inv * 500.f, 0.f, 1.f);
                R.RainRate = SRain * Inv;
                R.Reflectivity_dBZ = SdBZ * Inv;
                R.TKE = STKE * Inv;
                R.VortZ = SVort * Inv;
                R.HailMR = SHail * Inv;
                R.PhenomenonByte = 0;
            }

    DoubleBuffer.CommitWrite(SimTime.load());
}

// ============================================================
//  STORM DETECTION (Internal)
// ============================================================

TArray<FStormCell> FWeatherSimulationThread::DetectStormsInternal(const FSimulationState& State)
{
    TArray<FStormCell> NewCells;

    const float dBZ_threshold = 40.f;  // dBZ — severe storm threshold
    const float W_threshold = 10.f;  // m/s updraft
    const float VortZ_threshold = 0.01f; // 1/s vertical vorticity

    const float DX_cm = Config.DX_km * 100000.f;

    for (int32 y = 1; y < State.NY - 1; ++y)
        for (int32 x = 1; x < State.NX - 1; ++x)
        {
            float MaxdBZ = -32.f, MaxW = 0.f, MaxVort = 0.f;
            float EchoTop_m = 0.f;
            int32 MaxW_z = 0;

            for (int32 z = 0; z < State.NZ; ++z)
            {
                const auto& C = State.At(x, y, z);
                if (C.Reflectivity_dBZ > MaxdBZ)
                {
                    MaxdBZ = C.Reflectivity_dBZ;
                    if (C.Reflectivity_dBZ > dBZ_threshold)
                        EchoTop_m = Config.DZ_m * z * (1.f + 0.05f * z);
                }
                if (C.W > MaxW) { MaxW = C.W; MaxW_z = z; }
                if (FMath::Abs(C.VortZ) > MaxVort) MaxVort = FMath::Abs(C.VortZ);
            }

            if (MaxdBZ < dBZ_threshold && MaxW < W_threshold) continue;

            FVector CellPos = Config.WorldOrigin + FVector((float)x * DX_cm, (float)y * DX_cm, 0.f);
            bool bDuplicate = false;
            for (const auto& Existing : NewCells)
            {
                if (FVector::Dist2D(Existing.Position, CellPos) < DX_cm * 5.f)
                {
                    bDuplicate = true; break;
                }
            }
            if (bDuplicate) continue;

            FStormCell Cell;
            Cell.Position = CellPos;
            Cell.MaxRefl_dBZ = MaxdBZ;
            Cell.EchoTop_km = EchoTop_m / 1000.f;
            Cell.MaxW_ms = MaxW;
            Cell.MaxVortZ = MaxVort;

            float RotVel = MaxVort * 2000.f;
            Cell.RotationalVelocity_ms = RotVel;
            Cell.MesocycloneDiameter_km = 2.f;
            Cell.bIsMesocyclone = (RotVel > 10.f && MaxW > 20.f);
            Cell.bTVS = (RotVel > 20.f && MaxW > 30.f);

            const auto& Col = State.Columns[x + y * State.NX];
            Cell.Type = Col.ClassifyPhenomenon();

            NewCells.Add(Cell);
        }

    TrackStormCellsInternal(NewCells);
    return InternalTrackedStorms;
}

void FWeatherSimulationThread::TrackStormCellsInternal(TArray<FStormCell>& NewCells)
{
    const float MaxMatchDist_cm = Config.DX_km * 100000.f * 10.f;
    TArray<bool> Matched;
    Matched.SetNumZeroed(InternalTrackedStorms.Num());
    if (Matched.IsEmpty() && NewCells.IsEmpty()) return;

    for (auto& New : NewCells)
    {
        float BestDist = MaxMatchDist_cm;
        int32 BestIdx = -1;

        for (int32 i = 0; i < Matched.Num(); ++i)
        {
            if (Matched[i]) continue;

            float D = FVector::Dist2D(New.Position, InternalTrackedStorms[i].Position);
            if (D < BestDist) { BestDist = D; BestIdx = i; }
        }

        if (BestIdx >= 0)
        {
            float Dt = 5.f; // 5s update rate
            InternalTrackedStorms[BestIdx].Motion_ms = (New.Position - InternalTrackedStorms[BestIdx].Position) / (Dt * 100.f);
            InternalTrackedStorms[BestIdx].Position = New.Position;
            InternalTrackedStorms[BestIdx].MaxRefl_dBZ = New.MaxRefl_dBZ;
            InternalTrackedStorms[BestIdx].EchoTop_km = New.EchoTop_km;
            InternalTrackedStorms[BestIdx].MaxW_ms = New.MaxW_ms;
            InternalTrackedStorms[BestIdx].MaxVortZ = New.MaxVortZ;
            InternalTrackedStorms[BestIdx].bIsMesocyclone = New.bIsMesocyclone;
            InternalTrackedStorms[BestIdx].bTVS = New.bTVS;
            InternalTrackedStorms[BestIdx].Age_s += Dt;
            Matched[BestIdx] = true;
        }
        else
        {
            New.TrackID = NextTrackID++;
            New.Age_s = 0.f;
            InternalTrackedStorms.Add(New);
        }
    }

    for (int32 i = Matched.Num() - 1; i >= 0; --i)
    {
        if (!Matched[i])
        {
            InternalTrackedStorms.RemoveAtSwap(i);
        }
    }
}

// ============================================================
//  RK4 TIME INTEGRATION
// ============================================================

void FWeatherSimulationThread::StepRK4(float DT)
{
    const int32 N = CurrentState.Cells.Num();

    // k1 = f(y_n)
    ComputeTendencies(CurrentState, K1, DT);

    // k2 = f(y_n + DT/2 * k1)
    for (int32 i = 0; i < N; ++i)
    {
        TempStateA.Cells[i] = CurrentState.Cells[i];
        TempStateA.Cells[i].U += 0.5f * DT * K1[i].U;
        TempStateA.Cells[i].V += 0.5f * DT * K1[i].V;
        TempStateA.Cells[i].W += 0.5f * DT * K1[i].W;
        TempStateA.Cells[i].Theta += 0.5f * DT * K1[i].Theta;
        TempStateA.Cells[i].Qv += 0.5f * DT * K1[i].Qv;
    }
    ComputeTendencies(TempStateA, K2, DT);

    // k3 = f(y_n + DT/2 * k2)
    for (int32 i = 0; i < N; ++i)
    {
        TempStateB.Cells[i] = CurrentState.Cells[i];
        TempStateB.Cells[i].U += 0.5f * DT * K2[i].U;
        TempStateB.Cells[i].V += 0.5f * DT * K2[i].V;
        TempStateB.Cells[i].W += 0.5f * DT * K2[i].W;
        TempStateB.Cells[i].Theta += 0.5f * DT * K2[i].Theta;
        TempStateB.Cells[i].Qv += 0.5f * DT * K2[i].Qv;
    }
    ComputeTendencies(TempStateB, K3, DT);

    // k4 = f(y_n + DT * k3)
    for (int32 i = 0; i < N; ++i)
    {
        TempStateA.Cells[i] = CurrentState.Cells[i];
        TempStateA.Cells[i].U += DT * K3[i].U;
        TempStateA.Cells[i].V += DT * K3[i].V;
        TempStateA.Cells[i].W += DT * K3[i].W;
        TempStateA.Cells[i].Theta += DT * K3[i].Theta;
        TempStateA.Cells[i].Qv += DT * K3[i].Qv;
    }
    ComputeTendencies(TempStateA, K4, DT);

    // y_{n+1} = y_n + DT/6 * (k1 + 2k2 + 2k3 + k4)
    for (int32 i = 0; i < N; ++i)
    {
        auto& C = CurrentState.Cells[i];
        C.U += DT / 6.f * (K1[i].U + 2.f * K2[i].U + 2.f * K3[i].U + K4[i].U);
        C.V += DT / 6.f * (K1[i].V + 2.f * K2[i].V + 2.f * K3[i].V + K4[i].V);
        C.W += DT / 6.f * (K1[i].W + 2.f * K2[i].W + 2.f * K3[i].W + K4[i].W);
        C.Theta += DT / 6.f * (K1[i].Theta + 2.f * K2[i].Theta + 2.f * K3[i].Theta + K4[i].Theta);
        C.Qv += DT / 6.f * (K1[i].Qv + 2.f * K2[i].Qv + 2.f * K3[i].Qv + K4[i].Qv);

        // Clip to physical bounds
        C.Qv = FMath::Max(C.Qv, 0.f);
        C.Ql = FMath::Max(C.Ql, 0.f);
        C.Qi = FMath::Max(C.Qi, 0.f);
    }
}

void FWeatherSimulationThread::ComputeTendencies(const FSimulationState& S,
                                                 TArray<FAtmosphericCell>& Tend,
                                                 float DT)
{
    for (auto& T : Tend) T = FAtmosphericCell{}; // zero-init

    Tend_Momentum(S, Tend, DT);
    Tend_Continuity(S, Tend);
    Tend_Thermodynamic(S, Tend);
    if (Config.bEnableCoriolis)
        Tend_Coriolis(S, Tend);
}

// ============================================================
//  MOMENTUM TENDENCY  (advection + pressure gradient + buoyancy)
// ============================================================

void FWeatherSimulationThread::Tend_Momentum(const FSimulationState& S,
                                             TArray<FAtmosphericCell>& T,
                                             float /*DT*/)
{
    const int32 NX = S.NX, NY = S.NY, NZ = S.NZ;

    for (int32 z = 1; z < NZ - 1; ++z)
        for (int32 y = 1; y < NY - 1; ++y)
            for (int32 x = 1; x < NX - 1; ++x)
            {
                const auto& C = S.At(x, y, z);
                auto& Tc = T[S.Idx(x, y, z)];

                float U = C.U, V = C.V, W = C.W;

                // 5th-order upwind advection of U
                auto GetU = [&](int32 xi, int32 yi, int32 zi) { return S.At(xi, yi, zi).U; };
                auto GetV = [&](int32 xi, int32 yi, int32 zi) { return S.At(xi, yi, zi).V; };
                auto GetW = [&](int32 xi, int32 yi, int32 zi) { return S.At(xi, yi, zi).W; };

                Tc.U -= Advect5thOrder(S, x, y, z, U, V, W, GetU, InvDX, InvDZ);
                Tc.V -= Advect5thOrder(S, x, y, z, U, V, W, GetV, InvDX, InvDZ);
                Tc.W -= Advect5thOrder(S, x, y, z, U, V, W, GetW, InvDX, InvDZ);

                // Buoyancy on W:  B = g*(theta'/theta_ref + 0.61*qv - ql - qi - qr)
                // Reference theta from base state
                float Z_m = GetAltitude(z);
                float T_ref = WeatherConst::T0 - 6.5e-3f * Z_m;
                float P_ref = WeatherConst::P0 * FMath::Pow(T_ref / WeatherConst::T0,
                                                            WeatherConst::g / (WeatherConst::Rd * 6.5e-3f));
                float Theta_ref = T_ref * FMath::Pow(WeatherConst::P0 / P_ref, WeatherConst::Kappa);

                float ThetaPrime = C.Theta - Theta_ref;
                float Buoyancy = WeatherConst::g * (ThetaPrime / Theta_ref
                                                    + 0.61f * C.Qv
                                                    - C.Ql - C.Qi
                                                    - C.Rain.MixingRatio);

                Tc.W += Buoyancy;

                // Pressure gradient (finite diff on Exner function)
                float dPdx = (S.At(x + 1, y, z).Pres - S.At(x - 1, y, z).Pres) * 0.5f * InvDX;
                float dPdy = (S.At(x, y + 1, z).Pres - S.At(x, y - 1, z).Pres) * 0.5f * InvDY;
                float dPdz = (S.At(x, y, z + 1).Pres - S.At(x, y, z - 1).Pres) * 0.5f * InvDZ;

                float RhoInv = 1.f / FMath::Max(C.Rho, 0.001f);
                Tc.U -= dPdx * RhoInv;
                Tc.V -= dPdy * RhoInv;
                Tc.W -= dPdz * RhoInv;

                // Vertical vorticity (diagnostic)
                float dVdx = (S.At(x + 1, y, z).V - S.At(x - 1, y, z).V) * 0.5f * InvDX;
                float dUdy = (S.At(x, y + 1, z).U - S.At(x, y - 1, z).U) * 0.5f * InvDY;
                // Not a tendency; store in current state directly for diagnostics
                const_cast<FAtmosphericCell&>(C).VortZ = dVdx - dUdy;

                float dUdx = (S.At(x + 1, y, z).U - S.At(x - 1, y, z).U) * 0.5f * InvDX;
                float dVdy = (S.At(x, y + 1, z).V - S.At(x, y - 1, z).V) * 0.5f * InvDY;
                const_cast<FAtmosphericCell&>(C).Divergence = dUdx + dVdy;
            }
}

// ============================================================
//  THERMODYNAMIC TENDENCY
// ============================================================

void FWeatherSimulationThread::Tend_Thermodynamic(const FSimulationState& S,
                                                  TArray<FAtmosphericCell>& T)
{
    for (int32 z = 1; z < S.NZ - 1; ++z)
        for (int32 y = 1; y < S.NY - 1; ++y)
            for (int32 x = 1; x < S.NX - 1; ++x)
            {
                const auto& C = S.At(x, y, z);
                auto& Tc = T[S.Idx(x, y, z)];

                float U = C.U, V = C.V, W = C.W;

                auto GetTheta = [&](int32 xi, int32 yi, int32 zi) { return S.At(xi, yi, zi).Theta; };
                auto GetQv = [&](int32 xi, int32 yi, int32 zi) { return S.At(xi, yi, zi).Qv;    };

                Tc.Theta -= Advect5thOrder(S, x, y, z, U, V, W, GetTheta, InvDX, InvDZ);
                Tc.Qv -= Advect5thOrder(S, x, y, z, U, V, W, GetQv, InvDX, InvDZ);
            }
}

// ============================================================
//  CORIOLIS
// ============================================================

void FWeatherSimulationThread::Tend_Coriolis(const FSimulationState& S,
                                             TArray<FAtmosphericCell>& T)
{
    const float f = Config.CoriolisF();
    for (int32 i = 0; i < S.Cells.Num(); ++i)
    {
        T[i].U += f * S.Cells[i].V;
        T[i].V -= f * S.Cells[i].U;
    }
}

// ============================================================
//  CONTINUITY TENDENCY (anelastic divergence damping)
// ============================================================

void FWeatherSimulationThread::Tend_Continuity(const FSimulationState& S,
                                               TArray<FAtmosphericCell>& T)
{
    // For anelastic: divergence damping to maintain ∇·(ρ₀v)=0
    const float C_div = 0.1f;
    for (int32 z = 1; z < S.NZ - 1; ++z)
        for (int32 y = 1; y < S.NY - 1; ++y)
            for (int32 x = 1; x < S.NX - 1; ++x)
            {
                const auto& C = S.At(x, y, z);
                float Div = C.Divergence;
                auto& Tc = T[S.Idx(x, y, z)];
                Tc.U -= C_div * (S.At(x + 1, y, z).Divergence - S.At(x - 1, y, z).Divergence) * 0.5f * InvDX;
                Tc.V -= C_div * (S.At(x, y + 1, z).Divergence - S.At(x, y - 1, z).Divergence) * 0.5f * InvDY;
                Tc.W -= C_div * (S.At(x, y, z + 1).Divergence - S.At(x, y, z - 1).Divergence) * 0.5f * InvDZ;
            }
}

// ============================================================
//  PRESSURE POISSON (simplified SOR iteration)
// ============================================================

void FWeatherSimulationThread::SolvePressurePoisson(FSimulationState& S)
{
    // Iterative SOR solver for Poisson equation ∇²p = RHS
    const int32 NX = S.NX, NY = S.NY, NZ = S.NZ;
    const float DX = Config.DX_km * 1000.f;
    const float DZ = Config.DZ_m;
    const float Omega_SOR = 1.7f;
    const int32 MaxIter = 10;

    for (int32 iter = 0; iter < MaxIter; ++iter)
        for (int32 z = 1; z < NZ - 1; ++z)
            for (int32 y = 1; y < NY - 1; ++y)
                for (int32 x = 1; x < NX - 1; ++x)
                {
                    auto& C = S.At(x, y, z);
                    // RHS = -ρ₀ * ∇·v / Δt
                    float RHS = -C.Rho * C.Divergence / Config.DT_s;

                    float P_new = (
                        (S.At(x + 1, y, z).Pres + S.At(x - 1, y, z).Pres) / (DX * DX) +
                        (S.At(x, y + 1, z).Pres + S.At(x, y - 1, z).Pres) / (DX * DX) +
                        (S.At(x, y, z + 1).Pres + S.At(x, y, z - 1).Pres) / (DZ * DZ) - RHS
                        ) / (2.f / (DX * DX) + 2.f / (DX * DX) + 2.f / (DZ * DZ));

                    C.Pres = C.Pres + Omega_SOR * (P_new - C.Pres);
                }
}

// ============================================================
//  TURBULENCE  — TKE 1.5-order closure (Deardorff/Smagorinsky)
// ============================================================

void FWeatherSimulationThread::StepTurbulence(FSimulationState& S, float DT)
{
    if (!Config.bEnableTurbulence) return;

    const float Cs = 0.18f; // Smagorinsky constant
    const float DX = Config.DX_km * 1000.f;
    const float DZ = Config.DZ_m;
    const float Ck = 0.1f;  // TKE production coefficient
    const float Ce = 0.93f; // TKE dissipation coefficient

    for (int32 z = 1; z < S.NZ - 1; ++z)
        for (int32 y = 1; y < S.NY - 1; ++y)
            for (int32 x = 1; x < S.NX - 1; ++x)
            {
                auto& C = S.At(x, y, z);
                const float L = FMath::Pow(DX * DX * DZ, 1.f / 3.f); // mixing length

                // Strain rate magnitude |S|
                float dUdz = (S.At(x, y, z + 1).U - S.At(x, y, z - 1).U) * 0.5f * InvDZ;
                float dVdz = (S.At(x, y, z + 1).V - S.At(x, y, z - 1).V) * 0.5f * InvDZ;
                float dUdx = (S.At(x + 1, y, z).U - S.At(x - 1, y, z).U) * 0.5f * InvDX;
                float dVdy = (S.At(x, y + 1, z).V - S.At(x, y - 1, z).V) * 0.5f * InvDY;
                float dWdz = (S.At(x, y, z + 1).W - S.At(x, y, z - 1).W) * 0.5f * InvDZ;

                float S2 = dUdz * dUdz + dVdz * dVdz + dUdx * dUdx + dVdy * dVdy + dWdz * dWdz;
                float Km = Cs * Cs * L * L * FMath::Sqrt(2.f * S2); // eddy viscosity

                // Brunt-Väisälä buoyancy term
                float dTdz = (S.At(x, y, z + 1).Theta - S.At(x, y, z - 1).Theta) * 0.5f * InvDZ;
                float N2 = FMath::Max(0.f, WeatherConst::g / C.Theta * dTdz);

                // TKE equation: dE/dt = Km*S² - g/Tv * Kh*dTheta/dz - Ce*E^(3/2)/L
                float Prod = Km * S2;
                float Buoy = -Km * N2; // buoyancy production/destruction
                float Diss = Ce * FMath::Pow(FMath::Max(C.TKE, 0.f), 1.5f) / FMath::Max(L, 1.f);

                C.TKE = FMath::Max(0.f, C.TKE + DT * (Prod + Buoy - Diss));
                C.Eps = Diss;

                // Apply sub-grid diffusion to momentum
                C.U += DT * Km * (
                    (S.At(x + 1, y, z).U - 2.f * C.U + S.At(x - 1, y, z).U) * InvDX * InvDX +
                    (S.At(x, y, z + 1).U - 2.f * C.U + S.At(x, y, z - 1).U) * InvDZ * InvDZ);
                C.V += DT * Km * (
                    (S.At(x, y + 1, z).V - 2.f * C.V + S.At(x, y - 1, z).V) * InvDY * InvDY +
                    (S.At(x, y, z + 1).V - 2.f * C.V + S.At(x, y, z - 1).V) * InvDZ * InvDZ);
                C.Theta += DT * Km * (
                    (S.At(x + 1, y, z).Theta - 2.f * C.Theta + S.At(x - 1, y, z).Theta) * InvDX * InvDX +
                    (S.At(x, y, z + 1).Theta - 2.f * C.Theta + S.At(x, y, z - 1).Theta) * InvDZ * InvDZ);
            }
}

// ============================================================
//  MICROPHYSICS  — dispatch to scheme
// ============================================================

void FWeatherSimulationThread::StepMicrophysics(FSimulationState& S, float DT)
{
    for (int32 z = 0; z < S.NZ; ++z)
    {
        const float Z_m = GetAltitude(z);
        for (int32 y = 0; y < S.NY; ++y)
            for (int32 x = 0; x < S.NX; ++x)
            {
                auto& C = S.At(x, y, z);
                switch (Config.MicroScheme)
                {
                    case EMicrophysicsScheme::Kessler:  Micro_Kessler(C, DT, Z_m); break;
                    case EMicrophysicsScheme::WSM6:     Micro_WSM6(C, DT, Z_m); break;
                    case EMicrophysicsScheme::Morrison: Micro_Morrison(C, DT, Z_m); break;
                    case EMicrophysicsScheme::Thompson: Micro_Thompson(C, DT, Z_m); break;
                }
            }
    }
}

// ── Kessler warm-rain (simplest) ──────────────────────────

void FWeatherSimulationThread::Micro_Kessler(FAtmosphericCell& C, float DT, float Z_m)
{
    ProcessCondensation(C, DT);
    ProcessAutoconversion(C, DT);
    ProcessAccretion(C, DT);
    ProcessEvaporation(C, DT, Z_m);
    UpdateTerminalFallSpeeds(C);
}

// ── WSM6 — 6-category bulk 1-moment ──────────────────────

void FWeatherSimulationThread::Micro_WSM6(FAtmosphericCell& C, float DT, float Z_m)
{
    Micro_Kessler(C, DT, Z_m);
    if (C.Temperature < 273.15f)
    {
        ProcessFreezingRiming(C, DT);
        ProcessMelting(C, DT);
        ProcessHailGrowth(C, DT);
        ProcessHailFragmentation(C, DT);
    }
    UpdateTerminalFallSpeeds(C);
}

// ── Morrison 2-moment (full ice) ──────────────────────────

void FWeatherSimulationThread::Micro_Morrison(FAtmosphericCell& C, float DT, float Z_m)
{
    Micro_WSM6(C, DT, Z_m);
    // Number concentration tendencies (2-moment extension)
    // Rain number concentration
    float Qr = C.Rain.MixingRatio;
    if (Qr > 1e-7f)
    {
        const float N0r = 8e6f; // m⁻⁴ Marshall-Palmer
        C.Rain.Lambda = FMath::Pow(N0r * PI * 1000.f / (C.Rho * Qr), 0.25f);
        C.Rain.NumberConc = N0r / C.Rain.Lambda;
        C.Rain.MeanDiameter = 1.f / FMath::Max(C.Rain.Lambda, 1e-3f);
    }
    // Ice/snow number concentrations (simplified Fletcher nucleation)
    if (C.Temperature < 273.15f)
    {
        float Tc = C.Temperature - 273.15f;
        float Ni = 1e3f * FMath::Exp(-0.6f * Tc); // m⁻³ (Cooper)
        C.Snow.NumberConc = Ni;
    }
}

// ── Thompson aerosol-aware 2-moment ──────────────────────

void FWeatherSimulationThread::Micro_Thompson(FAtmosphericCell& C, float DT, float Z_m)
{
    Micro_Morrison(C, DT, Z_m);
    // Thompson extension: modified gamma for rain (mu_r dependent on Nt)
    if (C.Rain.NumberConc > 1.f)
    {
        // mu–lambda relation (empirical)
        float mu_r = FMath::Clamp(0.0005f * FMath::Pow(C.Rain.NumberConc, 0.466f), 0.f, 8.f);
        C.Rain.Lambda = FMath::Pow(
            (mu_r + 3.f) * (mu_r + 2.f) * (mu_r + 1.f) *
            PI * 1000.f * C.Rain.NumberConc / (6.f * C.Rho * C.Rain.MixingRatio),
            0.25f);
    }
}

// ============================================================
//  MICROPHYSICS SUB-PROCESSES
// ============================================================

float FWeatherSimulationThread::ClaudiusClapeyron_Es(float T_K) const
{
    float Tc = T_K - 273.15f;
    return WeatherConst::EsConst_A *
        FMath::Exp(WeatherConst::EsConst_B * Tc / (Tc + WeatherConst::EsConst_C));
}

float FWeatherSimulationThread::SaturationMixingRatio(float T_K, float P_Pa) const
{
    float Es = ClaudiusClapeyron_Es(T_K);
    return 0.622f * Es / FMath::Max(P_Pa - Es, 1.f);
}

void FWeatherSimulationThread::ProcessCondensation(FAtmosphericCell& C, float DT) const
{
    // Saturation adjustment — relax toward Qs
    float Qs = SaturationMixingRatio(C.Temperature, C.Pres);
    float dQv = C.Qv - Qs;

    if (dQv > 0.f)
    {
        // Supersaturation → condense
        float dq = dQv / (1.f + WeatherConst::Lv * WeatherConst::Lv * Qs /
                          (WeatherConst::Rv * WeatherConst::Cp * C.Temperature * C.Temperature));
        dq = FMath::Min(dq, dQv); // can't condense more than available vapour
        dq = FMath::Min(dq * DT * 0.5f, dQv); // relaxation timescale

        C.Qv -= dq;
        C.Ql += dq;
        C.Theta += WeatherConst::Lv / (WeatherConst::Cp *
                                       FMath::Pow(WeatherConst::P0 / C.Pres, WeatherConst::Kappa)) * dq;
    }
    else if (dQv < 0.f && C.Ql > 0.f)
    {
        // Sub-saturation → evaporate cloud water
        float evap = FMath::Min(-dQv, C.Ql) * DT;
        C.Ql -= evap;
        C.Qv += evap;
        C.Theta -= WeatherConst::Lv / (WeatherConst::Cp *
                                       FMath::Pow(WeatherConst::P0 / C.Pres, WeatherConst::Kappa)) * evap;
    }
}

void FWeatherSimulationThread::ProcessAutoconversion(FAtmosphericCell& C, float DT) const
{
    // Berry (1968) autoconversion: cloud water → rain
    const float Ql_thresh = 5e-4f;
    if (C.Ql > Ql_thresh)
    {
        float dQr = 1.67e-5f * (C.Ql - Ql_thresh) * DT;
        dQr = FMath::Min(dQr, C.Ql);
        C.Ql -= dQr;
        C.Rain.MixingRatio += dQr;
    }
}

void FWeatherSimulationThread::ProcessAccretion(FAtmosphericCell& C, float DT) const
{
    // Collection of cloud water by rain (Kessler 1969)
    if (C.Rain.MixingRatio > 1e-7f && C.Ql > 1e-7f)
    {
        float dQr = 2.2f * C.Ql * FMath::Pow(C.Rain.MixingRatio * C.Rho, 0.875f) * DT;
        dQr = FMath::Min(dQr, C.Ql);
        C.Ql -= dQr;
        C.Rain.MixingRatio += dQr;
    }
}

void FWeatherSimulationThread::ProcessEvaporation(FAtmosphericCell& C, float DT, float Z_m) const
{
    // Evaporation of rain below cloud base (sub-saturated air)
    float Qs = SaturationMixingRatio(C.Temperature, C.Pres);
    float dS = C.Qv / Qs - 1.f; // saturation deficit (negative)

    if (dS < 0.f && C.Rain.MixingRatio > 1e-7f)
    {
        // Parameterised evaporation rate (simplified Ogura-Takahashi)
        float evap = -dS * 5.4e-3f * FMath::Pow(C.Rain.MixingRatio * C.Rho, 0.65f) * DT;
        evap = FMath::Min(evap, C.Rain.MixingRatio);

        C.Rain.MixingRatio -= evap;
        C.Qv += evap;
        // Evaporative cooling
        C.Theta -= WeatherConst::Lv / (WeatherConst::Cp *
                                       FMath::Pow(WeatherConst::P0 / C.Pres, WeatherConst::Kappa)) * evap;
    }
}

void FWeatherSimulationThread::ProcessFreezingRiming(FAtmosphericCell& C, float DT) const
{
    if (C.Temperature >= 273.15f) return;
    float Tc = C.Temperature - 273.15f; // negative

    // Riming: cloud water freezes on snow/graupel
    if (C.Ql > 1e-7f && C.Snow.MixingRatio > 1e-8f)
    {
        float rim = 0.3f * C.Snow.MixingRatio * C.Ql * DT;
        rim = FMath::Min(rim, C.Ql);
        C.Ql -= rim;
        C.Graupel.MixingRatio += rim;
        C.Theta += WeatherConst::Lf / (WeatherConst::Cp *
                                       FMath::Pow(WeatherConst::P0 / C.Pres, WeatherConst::Kappa)) * rim;
    }

    // Rain freezing into graupel (Bigg 1953)
    if (C.Rain.MixingRatio > 1e-7f && C.Temperature < 268.f)
    {
        float freeze = 20.f * FMath::Exp(-0.66f * Tc) * C.Rain.MixingRatio * DT;
        freeze = FMath::Min(freeze, C.Rain.MixingRatio);
        C.Rain.MixingRatio -= freeze;
        C.Graupel.MixingRatio += freeze;
        C.Theta += WeatherConst::Lf / (WeatherConst::Cp *
                                       FMath::Pow(WeatherConst::P0 / C.Pres, WeatherConst::Kappa)) * freeze;
    }
}

void FWeatherSimulationThread::ProcessMelting(FAtmosphericCell& C, float DT) const
{
    if (C.Temperature <= 273.15f) return;
    float excess = C.Temperature - 273.15f;

    auto Melt = [&](float& Solid, bool bHail)
    {
        if (Solid < 1e-8f) return;
        float melt_rate = bHail ? 0.005f : 0.01f;
        float dm = melt_rate * excess * Solid * DT;
        dm = FMath::Min(dm, Solid);
        Solid -= dm;
        C.Rain.MixingRatio += dm;
        C.Theta -= WeatherConst::Lf / (WeatherConst::Cp *
                                       FMath::Pow(WeatherConst::P0 / C.Pres, WeatherConst::Kappa)) * dm;
    };

    Melt(C.Snow.MixingRatio, false);
    Melt(C.Graupel.MixingRatio, false);
    Melt(C.Hail.MixingRatio, true);
}

void FWeatherSimulationThread::ProcessHailGrowth(FAtmosphericCell& C, float DT) const
{
    // Hail growth by wet riming: dm/dt = π r² E ρ_l Δu
    // Simplified: accretion rate proportional to liquid water content
    if (C.Hail.MixingRatio < 1e-8f && C.Graupel.MixingRatio < 1e-6f) return;
    if (C.Temperature > 273.f) return;

    // Convert large graupel to hail under intense updraft (W > 15 m/s)
    if (C.Graupel.MixingRatio > 1e-4f && C.W > 15.f)
    {
        float conv = 0.1f * C.Graupel.MixingRatio * DT;
        C.Graupel.MixingRatio -= conv;
        C.Hail.MixingRatio += conv;
    }

    // Wet growth: hail sweeps up cloud water and rain
    if (C.Hail.MixingRatio > 1e-8f)
    {
        float E_coll = 0.9f; // collection efficiency
        float A_h = PI * FMath::Pow(C.Hail.MeanDiameter * 0.5f, 2.f);
        float dV = C.Hail.FallSpeed; // relative speed

        float accrete = E_coll * A_h * dV * (C.Ql + C.Rain.MixingRatio) * DT;
        float available = C.Ql + C.Rain.MixingRatio;
        accrete = FMath::Min(accrete, available);

        float fLiq = C.Ql / FMath::Max(available, 1e-10f);
        C.Ql -= accrete * fLiq;
        C.Rain.MixingRatio -= accrete * (1.f - fLiq);
        C.Hail.MixingRatio += accrete;
    }
}

void FWeatherSimulationThread::ProcessHailFragmentation(FAtmosphericCell& C, float DT) const
{
    // Large hailstones (D > ~5cm) break up due to aerodynamic instability
    const float D_max = 0.05f; // m
    if (C.Hail.MeanDiameter > D_max && C.Hail.MixingRatio > 1e-8f)
    {
        float frag = 0.05f * C.Hail.MixingRatio * DT; // fragmentation rate
        C.Hail.MixingRatio -= frag;
        C.Graupel.MixingRatio += frag * 0.5f;
        C.Rain.MixingRatio += frag * 0.5f;
    }
}

void FWeatherSimulationThread::UpdateTerminalFallSpeeds(FAtmosphericCell& C) const
{
    // Terminal velocity: balance gravity vs aerodynamic drag
    // V_t = sqrt(2mg / (Cd * rho_air * A))
    auto FallSpeedLiquid = [&](float D_m) -> float
    {
        // Atlas & Ulbrich (1977) empirical
        return D_m < 1e-3f ? 3.78e3f * D_m :
            (9.65f - 10.3f * FMath::Exp(-600.f * D_m));
    };

    if (C.Rain.MixingRatio > 1e-7f)
    {
        const float N0r = 8e6f;
        float Qr = C.Rain.MixingRatio * C.Rho;
        C.Rain.Lambda = FMath::Pow(N0r * PI * 1000.f / Qr, 0.25f);
        C.Rain.MeanDiameter = 1.f / FMath::Max(C.Rain.Lambda, 1e-4f);
        C.Rain.FallSpeed = FallSpeedLiquid(C.Rain.MeanDiameter) *
            FMath::Pow(WeatherConst::RhoAir0 / C.Rho, 0.5f);
    }

    if (C.Snow.MixingRatio > 1e-8f)
        C.Snow.FallSpeed = FMath::Min(2.5f, 0.2f * FMath::Pow(C.Snow.MixingRatio * 1000.f, 0.3f));

    if (C.Graupel.MixingRatio > 1e-8f)
        C.Graupel.FallSpeed = FMath::Min(7.f, 1.0f * FMath::Pow(C.Graupel.MixingRatio * 1000.f, 0.3f));

    if (C.Hail.MixingRatio > 1e-8f)
    {
        // Large hail: Vt = 9.32 * D^0.55 (Matson & Huggins)
        float D = C.Hail.MeanDiameter > 0.f ? C.Hail.MeanDiameter : 0.01f;
        C.Hail.FallSpeed = FMath::Clamp(9.32f * FMath::Pow(D, 0.55f), 5.f, 50.f);
    }
}

// ============================================================
//  SEDIMENTATION  — precipitation falls through grid
// ============================================================

void FWeatherSimulationThread::StepSedimentation(FSimulationState& S, float DT)
{
    const float DZ = Config.DZ_m;

    for (int32 z = S.NZ - 1; z >= 1; --z)
        for (int32 y = 0; y < S.NY; ++y)
            for (int32 x = 0; x < S.NX; ++x)
            {
                auto& C = S.At(x, y, z);
                auto& Below = S.At(x, y, z - 1);

                auto Sediment = [&](FHydrometeorState& H, FHydrometeorState& HBelow)
                {
                    if (H.MixingRatio < 1e-9f) return;
                    float flux = H.FallSpeed * H.MixingRatio * C.Rho / DZ * DT;
                    flux = FMath::Min(flux, H.MixingRatio);
                    H.MixingRatio -= flux;
                    HBelow.MixingRatio += flux;
                };

                Sediment(C.Rain, Below.Rain);
                Sediment(C.Snow, Below.Snow);
                Sediment(C.Graupel, Below.Graupel);
                Sediment(C.Hail, Below.Hail);
            }
}

// ============================================================
//  RADIATION  — simplified broadband SW/LW
// ============================================================

void FWeatherSimulationThread::StepRadiation(FSimulationState& S, float DT, float SimTime_s)
{
    if (!Config.bEnableRadiation) return;

    const float S0 = 1361.f;  // W/m² solar constant
    const float AlbEarth = 0.30f;
    // Solar zenith angle (simplified diurnal cycle)
    float LocalTime = FMath::Fmod(SimTime_s / 3600.f, 24.f);
    float ZenithAngle = FMath::DegreesToRadians(90.f - 90.f *
                                                FMath::Cos(PI * (LocalTime - 6.f) / 12.f));
    float CosSZA = FMath::Max(0.f, FMath::Cos(ZenithAngle));
    float SW_TOA = S0 * CosSZA * (1.f - AlbEarth);

    // Simple k-distribution: optical depth proportional to cloud content
    for (int32 x = 0; x < S.NX; ++x)
        for (int32 y = 0; y < S.NY; ++y)
        {
            float SW = SW_TOA;
            // Top-down sweep
            for (int32 z = S.NZ - 1; z >= 0; --z)
            {
                auto& C = S.At(x, y, z);
                float WaterPath = (C.Ql + C.Qi) * C.Rho * Config.DZ_m;
                float tau = 0.15f * WaterPath * 1000.f; // optical depth
                float Trans = FMath::Exp(-tau);
                float Heat = SW * (1.f - Trans) / (C.Rho * WeatherConst::Cp * Config.DZ_m);
                C.Theta += Heat * DT / FMath::Pow(WeatherConst::P0 / C.Pres, WeatherConst::Kappa);
                SW *= Trans;
            }

            // LW cooling: empirical ~2 K/day in clear sky
            for (int32 z = 0; z < S.NZ; ++z)
            {
                auto& C = S.At(x, y, z);
                float LW = 2.f / 86400.f; // K/s clear sky cooling
                C.Theta -= LW * DT * FMath::Pow(WeatherConst::P0 / C.Pres, WeatherConst::Kappa);
            }
        }
}

// ============================================================
//  COLUMN DIAGNOSTICS
// ============================================================

void FWeatherSimulationThread::ComputeAllColumns(FSimulationState& S)
{
    for (int32 y = 0; y < S.NY; ++y)
        for (int32 x = 0; x < S.NX; ++x)
        {
            auto& Col = S.Columns[x + y * S.NX];
            ComputeColumnCAPE_CIN(S, x, y, Col);
            ComputeColumnSRH(S, x, y, Col);
            ComputeShearParams(S, x, y, Col);

            // Composite parameters
            float CAPE = FMath::Max(Col.CAPE, 1.f);
            float Shear6 = FMath::Max(Col.BulkShear_0_6km, 1.f);
            float SRH3 = FMath::Max(Col.SRH_0_3km, 1.f);

            // SCP = (CAPE/1000) * (SRH_03/100) * (shear6/20)
            Col.SCP = (CAPE / 1000.f) * (SRH3 / 100.f) * (Shear6 / 20.f);

            // STP (Thompson et al. 2003)
            float LCL_term = (Col.LCL_m < 1000.f) ? 1.f : (2000.f - Col.LCL_m) / 1000.f;
            LCL_term = FMath::Clamp(LCL_term, 0.f, 1.f);
            Col.STP = (CAPE / 1500.f) * LCL_term * (SRH3 / 150.f) * (Shear6 / 20.f);

            // SHIP (≈significant hail parameter)
            float T_500 = S.At(x, y, FMath::Min(25, S.NZ - 1)).Temperature;
            float ThetaE_diff = 10.f; // simplified
            Col.SHIP = (CAPE / 2000.f) * (T_500 / 253.f) * (ThetaE_diff / 30.f) * (Shear6 / 25.f);

            // Precipitation rate from lowest-level rain content
            auto& SfcCell = S.At(x, y, 0);
            Col.PrecipRate = SfcCell.Rain.MixingRatio * SfcCell.Rain.FallSpeed * 3600.f * 1000.f;
        }
}

void FWeatherSimulationThread::ComputeColumnCAPE_CIN(const FSimulationState& S,
                                                     int32 Cx, int32 Cy,
                                                     FAtmosphericColumn& Col) const
{
    // Parcel method: lift surface parcel, compute buoyancy integral
    const auto& SfcCell = S.At(Cx, Cy, 0);
    float T_parcel = SfcCell.Temperature;
    float Qv_parcel = SfcCell.Qv;
    float Theta_parcel = SfcCell.Theta;

    Col.CAPE = 0.f; Col.CIN = 0.f;
    Col.LCL_m = 0.f; Col.LFC_m = 0.f; Col.EL_m = 0.f;

    bool bAboveLCL = false, bAboveLFC = false;

    for (int32 z = 1; z < S.NZ; ++z)
    {
        const float Z_m = GetAltitude(z);
        const auto& Env = S.At(Cx, Cy, z);

        // Dry adiabatic lapse rate below LCL
        float DZ = Config.DZ_m;
        if (!bAboveLCL)
        {
            T_parcel -= WeatherConst::g / WeatherConst::Cp * DZ;
            float Qs = SaturationMixingRatio(T_parcel, Env.Pres);
            if (Qv_parcel >= Qs)
            {
                bAboveLCL = true;
                Col.LCL_m = Z_m;
            }
        }
        else
        {
            // Moist adiabatic lapse rate
            float Es = ClaudiusClapeyron_Es(T_parcel);
            float Qs = 0.622f * Es / (Env.Pres - Es);
            float Gamma_m = WeatherConst::g * (1.f + WeatherConst::Lv * Qs /
                                               (WeatherConst::Rd * T_parcel)) /
                (WeatherConst::Cp + WeatherConst::Lv * WeatherConst::Lv * Qs /
                 (WeatherConst::Rv * T_parcel * T_parcel));
            T_parcel -= Gamma_m * DZ;
        }

        float Tv_parcel = T_parcel * (1.f + 0.61f * Qv_parcel);
        float Tv_env = Env.VirtualTemp > 0.f ? Env.VirtualTemp : Env.Temperature;
        float Buoy = WeatherConst::g * (Tv_parcel - Tv_env) / Tv_env;

        if (bAboveLCL)
        {
            if (Buoy > 0.f)
            {
                if (!bAboveLFC) { bAboveLFC = true; Col.LFC_m = Z_m; }
                Col.CAPE += Buoy * DZ;
                Col.EL_m = Z_m;
            }
            else if (!bAboveLFC)
            {
                Col.CIN += Buoy * DZ;
            }
        }
    }
}

void FWeatherSimulationThread::ComputeColumnSRH(const FSimulationState& S,
                                                int32 Cx, int32 Cy,
                                                FAtmosphericColumn& Col) const
{
    FVector StormC = BunkerStormMotion(S, Cx, Cy);
    Col.StormMotion = StormC;

    Col.SRH_0_3km = 0.f; Col.SRH_0_1km = 0.f;
    const float h_3km = 3000.f, h_1km = 1000.f;

    for (int32 z = 1; z < S.NZ - 1; ++z)
    {
        float Z_m = GetAltitude(z);
        if (Z_m > h_3km) break;

        const auto& Below = S.At(Cx, Cy, z - 1);
        const auto& Above = S.At(Cx, Cy, z + 1);

        // SRH = -(V - C) · (∇ × V) dz  (Storm-Relative Helicity)
        FVector V_sr = FVector(S.At(Cx, Cy, z).U, S.At(Cx, Cy, z).V, 0.f) - StormC;
        float dVdz = (Above.V - Below.V) * 0.5f * InvDZ;
        float dUdz = (Above.U - Below.U) * 0.5f * InvDZ;
        // Horizontal vorticity components
        float omegaX = dVdz;  // simplified (ignores d/dx terms)
        float omegaY = -dUdz;

        float SRH_layer = -(V_sr.X * omegaX + V_sr.Y * omegaY) * Config.DZ_m;
        Col.SRH_0_3km += SRH_layer;
        if (Z_m <= h_1km) Col.SRH_0_1km += SRH_layer;
    }
}

void FWeatherSimulationThread::ComputeShearParams(const FSimulationState& S,
                                                  int32 Cx, int32 Cy,
                                                  FAtmosphericColumn& Col) const
{
    const auto& Sfc = S.At(Cx, Cy, 0);
    FVector V_sfc(Sfc.U, Sfc.V, 0.f);

    auto FindWindAtHeight = [&](float TargetZ) -> FVector
    {
        for (int32 z = 0; z < S.NZ; ++z)
        {
            if (GetAltitude(z) >= TargetZ)
            {
                const auto& C = S.At(Cx, Cy, z);
                return FVector(C.U, C.V, 0.f);
            }
        }
        return V_sfc;
    };

    FVector V_1km = FindWindAtHeight(1000.f);
    FVector V_6km = FindWindAtHeight(6000.f);

    Col.BulkShear_0_6km = (V_6km - V_sfc).Size();
    Col.BulkShear_0_1km = (V_1km - V_sfc).Size();
}

FVector FWeatherSimulationThread::BunkerStormMotion(const FSimulationState& S,
                                                    int32 Cx, int32 Cy) const
{
    // Bunker et al. (2000) method: deviation from mean wind
    FVector MeanWind(0.f);
    int32 Count = 0;
    for (int32 z = 0; z < S.NZ && GetAltitude(z) < 6000.f; ++z)
    {
        const auto& C = S.At(Cx, Cy, z);
        MeanWind += FVector(C.U, C.V, 0.f);
        ++Count;
    }
    if (Count > 0) MeanWind /= (float)Count;

    // Deviate 7.5 m/s to the right (Northern Hemisphere supercell)
    FVector Shear(S.At(Cx, Cy, 6).U - S.At(Cx, Cy, 0).U,
                  S.At(Cx, Cy, 6).V - S.At(Cx, Cy, 0).V, 0.f);
    float ShearMag = FMath::Max(Shear.Size(), 1.f);
    FVector ShearN(-Shear.Y / ShearMag, Shear.X / ShearMag, 0.f); // normal to shear

    return MeanWind + ShearN * 7.5f; // right-mover
}

// ============================================================
//  REFLECTIVITY  — Z-R relationships
// ============================================================

float FWeatherSimulationThread::Z_from_RainMR(float Qr, float Rho) const
{
    // Z = 200 R^1.6  (Marshall-Palmer) → rearranged
    float Rainrate = Qr * Rho * 3600.f; // mm/hr approx
    return Rainrate > 1e-3f ? 200.f * FMath::Pow(Rainrate, 1.6f) : 0.f; // mm^6/m^3
}

float FWeatherSimulationThread::Z_from_SnowMR(float Qs, float Rho) const
{
    float IWC = Qs * Rho * 1000.f; // g/m³
    return IWC > 1e-3f ? 670.f * FMath::Pow(IWC, 1.47f) : 0.f;
}

float FWeatherSimulationThread::Z_from_HailMR(float Qg, float Rho) const
{
    float HWC = Qg * Rho * 1000.f;
    return HWC > 1e-3f ? 4000.f * FMath::Pow(HWC, 1.5f) : 0.f;
}

void FWeatherSimulationThread::ComputeReflectivity(FSimulationState& S)
{
    for (int32 z = 0; z < S.NZ; ++z)
        for (int32 y = 0; y < S.NY; ++y)
            for (int32 x = 0; x < S.NX; ++x)
            {
                auto& C = S.At(x, y, z);
                float Z_total = Z_from_RainMR(C.Rain.MixingRatio, C.Rho)
                    + Z_from_SnowMR(C.Snow.MixingRatio, C.Rho)
                    + Z_from_HailMR(C.Graupel.MixingRatio + C.Hail.MixingRatio, C.Rho);

                C.Reflectivity_dBZ = Z_total > 0.f ?
                    10.f * FMath::LogX(10.f, Z_total) : -32.f;
            }
}

// ============================================================
//  5TH-ORDER ADVECTION (WENO-like upwind)
// ============================================================

float FWeatherSimulationThread::Advect5thOrder(
    const FSimulationState& S, int32 x, int32 y, int32 z,
    float U, float V, float W,
    const TFunctionRef<float(int32, int32, int32)>& GetVal,
    float InvDXArg, float InvDZArg) const
{
    // Simplified 3rd-order upwind for stability
    auto Upwind3 = [](float vm2, float vm1, float v0, float vp1, float vp2, float vel, float invH)
    {
        if (vel > 0.f)
            return vel * invH * (2.f * v0 + 3.f * vp1 - 6.f * vm1 + vm2) / 6.f;
        else
            return vel * invH * (-vp2 + 6.f * vp1 - 3.f * v0 - 2.f * vm1) / 6.f;
    };

    float adv_x = Upwind3(
        GetVal(x - 2, y, z), GetVal(x - 1, y, z), GetVal(x, y, z),
        GetVal(x + 1, y, z), GetVal(x + 2, y, z), U, InvDXArg);

    float adv_y = Upwind3(
        GetVal(x, y - 2, z), GetVal(x, y - 1, z), GetVal(x, y, z),
        GetVal(x, y + 1, z), GetVal(x, y + 2, z), V, InvDXArg);

    float adv_z = Upwind3(
        GetVal(x, y, z - 2), GetVal(x, y, z - 1), GetVal(x, y, z),
        GetVal(x, y, z + 1), GetVal(x, y, z + 2), W, InvDZArg);

    return adv_x + adv_y + adv_z;
}

// ============================================================
//  HELPERS
// ============================================================

FVector FWeatherSimulationThread::CellToWorld(int32 x, int32 y, int32 z) const
{
    float DX_cm = Config.DX_km * 100000.f;
    return Config.WorldOrigin + FVector(x * DX_cm, y * DX_cm, GetAltitude(z) * 100.f);
}

bool FWeatherSimulationThread::WorldToCell(const FVector& Pos,
                                           int32& Cx, int32& Cy, int32& Cz) const
{
    FVector Local = Pos - Config.WorldOrigin;
    float DX_cm = Config.DX_km * 100000.f;
    float DZ_cm = Config.DZ_m * 100.f;
    Cx = FMath::RoundToInt(Local.X / DX_cm);
    Cy = FMath::RoundToInt(Local.Y / DX_cm);
    Cz = FMath::RoundToInt(Local.Z / DZ_cm);
    Cx = (Cx % Config.GridNX + Config.GridNX) % Config.GridNX;
    Cy = (Cy % Config.GridNY + Config.GridNY) % Config.GridNY;
    Cz = FMath::Clamp(Cz, 0, Config.GridNZ - 1);
    return true;
}

float FWeatherSimulationThread::GetAltitude(int32 z) const
{
    return Config.DZ_m * z * (1.f + 0.05f * z);
}