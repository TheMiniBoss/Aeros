// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "Containers/Queue.h"
#include "WeatherBuffer.h"
#include "WeatherTypes.h"

// ============================================================
//  WEATHER SIMULATION THREAD
//  Runs Navier-Stokes + microphysics on a background thread.
//  Communicates with UWeatherSubsystem via thread-safe queues.
// ============================================================

struct FSimulationState
{
    TArray<FAtmosphericCell> Cells;   // Flat [x + y*NX + z*NX*NY]
    TArray<FAtmosphericColumn> Columns; // One per (x,y) column
    float SimTime = 0.f;
    int32 NX, NY, NZ;

    FORCEINLINE int32 Idx(int32 x, int32 y, int32 z) const
    {
        // Periodic wrap in x/y for infinite world
        x = (x % NX + NX) % NX;
        y = (y % NY + NY) % NY;
        z = FMath::Clamp(z, 0, NZ - 1);
        return x + y * NX + z * NX * NY;
    }

    FAtmosphericCell& At(int32 x, int32 y, int32 z) { return Cells[Idx(x, y, z)]; }
    const FAtmosphericCell& At(int32 x, int32 y, int32 z) const { return Cells[Idx(x, y, z)]; }
};

// Command from game thread to simulation thread
struct FSimCommand
{
    enum class EType { SetConfig, InjectPerturbation, Reset } Type;
    FWeatherSimConfig Config;
    FVector WorldPos;
    float   Strength;
};

class AEROS_API FWeatherSimulationThread : public FRunnable
{
public:
    explicit FWeatherSimulationThread(const FWeatherSimConfig& InConfig);
    virtual ~FWeatherSimulationThread();

    //── FRunnable interface ────────────────────────────────
    virtual bool   Init()    override;
    virtual uint32 Run()     override;
    virtual void   Stop()    override;
    virtual void   Exit()    override;

    //── Public thread-safe API ─────────────────────────────
    void SendCommand(const FSimCommand& Cmd);

    float GetSimulationTime() const { return SimTime.load(); }
    bool  IsRunning()         const { return bIsRunning; }

    // Lock-free render view — called every game frame, ZERO blocking
    FWeatherDoubleBuffer::FReadView GetRenderView() const
    {
        return DoubleBuffer.GetReadView();
    }

    // Column diagnostics — updated at low rate (2s), separate lock
    bool TryGetColumns(TArray<FAtmosphericColumn>& Out, int32& OutNX, int32& OutNY) const
    {
        if (!bColumnsReady.load()) return false;
        FScopeLock Lock(&ColumnMutex);
        Out = CachedColumns;
        OutNX = Config.GridNX;
        OutNY = Config.GridNY;
        return true;
    }

    // Storm list — updated at low rate
    bool TryGetStorms(TArray<FStormCell>& Out) const
    {
        if (!bStormsReady.load()) return false;
        FScopeLock Lock(&StormMutex);
        Out = CachedStorms;
        return true;
    }

private:
    //── Initialisation ────────────────────────────────────
    void InitialiseGrid();
    void SetBaseStateProfile(FAtmosphericCell& Cell, float Z_m) const;
    void ApplyRandomPerturbations();

    //── RK4 time integration ──────────────────────────────
    void StepRK4(float DT);
    void ComputeTendencies(const FSimulationState& State,
                           TArray<FAtmosphericCell>& Tend,
                           float DT);

    //── Dynamic core tendencies ───────────────────────────
    void Tend_Momentum(const FSimulationState& S, TArray<FAtmosphericCell>& T, float DT);
    void Tend_Continuity(const FSimulationState& S, TArray<FAtmosphericCell>& T);
    void Tend_Thermodynamic(const FSimulationState& S, TArray<FAtmosphericCell>& T);
    void Tend_Coriolis(const FSimulationState& S, TArray<FAtmosphericCell>& T);

    //── Pressure solver (anelastic Poisson) ───────────────
    void SolvePressurePoisson(FSimulationState& S);
    void ApplyPressureGradient(FSimulationState& S);

    //── Turbulence (TKE 1.5-order closure) ────────────────
    void StepTurbulence(FSimulationState& S, float DT);
    float ComputeSmagorinskyKm(const FSimulationState& S, int32 x, int32 y, int32 z) const;

    //── Microphysics ──────────────────────────────────────
    void StepMicrophysics(FSimulationState& S, float DT);
    void Micro_Kessler(FAtmosphericCell& Cell, float DT, float Z_m);
    void Micro_WSM6(FAtmosphericCell& Cell, float DT, float Z_m);
    void Micro_Morrison(FAtmosphericCell& Cell, float DT, float Z_m);
    void Micro_Thompson(FAtmosphericCell& Cell, float DT, float Z_m);

    //── Microphysics sub-processes ────────────────────────
    float SaturationMixingRatio(float T_K, float P_Pa) const;
    float ClaudiusClapeyron_Es(float T_K)             const;
    void  ProcessCondensation(FAtmosphericCell& C, float DT) const;
    void  ProcessAutoconversion(FAtmosphericCell& C, float DT) const;
    void  ProcessAccretion(FAtmosphericCell& C, float DT) const;
    void  ProcessEvaporation(FAtmosphericCell& C, float DT, float Z_m) const;
    void  ProcessFreezingRiming(FAtmosphericCell& C, float DT) const;
    void  ProcessMelting(FAtmosphericCell& C, float DT) const;
    void  ProcessHailGrowth(FAtmosphericCell& C, float DT) const;
    void  ProcessHailFragmentation(FAtmosphericCell& C, float DT) const;
    void  UpdateTerminalFallSpeeds(FAtmosphericCell& C) const;

    //── Sedimentation (precipitation fall) ────────────────
    void StepSedimentation(FSimulationState& S, float DT);

    //── Radiation (simplified broadband) ──────────────────
    void StepRadiation(FSimulationState& S, float DT, float SimTime_s);

    //── Column diagnostics ────────────────────────────────
    void ComputeAllColumns(FSimulationState& S);
    void ComputeColumnCAPE_CIN(const FSimulationState& S,
                               int32 Cx, int32 Cy,
                               FAtmosphericColumn& Col) const;
    void ComputeColumnSRH(const FSimulationState& S,
                          int32 Cx, int32 Cy,
                          FAtmosphericColumn& Col) const;
    void ComputeShearParams(const FSimulationState& S,
                            int32 Cx, int32 Cy,
                            FAtmosphericColumn& Col) const;
    FVector BunkerStormMotion(const FSimulationState& S, int32 Cx, int32 Cy) const;

    //── Reflectivity for radar ────────────────────────────
    void ComputeReflectivity(FSimulationState& S);
    float Z_from_RainMR(float Qr, float Rho) const;
    float Z_from_SnowMR(float Qs, float Rho) const;
    float Z_from_HailMR(float Qg, float Rho) const;

    //── Advection helpers ─────────────────────────────────
    float Advect5thOrder(const FSimulationState& S, int32 x, int32 y, int32 z,
                         float U, float V, float W,
                         const TFunctionRef<float(int32, int32, int32)>& GetVal,
                         float Inv_DX, float Inv_DZ) const;

    //── Helpers ───────────────────────────────────────────
    FVector CellToWorld(int32 x, int32 y, int32 z) const;
    bool    WorldToCell(const FVector& Pos, int32& Cx, int32& Cy, int32& Cz) const;
    float   GetAltitude(int32 z) const;

    //── Optimisation ──────────────────────────────────────
    void PublishRenderGrid();
    TArray<FStormCell> DetectStormsInternal(const FSimulationState& State);
    void TrackStormCellsInternal(TArray<FStormCell>& NewCells);

    //── State ─────────────────────────────────────────────
    FWeatherSimConfig    Config;
    FSimulationState     CurrentState;
    FSimulationState     TempStateA;   // RK4 buffer
    FSimulationState     TempStateB;
    TArray<FAtmosphericCell> K1, K2, K3, K4; // RK4 tendencies

    // Render Data Double Buffering
    FWeatherDoubleBuffer DoubleBuffer;

    // Column cache — updated every ~2s on sim thread
    mutable FCriticalSection    ColumnMutex;
    TArray<FAtmosphericColumn>  CachedColumns;
    std::atomic<bool>           bColumnsReady{ false };
    float                       LastColumnUpdate = -999.f;

    // Storm cache — updated every ~5s on sim thread
    mutable FCriticalSection    StormMutex;
    TArray<FStormCell>          CachedStorms;
    std::atomic<bool>           bStormsReady{ false };
    float                       LastStormUpdate = -999.f;

    // Internal Storm tracking
    TArray<FStormCell>          InternalTrackedStorms;
    int32                       NextTrackID = 0;

    TQueue<FSimCommand, EQueueMode::Mpsc> CommandQueue;

    std::atomic<float> SimTime{ 0.f };
    FThreadSafeBool    bIsRunning;
    FThreadSafeBool    bShouldStop;

    FRunnableThread* Thread = nullptr;

    // Cached inverse spacings
    float InvDX = 0.f, InvDY = 0.f, InvDZ = 0.f;
};