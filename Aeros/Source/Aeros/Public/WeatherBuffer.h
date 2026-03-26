#pragma once

#include "CoreMinimal.h"
#include "WeatherTypes.h"
#include <atomic>

// ============================================================
//  SLIM RENDER CELL  — game-thread readable, tiny footprint
//  Full FAtmosphericCell ~240 bytes. This is 40 bytes.
// ============================================================
struct FWeatherRenderCell
{
    float U, V, W;              // wind m/s
    float Temperature;          // K
    float RelativeHumidity;     // 0-1
    float CloudFraction;        // Ql+Qi integrated
    float RainRate;             // mm/hr
    float Reflectivity_dBZ;
    float TKE;
    float VortZ;
    float HailMR;               // hail mixing ratio
    uint8 PhenomenonByte;       // EWeatherPhenomenon cast

    EWeatherPhenomenon GetPhenomenon() const
    {
        return static_cast<EWeatherPhenomenon>(PhenomenonByte);
    }
};

// ============================================================
//  LOCK-FREE DOUBLE BUFFER
//  Sim thread writes to back, game thread reads from front.
//  Swap is atomic — game thread NEVER blocks.
// ============================================================
class FWeatherDoubleBuffer
{
public:
    void Initialise(int32 InNX, int32 InNY, int32 InNZ)
    {
        NX = InNX; NY = InNY; NZ = InNZ;
        const int32 RenderNX = FMath::CeilToInt(NX / float(RenderDownsample));
        const int32 RenderNY = FMath::CeilToInt(NY / float(RenderDownsample));
        const int32 RenderNZ = FMath::CeilToInt(NZ / float(RenderDownsample));

        for (int32 i = 0; i < 2; ++i)
        {
            Buffers[i].RenderGrid.SetNum(RenderNX * RenderNY * RenderNZ);
            Buffers[i].RNX = RenderNX;
            Buffers[i].RNY = RenderNY;
            Buffers[i].RNZ = RenderNZ;
            Buffers[i].SimTime = 0.f;
            Buffers[i].Generation = 0;
        }
        ReadIdx.store(0);
        WriteIdx.store(1);
    }

    // Called by SIM THREAD after computing render data
    void BeginWrite() { /* write to Buffers[WriteIdx] */ }
    void CommitWrite(float SimTime)
    {
        int32 W = WriteIdx.load(std::memory_order_relaxed);
        Buffers[W].SimTime = SimTime;
        Buffers[W].Generation += 1;

        // Atomic swap: front ↔ back
        int32 R = ReadIdx.exchange(W, std::memory_order_acq_rel);
        WriteIdx.store(R, std::memory_order_release);
    }

    // Called by GAME THREAD — NEVER blocks
    struct FReadView
    {
        const FWeatherRenderCell* Grid = nullptr;
        int32 RNX = 0, RNY = 0, RNZ = 0;
        float SimTime = 0.f;
        uint64 Generation = 0;

        const FWeatherRenderCell& At(int32 x, int32 y, int32 z) const
        {
            static FWeatherRenderCell Empty{};
            if (!Grid) return Empty;
            x = (x % RNX + RNX) % RNX;
            y = (y % RNY + RNY) % RNY;
            z = FMath::Clamp(z, 0, RNZ - 1);
            return Grid[x + y * RNX + z * RNX * RNY];
        }
    };

    FReadView GetReadView() const
    {
        int32 R = ReadIdx.load(std::memory_order_acquire);
        const auto& B = Buffers[R];
        FReadView V;
        V.Grid = B.RenderGrid.GetData();
        V.RNX = B.RNX;
        V.RNY = B.RNY;
        V.RNZ = B.RNZ;
        V.SimTime = B.SimTime;
        V.Generation = B.Generation;
        return V;
    }

    // Get writable back buffer (sim thread only!)
    TArray<FWeatherRenderCell>& GetWriteBuffer()
    {
        return Buffers[WriteIdx.load(std::memory_order_relaxed)].RenderGrid;
    }
    int32 GetWriteRNX() const { return Buffers[WriteIdx.load()].RNX; }
    int32 GetWriteRNY() const { return Buffers[WriteIdx.load()].RNY; }
    int32 GetWriteRNZ() const { return Buffers[WriteIdx.load()].RNZ; }

    // How many sim cells per render cell
    static constexpr int32 RenderDownsample = 2; // 128→64, 50→25

private:
    struct FBuffer
    {
        TArray<FWeatherRenderCell> RenderGrid;
        int32  RNX = 0, RNY = 0, RNZ = 0;
        float  SimTime = 0.f;
        uint64 Generation = 0;
    };

    FBuffer Buffers[2];
    std::atomic<int32> ReadIdx{ 0 };
    std::atomic<int32> WriteIdx{ 1 };
    int32 NX = 0, NY = 0, NZ = 0;
};

// ============================================================
//  COLUMN CACHE  — game-thread side, updated at low rate
// ============================================================
struct FColumnCache
{
    TArray<FAtmosphericColumn> Columns; // NX*NY
    int32 NX = 0, NY = 0;
    float LastUpdateTime = -1.f;
    float UpdateInterval = 2.f; // seconds — columns update every 2s

    bool NeedsUpdate(float Now) const
    {
        return (Now - LastUpdateTime) > UpdateInterval;
    }
};