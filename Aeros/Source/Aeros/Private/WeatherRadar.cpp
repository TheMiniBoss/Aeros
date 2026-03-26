// Fill out your copyright notice in the Description page of Project Settings.

#include "WeatherRadar.h"
#include "WeatherSubsystem.h"
#include "Misc/ScopeLock.h"

// ============================================================
//  INITIALISE
// ============================================================

void UWeatherRadar::Initialise(UWeatherSubsystem* InSubsystem)
{
    Subsystem = InSubsystem;
    CurrentVolumeScan.RadarLocation = RadarLocation;
    CurrentVolumeScan.MaxRange_km = MaxRange_km;
}

void UWeatherRadar::SetLocation(const FVector& WorldPos)
{
    RadarLocation = WorldPos;
    CurrentVolumeScan.RadarLocation = RadarLocation;
}

void UWeatherRadar::OnNewSimulationData(const FWeatherDoubleBuffer::FReadView& InView, const FWeatherSimConfig& Config)
{
    FScopeLock Lock(&Statelock);
    LatestView = InView;
    LatestConfig = Config;
    // On vérifie que le pointeur Grid n'est pas nul
    bHasState = (LatestView.Grid != nullptr);
}

// ============================================================
//  TICK — drives antenna rotation & scanning
// ============================================================

void UWeatherRadar::Tick(float DeltaTime)
{
    if (!bIsScanning) return;

    FScopeLock StateLock(&Statelock);
    if (!bHasState) return;

    TArray<float> Elevations = GetVCPElevations(ScanMode);
    const float AzStep_deg = 360.f / NumAzimuths;

    ScanTimer += DeltaTime;
    float AzPerStep = AntennaSweepRate * DeltaTime; // degrees moved this frame

    // Progress azimuth
    CurrentAzimuth_deg += AzPerStep;

    // For each elevation being scanned this frame
    while (CurrentAzimuth_deg >= 360.f)
    {
        CurrentAzimuth_deg -= 360.f;
        ++CurrentElevIdx;

        if (CurrentElevIdx >= Elevations.Num())
        {
            // Volume scan complete — finalise products
            CurrentElevIdx = 0;
            PreviousVolumeScan = CurrentVolumeScan;
            {
                FScopeLock Lock(&ScanLock);
                BuildCAPPI(CurrentVolumeScan);
                BuildEchoTop(CurrentVolumeScan);
                BuildVIL(CurrentVolumeScan);
                CurrentVolumeScan.ScanStartTime = ScanTimer;
                bVolumeScanReady = true;
            }

            if (Subsystem)
                Subsystem->OnRadarScanComplete.Broadcast(CurrentVolumeScan);

            // Re-initialise for next volume
            CurrentVolumeScan.Sweeps.Empty();
            CurrentVolumeScan.RadarLocation = RadarLocation;
        }
    }

    CurrentElevation_deg = Elevations.IsValidIndex(CurrentElevIdx) ?
        Elevations[CurrentElevIdx] : 0.5f;

    // Compute one azimuth ray per tick at current elevation
    while (CurrentVolumeScan.Sweeps.Num() <= CurrentElevIdx)
    {
        FRadarSweep NewSweep;
        NewSweep.Elevation_deg = CurrentElevation_deg;
        NewSweep.Timestamp = ScanTimer;
        CurrentVolumeScan.Sweeps.Add(NewSweep);
    }

    auto& ActiveSweep = CurrentVolumeScan.Sweeps[CurrentElevIdx];
    ComputeBeam(CurrentElevation_deg, CurrentAzimuth_deg, ActiveSweep);
}

// ============================================================
//  SCAN CONTROL
// ============================================================

void UWeatherRadar::StartScan(ERadarScanMode Mode)
{
    ScanMode = Mode;
    bIsScanning = true;
    CurrentElevIdx = 0;
    CurrentAzimuth_deg = 0.f;
    CurrentVolumeScan.Sweeps.Empty();
}

void UWeatherRadar::StopScan() { bIsScanning = false; }
void UWeatherRadar::SetScanMode(ERadarScanMode Mode) { ScanMode = Mode; }
void UWeatherRadar::SetMaxRange(float R) { MaxRange_km = R; }
void UWeatherRadar::SetPRF(float F) { PRF_Hz = F; }

float UWeatherRadar::GetNyquistVelocity() const
{
    // V_max = (PRF * lambda) / 4
    return (PRF_Hz * Wavelength_m) / 4.f;
}

float UWeatherRadar::GetMaxUnambiguousRange_km() const
{
    // R_max = c / (2 * PRF)
    const float c = 3e8f; // m/s
    return (c / (2.f * PRF_Hz)) / 1000.f;
}

UTexture2D* UWeatherRadar::GenerateRadarTexture(const TArray<float>& ReflectivityData, int32 GridX, int32 GridY)
{
    if (ReflectivityData.Num() != (GridX * GridY) || GridX <= 0 || GridY <= 0) return nullptr;

    UTexture2D* RadarTexture = UTexture2D::CreateTransient(GridX, GridY, PF_B8G8R8A8);
    if (!RadarTexture) return nullptr;

    FTexture2DMipMap& Mip = RadarTexture->GetPlatformData()->Mips[0];
    void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FColor* PixelData = static_cast<FColor*>(Data);

    for (int32 i = 0; i < ReflectivityData.Num(); ++i)
    {
        float dBZ = ReflectivityData[i];
        FColor PixelColor = FColor::Transparent;

        if (dBZ >= 60.f)      PixelColor = FColor(255, 0, 255, 255);
        else if (dBZ >= 50.f) PixelColor = FColor(255, 0, 0, 255);
        else if (dBZ >= 40.f) PixelColor = FColor(255, 128, 0, 255);
        else if (dBZ >= 30.f) PixelColor = FColor(255, 255, 0, 255);
        else if (dBZ >= 20.f) PixelColor = FColor(0, 255, 0, 255);
        else if (dBZ >= 5.f)  PixelColor = FColor(0, 150, 255, 150);

        PixelData[i] = PixelColor;
    }

    Mip.BulkData.Unlock();
    RadarTexture->UpdateResource();
    return RadarTexture;
}

// ============================================================
//  BEAM COMPUTATION
// ============================================================

void UWeatherRadar::ComputeBeam(float Elevation_deg, float Azimuth_deg,
                                FRadarSweep& Sweep)
{
    FScopeLock StateLock(&Statelock);
    if (!bHasState) return;

    const float MaxRange_m = MaxRange_km * 1000.f;
    const int32 NumGates = FMath::RoundToInt(MaxRange_m / GateSpacing_m);

    for (int32 g = 0; g < NumGates; ++g)
    {
        float Range_m = (g + 0.5f) * GateSpacing_m;
        FRadarGate Gate = ComputeGate(Range_m, Azimuth_deg, Elevation_deg,
                                      LatestView, LatestConfig);
        Sweep.Gates.Add(Gate);
    }
}

FRadarGate UWeatherRadar::ComputeGate(float Range_m, float Az_deg, float El_deg,
                                      const FWeatherDoubleBuffer::FReadView& View,
                                      const FWeatherSimConfig& Cfg) const
{
    FRadarGate Gate;
    Gate.Range_m = Range_m;
    Gate.Azimuth_deg = Az_deg;
    Gate.Elevation_deg = El_deg;
    Gate.WorldPosition = GateWorldPosition(Range_m, Az_deg, El_deg);

    FVector Local = Gate.WorldPosition - Cfg.WorldOrigin;
    // On prend en compte le Downsample du buffer allégé
    float DX_cm = Cfg.DX_km * 100000.f * FWeatherDoubleBuffer::RenderDownsample;
    float DZ_cm = Cfg.DZ_m * 100.f * FWeatherDoubleBuffer::RenderDownsample;

    int32 Cx = (int32)(Local.X / DX_cm);
    int32 Cy = (int32)(Local.Y / DX_cm);
    int32 Cz = FMath::Max(0, (int32)(Local.Z / DZ_cm));

    if (Cx < 0 || Cy < 0 || Cx >= View.RNX || Cy >= View.RNY || Cz >= View.RNZ)
    {
        Gate.bIsValid = false;
        Gate.Reflectivity_dBZ = -32.f;
        return Gate;
    }

    const auto& Cell = View.At(Cx, Cy, Cz);
    Gate.bIsValid = true;
    Gate.Reflectivity_dBZ = Cell.Reflectivity_dBZ - BeamAtmosphericAttenuation(Range_m, Cell.Reflectivity_dBZ) - BeamBlockage(El_deg, Az_deg, Range_m);

    float Az_rad = FMath::DegreesToRadians(Az_deg);
    float El_rad = FMath::DegreesToRadians(El_deg);
    FVector BeamDir(FMath::Sin(Az_rad) * FMath::Cos(El_rad), FMath::Cos(Az_rad) * FMath::Cos(El_rad), FMath::Sin(El_rad));

    Gate.RadialVelocity_ms = FVector::DotProduct(FVector(Cell.U, Cell.V, Cell.W), BeamDir);
    float Vn = GetNyquistVelocity();
    if (FMath::Abs(Gate.RadialVelocity_ms) > Vn) Gate.RadialVelocity_ms = FMath::Fmod(Gate.RadialVelocity_ms + Vn, 2.f * Vn) - Vn;

    Gate.SpectrumWidth = FMath::Sqrt(FMath::Max(Cell.TKE * 0.667f, 0.f));
    Gate.DifferentialRef = Compute_ZDR(Cell);
    Gate.KDP = Compute_KDP(Cell);
    Gate.CorrelationCoeff = Compute_CC(Cell);

    return Gate;
}

// ============================================================
//  BEAM GEOMETRY
// ============================================================

FVector UWeatherRadar::GateWorldPosition(float Range_m, float Az_deg,
                                         float El_deg) const
{
    float Az_rad = FMath::DegreesToRadians(Az_deg);
    float El_rad = FMath::DegreesToRadians(El_deg);

    // Account for Earth curvature (4/3 effective Earth radius model)
    const float Re_eff = 4.f / 3.f * 6371000.f;
    float H = FMath::Sqrt(Range_m * Range_m + Re_eff * Re_eff +
                          2.f * Range_m * Re_eff * FMath::Sin(El_rad)) - Re_eff;

    float GndRange = Re_eff * FMath::Asin(Range_m * FMath::Cos(El_rad) / (Re_eff + H));

    FVector Offset(
        GndRange * FMath::Sin(Az_rad) * 100.f, // m → cm (Unreal units)
        GndRange * FMath::Cos(Az_rad) * 100.f,
        H * 100.f);

    return RadarLocation + Offset;
}

float UWeatherRadar::BeamHeight_m(float Range_m, float Elevation_rad) const
{
    const float Re_eff = 4.f / 3.f * 6371000.f;
    return FMath::Sqrt(Range_m * Range_m + Re_eff * Re_eff +
                       2.f * Range_m * Re_eff * FMath::Sin(Elevation_rad)) - Re_eff;
}

float UWeatherRadar::BeamAtmosphericAttenuation(float Range_m, float Z_dBZ) const
{
    // Two-way specific attenuation at S-band: ~0.0005 dB/km/dBZ
    // Simplified: ~0.01 dB/km for heavy rain
    float Rain_attn = 0.01f * Range_m / 1000.f;
    return FMath::Min(Rain_attn, 20.f); // cap at 20 dB
}

float UWeatherRadar::BeamBlockage(float /*El*/, float /*Az*/, float /*R*/) const
{
    return 0.f; // Terrain blockage would require DEM data
}

// ============================================================
//  DUAL-POL PARAMETERS
// ============================================================

float UWeatherRadar::Compute_ZDR(const FWeatherRenderCell& Cell) const
{
    // ZDR (dB): pluie oblate → positif, glace → positif, grêle → ~0
    if (Cell.RainRate > 1.f)
        return FMath::Clamp(0.5f + Cell.RainRate * 0.05f, 0.f, 4.f);

    if (Cell.HailMR > 1e-4f)
        return FMath::FRandRange(-0.2f, 0.2f); // Grêle = rebondit/tournoie = ~0

    if (Cell.CloudFraction > 0.1f)
        return FMath::FRandRange(0.5f, 1.5f);

    return 0.f;
}

float UWeatherRadar::Compute_KDP(const FWeatherRenderCell& Cell) const
{
    // KDP (°/km): proportionnel à la quantité d'eau liquide
    return Cell.RainRate > 1.f ? 0.3f * FMath::Pow(Cell.RainRate * 0.05f, 1.17f) : 0.f;
}

float UWeatherRadar::Compute_CC(const FWeatherRenderCell& Cell) const
{
    // ρhv: ~1.0 pour pluie, plus bas pour grêle
    if (Cell.HailMR > 1e-4f) return 0.75f;
    if (Cell.RainRate > 0.1f) return 0.99f;
    if (Cell.CloudFraction > 0.1f) return 0.95f;
    return 1.f;
}

// ============================================================
//  CAPPI / PRODUCTS
// ============================================================

void UWeatherRadar::BuildCAPPI(FRadarVolumeScan& Scan) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return;

    const auto& S = LatestView;
    const auto& Cfg = LatestConfig;
    int32 N = S.RNX * S.RNY;

    Scan.CAPPI_1km.SetNum(N);
    Scan.CAPPI_3km.SetNum(N);

    float DZ1 = 1000.f, DZ3 = 3000.f;

    for (int32 y = 0; y < S.RNY; ++y)
        for (int32 x = 0; x < S.RNX; ++x)
        {
            float Z1 = -32.f, Z3 = -32.f;

            for (int32 z = 0; z < S.RNZ; ++z)
            {
                float Alt = Cfg.DZ_m * z * (1.f + 0.05f * z);
                float dBZ = S.At(x, y, z).Reflectivity_dBZ;

                if (FMath::Abs(Alt - DZ1) < Cfg.DZ_m)
                    Z1 = FMath::Max(Z1, dBZ);
                if (FMath::Abs(Alt - DZ3) < Cfg.DZ_m)
                    Z3 = FMath::Max(Z3, dBZ);
            }

            Scan.CAPPI_1km[x + y * S.RNX] = Z1;
            Scan.CAPPI_3km[x + y * S.RNX] = Z3;
        }
}

void UWeatherRadar::BuildEchoTop(FRadarVolumeScan& Scan) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return;

    const auto& S = LatestView;
    const auto& Cfg = LatestConfig;

    const float EchoTopThresh = 18.f; // dBZ
    Scan.EchoTop_km.SetNum(S.RNX * S.RNY, 0.f);

    for (int32 y = 0; y < S.RNY; ++y)
        for (int32 x = 0; x < S.RNX; ++x)
        {
            float MaxAlt = 0.f;
            for (int32 z = S.RNZ - 1; z >= 0; --z)
            {
                if (S.At(x, y, z).Reflectivity_dBZ >= EchoTopThresh)
                {
                    MaxAlt = Cfg.DZ_m * z * (1.f + 0.05f * z) / 1000.f;
                    break;
                }
            }
            Scan.EchoTop_km[x + y * S.RNX] = MaxAlt;
        }
}

void UWeatherRadar::BuildVIL(FRadarVolumeScan& Scan) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return;

    const auto& S = LatestView;
    const auto& Cfg = LatestConfig;

    // VIL = 3.44 × 10⁻⁶ Σ ((Z1^4/7 + Z2^4/7)/2 * Δh)
    TArray<float> VIL;
    VIL.SetNum(S.RNX * S.RNY, 0.f);

    for (int32 y = 0; y < S.RNY; ++y)
        for (int32 x = 0; x < S.RNX; ++x)
        {
            float Sum = 0.f;
            for (int32 z = 0; z < S.RNZ - 1; ++z)
            {
                float Z1 = S.At(x, y, z).Reflectivity_dBZ;
                float Z2 = S.At(x, y, z + 1).Reflectivity_dBZ;
                if (Z1 < 18.f && Z2 < 18.f) continue;

                float Lin1 = FMath::Max(0.f, FMath::Pow(10.f, Z1 * 0.1f));
                float Lin2 = FMath::Max(0.f, FMath::Pow(10.f, Z2 * 0.1f));
                float DZ_m = Cfg.DZ_m * (1.f + 0.1f * z);
                Sum += (FMath::Pow(Lin1, 4.f / 7.f) + FMath::Pow(Lin2, 4.f / 7.f)) * 0.5f * DZ_m;
            }
            VIL[x + y * S.RNX] = 3.44e-6f * Sum;
        }

    // Stored in EchoTop array slot (reuse)
    Scan.EchoTop_km = VIL; // Note: caller should use GetVIL() not EchoTop
}

// ============================================================
//  DATA ACCESS
// ============================================================

FRadarVolumeScan UWeatherRadar::GetLatestVolumeScan() const
{
    FScopeLock Lock(&ScanLock);
    return CurrentVolumeScan;
}

TArray<FRadarGate> UWeatherRadar::GetPPISweep(float Elevation_deg) const
{
    FScopeLock Lock(&ScanLock);
    for (const auto& Sweep : CurrentVolumeScan.Sweeps)
    {
        if (FMath::Abs(Sweep.Elevation_deg - Elevation_deg) < 0.5f)
            return Sweep.Gates;
    }
    return {};
}

TArray<FRadarGate> UWeatherRadar::GetRHISweep(float Azimuth_deg) const
{
    // Collect all gates at this azimuth across all elevations
    FScopeLock Lock(&ScanLock);
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return {};

    TArray<FRadarGate> RHI;
    const int32 AzSearch = FMath::RoundToInt(Azimuth_deg);

    for (const auto& Sweep : CurrentVolumeScan.Sweeps)
    {
        // Find the azimuth index in this sweep
        for (const auto& Gate : Sweep.Gates)
        {
            if (FMath::Abs(Gate.Azimuth_deg - Azimuth_deg) < 1.f)
                RHI.Add(Gate);
        }
    }

    // Sort by range
    RHI.Sort([](const FRadarGate& A, const FRadarGate& B)
    { return A.Range_m < B.Range_m || (A.Range_m == B.Range_m && A.Elevation_deg < B.Elevation_deg); });

    return RHI;
}

TArray<float> UWeatherRadar::GetCAPPI(float Altitude_km, int32& OutX, int32& OutY) const
{
    FScopeLock Lock(&ScanLock);
    OutX = bHasState ? LatestView.RNX : 0;
    OutY = bHasState ? LatestView.RNY : 0;

    if (FMath::Abs(Altitude_km - 1.f) < 0.5f) return CurrentVolumeScan.CAPPI_1km;
    if (FMath::Abs(Altitude_km - 3.f) < 0.5f) return CurrentVolumeScan.CAPPI_3km;
    return {};
}

TArray<float> UWeatherRadar::GetCompositeReflectivity(int32& OutX, int32& OutY) const
{
    {
        FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
        if (!bHasState) { OutX = OutY = 0; return {}; }

        OutX = LatestView.RNX;
        OutY = LatestView.RNY;

        TArray<float> Comp;
        Comp.Init(-32.0f, OutX * OutY);

        for (int32 y = 0; y < OutY; ++y)
            for (int32 x = 0; x < OutX; ++x)
            {
                float MaxZ = -32.f;
                for (int32 z = 0; z < LatestView.RNZ; ++z)
                    MaxZ = FMath::Max(MaxZ, LatestView.At(x, y, z).Reflectivity_dBZ);

                Comp[x + y * OutX] = MaxZ;
            }
        return Comp;
    }
}

TArray<float> UWeatherRadar::GetEchoTop_km(int32& OutX, int32& OutY) const
{
    FScopeLock Lock(&ScanLock);
    OutX = bHasState ? LatestView.RNX : 0;
    OutY = bHasState ? LatestView.RNY : 0;
    return CurrentVolumeScan.EchoTop_km;
}

TArray<float> UWeatherRadar::GetVIL(int32& OutX, int32& OutY) const
{
    // VIL is stored in EchoTop_km after BuildVIL — ideally use separate array
    return GetEchoTop_km(OutX, OutY);
}

// ============================================================
//  POINT QUERIES
// ============================================================

float UWeatherRadar::GetReflectivityAtPoint(const FVector& WorldPos) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return -32.f;

    FVector Local = WorldPos - LatestConfig.WorldOrigin;
    float DX_cm = LatestConfig.DX_km * 100000.f;
    int32 Cx = (int32)(Local.X / DX_cm);
    int32 Cy = (int32)(Local.Y / DX_cm);
    int32 Cz = (int32)(Local.Z / (LatestConfig.DZ_m * 100.f));

    if (Cx < 0 || Cy < 0 || Cx >= LatestView.RNX ||
        Cy >= LatestView.RNY || Cz >= LatestView.RNZ) return -32.f;

    return LatestView.At(Cx, Cy, Cz).Reflectivity_dBZ;
}

float UWeatherRadar::GetRadialVelocityAtPoint(const FVector& WorldPos) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return 0.f;

    FVector Dir = WorldPos - RadarLocation;
    Dir.Normalize();

    FVector Local = WorldPos - LatestConfig.WorldOrigin;
    float DX_cm = LatestConfig.DX_km * 100000.f;
    int32 Cx = (int32)(Local.X / DX_cm);
    int32 Cy = (int32)(Local.Y / DX_cm);
    int32 Cz = (int32)(Local.Z / (LatestConfig.DZ_m * 100.f));

    if (Cx < 0 || Cy < 0 || Cx >= LatestView.RNX ||
        Cy >= LatestView.RNY || Cz >= LatestView.RNZ) return 0.f;

    const auto& Cell = LatestView.At(Cx, Cy, Cz);
    return FVector::DotProduct(FVector(Cell.U, Cell.V, Cell.W), Dir);
}

float UWeatherRadar::GetZDR_AtPoint(const FVector& WorldPos) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return 0.f;

    FVector Local = WorldPos - LatestConfig.WorldOrigin;
    float DX_cm = LatestConfig.DX_km * 100000.f * FWeatherDoubleBuffer::RenderDownsample;
    float DZ_cm = LatestConfig.DZ_m * 100.f * FWeatherDoubleBuffer::RenderDownsample;

    int32 Cx = FMath::Clamp((int32)(Local.X / DX_cm), 0, LatestView.RNX - 1);
    int32 Cy = FMath::Clamp((int32)(Local.Y / DX_cm), 0, LatestView.RNY - 1);
    int32 Cz = FMath::Clamp((int32)(Local.Z / DZ_cm), 0, LatestView.RNZ - 1);

    return Compute_ZDR(LatestView.At(Cx, Cy, Cz));
}

float UWeatherRadar::GetCC_AtPoint(const FVector& WorldPos) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return 1.f;

    FVector Local = WorldPos - LatestConfig.WorldOrigin;
    float DX_cm = LatestConfig.DX_km * 100000.f * FWeatherDoubleBuffer::RenderDownsample;
    float DZ_cm = LatestConfig.DZ_m * 100.f * FWeatherDoubleBuffer::RenderDownsample;

    int32 Cx = FMath::Clamp((int32)(Local.X / DX_cm), 0, LatestView.RNX - 1);
    int32 Cy = FMath::Clamp((int32)(Local.Y / DX_cm), 0, LatestView.RNY - 1);
    int32 Cz = FMath::Clamp((int32)(Local.Z / DZ_cm), 0, LatestView.RNZ - 1);

    return Compute_CC(LatestView.At(Cx, Cy, Cz));
}

float UWeatherRadar::GetKDP_AtPoint(const FVector& WorldPos) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return 0.f;

    FVector Local = WorldPos - LatestConfig.WorldOrigin;
    float DX_cm = LatestConfig.DX_km * 100000.f * FWeatherDoubleBuffer::RenderDownsample;
    float DZ_cm = LatestConfig.DZ_m * 100.f * FWeatherDoubleBuffer::RenderDownsample;

    int32 Cx = FMath::Clamp((int32)(Local.X / DX_cm), 0, LatestView.RNX - 1);
    int32 Cy = FMath::Clamp((int32)(Local.Y / DX_cm), 0, LatestView.RNY - 1);
    int32 Cz = FMath::Clamp((int32)(Local.Z / DZ_cm), 0, LatestView.RNZ - 1);

    return Compute_KDP(LatestView.At(Cx, Cy, Cz));
}

// ============================================================
//  MESOCYCLONE DETECTION  (MDA algorithm, simplified)
// ============================================================

TArray<FMesocycloneDetection> UWeatherRadar::DetectMesocyclones() const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    if (!bHasState) return {};

    TArray<FMesocycloneDetection> Detections;
    const auto& S = LatestView;
    const auto& Cfg = LatestConfig;
    float DX_cm = Cfg.DX_km * 100000.f;

    // Detection thresholds (NWS MDA criteria)
    const float MinRotVel_ms = 10.f;  // m/s
    const float MinDepth_km = 3.f;
    const float MinVorticity = 0.01f; // 1/s

    for (int32 y = 2; y < S.RNY - 2; ++y)
        for (int32 x = 2; x < S.RNX - 2; ++x)
        {
            float MaxVort = 0.f, MaxW = 0.f;
            int32 BaseZ = -1, TopZ = -1;

            for (int32 z = 0; z < S.RNZ; ++z)
            {
                const auto& C = S.At(x, y, z);
                float V = FMath::Abs(C.VortZ);
                if (V > MaxVort) MaxVort = V;
                if (C.W > MaxW)  MaxW = C.W;

                if (V > MinVorticity)
                {
                    if (BaseZ < 0) BaseZ = z;
                    TopZ = z;
                }
            }

            if (MaxVort < MinVorticity || MaxW < 15.f) continue;

            float DepthAlt = (TopZ - FMath::Max(BaseZ, 0)) * Cfg.DZ_m / 1000.f;
            if (DepthAlt < MinDepth_km) continue;

            FMesocycloneDetection Det;
            Det.Position = RadarLocation + FVector((float)x * DX_cm, (float)y * DX_cm, 0.f);
            Det.Range_km = FVector::Dist2D(Det.Position, RadarLocation) / 100000.f;
            Det.Azimuth_deg = FMath::RadiansToDegrees(
                FMath::Atan2(Det.Position.Y - RadarLocation.Y,
                Det.Position.X - RadarLocation.X));
            Det.Depth_km = DepthAlt;
            Det.Diameter_km = Cfg.DX_km * 2.f;
            Det.RotVelocity_ms = MaxVort * 1000.f; // 1/s * m ≈ m/s at 1km
            Det.bTVS = Det.RotVelocity_ms > 20.f && MaxW > 30.f;
            Det.bMDAAlert = true;

            // Rank 1–5
            if (Det.RotVelocity_ms > 40.f) Det.Rank = 5;
            else if (Det.RotVelocity_ms > 30.f) Det.Rank = 4;
            else if (Det.RotVelocity_ms > 20.f) Det.Rank = 3;
            else if (Det.RotVelocity_ms > 15.f) Det.Rank = 2;
            else                                 Det.Rank = 1;

            Detections.Add(Det);
        }

    return Detections;
}

// ============================================================
//  STORM TRACKING AND INTENSITY (STI)
// ============================================================

TArray<FStormCell> UWeatherRadar::STI_StormTracking() const
{
    if (!Subsystem) return {};
    return Subsystem->GetAllDetectedStorms();
}

// ============================================================
//  VAD WIND PROFILE
// ============================================================

FVADWindProfile UWeatherRadar::ComputeVADWindProfile(float Elevation_deg) const
{
    FScopeLock StateLock(const_cast<FCriticalSection*>(&Statelock));
    FVADWindProfile Profile;
    if (!bHasState) return Profile;

    const auto& S = LatestView;
    const auto& Cfg = LatestConfig;

    // For each height level, fit sinusoid to radial velocity vs azimuth
    // V_r(az) = V_H cos(az - az_H) * cos(El) + W * sin(El)
    // Extract V_H (horiz wind speed) and az_H (direction) by harmonic analysis

    for (int32 z = 0; z < S.RNZ; ++z)
    {
        float Alt_m = Cfg.DZ_m * z * (1.f + 0.05f * z);
        // Take ring of points at this altitude
        const int32 Cx = S.RNX / 2, Cy = S.RNY / 2;
        const int32 Radius = 20; // cells

        float SumSin = 0.f, SumCos = 0.f, SumVr = 0.f;
        float SumVrSin = 0.f, SumVrCos = 0.f;
        int32 N = 0;

        for (int32 az = 0; az < 360; az += 10)
        {
            float Az_rad = FMath::DegreesToRadians((float)az);
            int32 Px = Cx + (int32)(Radius * FMath::Sin(Az_rad));
            int32 Py = Cy + (int32)(Radius * FMath::Cos(Az_rad));

            if (Px < 0 || Py < 0 || Px >= S.RNX || Py >= S.RNY) continue;
            const auto& C = S.At(Px, Py, z);

            float El_rad = FMath::DegreesToRadians(Elevation_deg);
            FVector BeamDir(FMath::Sin(Az_rad) * FMath::Cos(El_rad),
                            FMath::Cos(Az_rad) * FMath::Cos(El_rad),
                            FMath::Sin(El_rad));
            float Vr = FVector::DotProduct(FVector(C.U, C.V, C.W), BeamDir);

            SumVrSin += Vr * FMath::Sin(Az_rad);
            SumVrCos += Vr * FMath::Cos(Az_rad);
            ++N;
        }

        if (N == 0) continue;

        float A1 = 2.f * SumVrSin / N;
        float B1 = 2.f * SumVrCos / N;
        float SpeedH = FMath::Sqrt(A1 * A1 + B1 * B1);
        float DirRad = FMath::Atan2(-B1, A1) + FMath::DegreesToRadians(180.f);

        float DivLevel = (SumVrSin + SumVrCos) / N * 1e-3f; // 10⁻³ s⁻¹

        Profile.Altitude_m.Add(Alt_m);
        Profile.WindSpeed_ms.Add(SpeedH);
        Profile.WindDir_deg.Add(FMath::RadiansToDegrees(DirRad));
        Profile.DivergenceLevel.Add(DivLevel);
    }

    return Profile;
}

// ============================================================
//  QPE  — Quantitative Precipitation Estimation
// ============================================================

float UWeatherRadar::EstimateRainfallRate_QPE(const FVector& WorldPos) const
{
    float Z_dBZ = GetReflectivityAtPoint(WorldPos);
    if (Z_dBZ < 10.f) return 0.f;

    // R = (Z / 200)^(1/1.6)  (Marshall-Palmer Z-R)
    float Z_lin = FMath::Pow(10.f, Z_dBZ * 0.1f);
    return FMath::Pow(Z_lin / 200.f, 1.f / 1.6f); // mm/hr
}

// ============================================================
//  VCP ELEVATION SCHEDULES
// ============================================================

TArray<float> UWeatherRadar::GetVCPElevations(ERadarScanMode Mode) const
{
    switch (Mode)
    {
        case ERadarScanMode::VCP21:
            return { 0.5f, 1.5f, 2.4f, 3.4f, 4.3f, 6.0f, 9.9f, 14.6f, 19.5f };

        case ERadarScanMode::VCP212:
            return { 0.5f, 0.9f, 1.3f, 1.8f, 2.4f, 3.1f, 4.0f,
                     5.1f, 6.4f, 8.0f, 10.0f, 12.5f, 15.6f, 19.5f };

        case ERadarScanMode::VCP31:
            return { 0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f };

        case ERadarScanMode::VCP35:
            return { 0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f,
                     6.5f, 7.5f, 8.5f, 9.5f, 10.5f };

        case ERadarScanMode::Continuous:
            return { 0.5f, 2.4f, 4.3f, 9.9f, 19.5f };

        default:
            return { 0.5f };
    }
}