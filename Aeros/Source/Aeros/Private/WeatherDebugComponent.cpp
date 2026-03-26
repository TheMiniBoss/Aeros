// Fill out your copyright notice in the Description page of Project Settings.


#include "WeatherDebugComponent.h"
#include "WeatherSubsystem.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Kismet/KismetSystemLibrary.h"

UWeatherDebugComponent::UWeatherDebugComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 0.05f; // 20 fps cap for debug
}

void UWeatherDebugComponent::BeginPlay()
{
    Super::BeginPlay();
    if (UWorld* World = GetWorld())
        WeatherSys = World->GetSubsystem<UWeatherSubsystem>();
}

// ============================================================
//  TICK
// ============================================================

void UWeatherDebugComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                           FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!WeatherSys) return;

    UWorld* W = GetWorld();
    FVector Cam = GetCameraPosition();

    if (Wind.bEnabled)               DrawWindLayer(W, Cam);
    if (Reflectivity.bEnabled)       DrawReflectivity(W, Cam);
    if (Temperature.bEnabled)        DrawTemperatureLayer(W, Cam);
    if (Vorticity.bEnabled)          DrawVorticityLayer(W, Cam);
    if (CloudLayer.bEnabled)         DrawCloudLayer(W, Cam);
    if (StormCells.bEnabled)         DrawStormCells(W);
    if (ColumnDiagnostics.bEnabled)  DrawColumnDiagnostics(W, Cam);
    if (RadarBeam.bEnabled)          DrawRadarBeam(W);
    if (Sounding.bEnabled)           DrawSounding(W, Cam);
}

// ============================================================
//  WIND LAYER  — coloured arrows
// ============================================================

void UWeatherDebugComponent::DrawWindLayer(UWorld* W, const FVector& CamPos)
{
    if (!W) return;
    const FWeatherSimConfig& Cfg = WeatherSys->GetConfiguration();
    const float DX_cm = Cfg.DX_km * 100000.f * HorizontalSampleStep;
    const float DZ_cm = Cfg.DZ_m * 100.f * VerticalSampleStep;
    const float Radius = DebugRadius_km * 100000.f;

    FVector Origin = WeatherSys->GetDomainOrigin();
    int32 NX = Cfg.GridNX / HorizontalSampleStep;
    int32 NY = Cfg.GridNY / HorizontalSampleStep;
    int32 NZ = Cfg.GridNZ / VerticalSampleStep;

    for (int32 iz = 0; iz < NZ; ++iz)
        for (int32 iy = 0; iy < NY; ++iy)
            for (int32 ix = 0; ix < NX; ++ix)
            {
                FVector Pos = Origin + FVector(ix * DX_cm, iy * DX_cm, iz * DZ_cm);
                if (!IsWithinDebugRadius(Pos, CamPos)) continue;
                if (AltitudeSlice_m >= 0.f &&
                    FMath::Abs(Pos.Z / 100.f - AltitudeSlice_m) > Cfg.DZ_m * VerticalSampleStep)
                    continue;

                FAtmosphericCell Cell;
                if (!WeatherSys->GetAtmosphereAtLocation(Pos, Cell)) continue;

                float Speed = FMath::Sqrt(Cell.U * Cell.U + Cell.V * Cell.V + Cell.W * Cell.W);
                if (Speed < 0.5f) continue;

                // Color: slow=cyan, fast=red
                FColor C = ValueToColor(Speed, 0.f, 30.f, FColor::Cyan, FColor::Red);
                FVector Dir(Cell.U, Cell.V, Cell.W);
                Dir.Normalize();

                DrawArrow(W, Pos, Dir, Speed, C, DrawDuration);

                if (bShowNumericalValues)
                {
                    DrawDebugString(W, Pos + FVector(0, 0, 50),
                                    FString::Printf(TEXT("%.1f m/s"), Speed),
                                    nullptr, C, DrawDuration);
                }
            }
}

// ============================================================
//  REFLECTIVITY LAYER  — NWS colour scale
// ============================================================

void UWeatherDebugComponent::DrawReflectivity(UWorld* W, const FVector& CamPos)
{
    if (!W) return;
    const FWeatherSimConfig& Cfg = WeatherSys->GetConfiguration();
    const float DX_cm = Cfg.DX_km * 100000.f * HorizontalSampleStep;
    const float DZ_cm = Cfg.DZ_m * 100.f * VerticalSampleStep;
    FVector Origin = WeatherSys->GetDomainOrigin();

    int32 NX = Cfg.GridNX / HorizontalSampleStep;
    int32 NY = Cfg.GridNY / HorizontalSampleStep;
    int32 NZ = Cfg.GridNZ / VerticalSampleStep;

    for (int32 iy = 0; iy < NY; ++iy)
        for (int32 ix = 0; ix < NX; ++ix)
        {
            float MaxdBZ = -32.f;
            FVector MaxPos = FVector::ZeroVector;

            for (int32 iz = 0; iz < NZ; ++iz)
            {
                FVector Pos = Origin + FVector(ix * DX_cm, iy * DX_cm, iz * DZ_cm);
                if (!IsWithinDebugRadius(Pos, CamPos)) continue;
                if (AltitudeSlice_m >= 0.f &&
                    FMath::Abs(Pos.Z / 100.f - AltitudeSlice_m) > Cfg.DZ_m * VerticalSampleStep)
                    continue;

                float dBZ = WeatherSys->GetReflectivity_dBZ(Pos);
                if (bShowOnlyHighestRefl && dBZ > MaxdBZ)
                {
                    MaxdBZ = dBZ; MaxPos = Pos;
                }
                else if (!bShowOnlyHighestRefl && dBZ > 10.f)
                {
                    DrawColoredPoint(W, Pos, ReflectivityColor(dBZ),
                                     Reflectivity.PointSize, DrawDuration);
                    if (bShowNumericalValues)
                        DrawDebugString(W, Pos, FString::Printf(TEXT("%.0f dBZ"), dBZ),
                                        nullptr, FColor::White, DrawDuration);
                }
            }

            if (bShowOnlyHighestRefl && MaxdBZ > 10.f)
            {
                DrawColoredPoint(W, MaxPos, ReflectivityColor(MaxdBZ),
                                 Reflectivity.PointSize, DrawDuration);
                if (bShowNumericalValues)
                    DrawDebugString(W, MaxPos + FVector(0, 0, 200),
                                    FString::Printf(TEXT("%.0f dBZ"), MaxdBZ),
                                    nullptr, FColor::White, DrawDuration);
            }
        }
}

// ============================================================
//  TEMPERATURE LAYER
// ============================================================

void UWeatherDebugComponent::DrawTemperatureLayer(UWorld* W, const FVector& CamPos)
{
    if (!W) return;
    const FWeatherSimConfig& Cfg = WeatherSys->GetConfiguration();
    const float DX_cm = Cfg.DX_km * 100000.f * HorizontalSampleStep;
    const float DZ_cm = Cfg.DZ_m * 100.f * VerticalSampleStep;
    FVector Origin = WeatherSys->GetDomainOrigin();

    int32 NX = Cfg.GridNX / HorizontalSampleStep;
    int32 NY = Cfg.GridNY / HorizontalSampleStep;
    int32 NZ = (AltitudeSlice_m < 0.f) ? Cfg.GridNZ / VerticalSampleStep : 1;

    for (int32 iz = 0; iz < NZ; ++iz)
        for (int32 iy = 0; iy < NY; ++iy)
            for (int32 ix = 0; ix < NX; ++ix)
            {
                float Z = (AltitudeSlice_m >= 0.f) ? AltitudeSlice_m * 100.f : iz * DZ_cm;
                FVector Pos = Origin + FVector(ix * DX_cm, iy * DX_cm, Z);
                if (!IsWithinDebugRadius(Pos, CamPos)) continue;

                float T = WeatherSys->GetTemperatureAtLocation(Pos);
                FColor C = TemperatureColor(T);
                DrawColoredPoint(W, Pos, C, Temperature.PointSize, DrawDuration);

                if (bShowNumericalValues)
                    DrawDebugString(W, Pos, FString::Printf(TEXT("%.0f K / %.0f°C"), T, T - 273.15f),
                                    nullptr, C, DrawDuration);
            }
}

// ============================================================
//  VORTICITY LAYER
// ============================================================

void UWeatherDebugComponent::DrawVorticityLayer(UWorld* W, const FVector& CamPos)
{
    if (!W) return;
    const FWeatherSimConfig& Cfg = WeatherSys->GetConfiguration();
    const float DX_cm = Cfg.DX_km * 100000.f * HorizontalSampleStep;
    const float DZ_cm = Cfg.DZ_m * 100.f * VerticalSampleStep;
    FVector Origin = WeatherSys->GetDomainOrigin();

    int32 NX = Cfg.GridNX / HorizontalSampleStep;
    int32 NY = Cfg.GridNY / HorizontalSampleStep;
    int32 NZ = Cfg.GridNZ / VerticalSampleStep;
    const float VortThresh = 0.003f; // only draw significant vorticity

    for (int32 iz = 0; iz < NZ; ++iz)
        for (int32 iy = 0; iy < NY; ++iy)
            for (int32 ix = 0; ix < NX; ++ix)
            {
                FVector Pos = Origin + FVector(ix * DX_cm, iy * DX_cm, iz * DZ_cm);
                if (!IsWithinDebugRadius(Pos, CamPos)) continue;

                FAtmosphericCell Cell;
                if (!WeatherSys->GetAtmosphereAtLocation(Pos, Cell)) continue;
                if (FMath::Abs(Cell.VortZ) < VortThresh) continue;

                FColor C = VorticityColor(Cell.VortZ);
                DrawColoredPoint(W, Pos, C, Vorticity.PointSize * FMath::Min(FMath::Abs(Cell.VortZ) / 0.01f, 3.f),
                                 DrawDuration);

                // Draw a spiral indicator for strong rotation
                if (FMath::Abs(Cell.VortZ) > 0.01f)
                {
                    float R = 500.f; // 5m in cm
                    for (int32 a = 0; a < 360; a += 30)
                    {
                        float A0 = FMath::DegreesToRadians(a);
                        float A1 = FMath::DegreesToRadians(a + 30.f);
                        FVector P0 = Pos + FVector(R * FMath::Cos(A0), R * FMath::Sin(A0), 0);
                        FVector P1 = Pos + FVector(R * FMath::Cos(A1), R * FMath::Sin(A1), 0);
                        DrawDebugLine(W, P0, P1, C, false, DrawDuration, 0, 20.f);
                    }
                }
            }
}

// ============================================================
//  CLOUD LAYER
// ============================================================

void UWeatherDebugComponent::DrawCloudLayer(UWorld* W, const FVector& CamPos)
{
    if (!W) return;
    const FWeatherSimConfig& Cfg = WeatherSys->GetConfiguration();
    const float DX_cm = Cfg.DX_km * 100000.f * HorizontalSampleStep;
    const float DZ_cm = Cfg.DZ_m * 100.f * VerticalSampleStep;
    FVector Origin = WeatherSys->GetDomainOrigin();

    int32 NX = Cfg.GridNX / HorizontalSampleStep;
    int32 NY = Cfg.GridNY / HorizontalSampleStep;
    int32 NZ = Cfg.GridNZ / VerticalSampleStep;

    for (int32 iz = 0; iz < NZ; ++iz)
        for (int32 iy = 0; iy < NY; ++iy)
            for (int32 ix = 0; ix < NX; ++ix)
            {
                FVector Pos = Origin + FVector(ix * DX_cm, iy * DX_cm, iz * DZ_cm);
                if (!IsWithinDebugRadius(Pos, CamPos)) continue;

                FAtmosphericCell Cell;
                if (!WeatherSys->GetAtmosphereAtLocation(Pos, Cell)) continue;

                float Cloud = Cell.Ql; // CloudFraction proxy
                if (Cloud < 0.05f) continue;

                uint8 Alpha = (uint8)(Cloud * 255.f);
                FColor CloudColor(200, 200, 255, Alpha);
                DrawColoredPoint(W, Pos, CloudColor, CloudLayer.PointSize * (0.5f + Cloud * 2.f),
                                 DrawDuration);
            }
}

// ============================================================
//  STORM CELLS
// ============================================================

void UWeatherDebugComponent::DrawStormCells(UWorld* W)
{
    if (!W) return;
    TArray<FStormCell> Storms = WeatherSys->GetAllDetectedStorms();

    for (const auto& Storm : Storms)
    {
        // Bounding cylinder
        float R_cm = Storm.EchoTop_km > 0.f ? 5000.f * 100.f : 2000.f * 100.f;
        float H_cm = Storm.EchoTop_km * 1000.f * 100.f;
        FColor C = PhenomenonColor(Storm.Type);

        DrawDebugCylinder(W, Storm.Position,
                          Storm.Position + FVector(0, 0, H_cm),
                          R_cm, 16, C, false, DrawDuration, 0, 30.f);

        // Mesocyclone ring
        if (Storm.bIsMesocyclone)
        {
            float Mr = Storm.MesocycloneDiameter_km * 0.5f * 100000.f;
            DrawDebugCircle(W, Storm.Position + FVector(0, 0, H_cm * 0.5f),
                            Mr, 32, FColor::Magenta, false, DrawDuration, 0, 50.f,
                            FVector(0, 0, 1), FVector(1, 0, 0));
        }

        // TVS tornado indicator
        if (Storm.bTVS)
        {
            DrawDebugCylinder(W, Storm.Position,
                              Storm.Position + FVector(0, 0, 5000.f * 100.f),
                              200.f * 100.f, 8, FColor::Red, false, DrawDuration, 0, 80.f);
        }

        // Motion vector
        FVector MotionVec = FVector(Storm.Motion_ms.X, Storm.Motion_ms.Y, 0.f)
            * 100.f * 60.f; // 1-minute motion
        DrawDebugDirectionalArrow(W, Storm.Position, Storm.Position + MotionVec,
                       2000.f, FColor::Yellow, false, DrawDuration, 0, 30.f);

        // Label
        FString Label = FString::Printf(
            TEXT("[%d] %s\n%.0fdBZ ET:%.1fkm\n%s%s"),
            Storm.TrackID,
            *UEnum::GetValueAsString(Storm.Type),
            Storm.MaxRefl_dBZ, Storm.EchoTop_km,
            Storm.bIsMesocyclone ? TEXT("MESO ") : TEXT(""),
            Storm.bTVS ? TEXT("TVS!") : TEXT(""));

        DrawDebugString(W, Storm.Position + FVector(0, 0, H_cm + 50000.f),
                        Label, nullptr, C, DrawDuration, false, TextScale);
    }
}

// ============================================================
//  COLUMN DIAGNOSTICS  — CAPE/CIN/SRH text per column
// ============================================================

void UWeatherDebugComponent::DrawColumnDiagnostics(UWorld* W, const FVector& CamPos)
{
    if (!W) return;
    const FWeatherSimConfig& Cfg = WeatherSys->GetConfiguration();
    const float DX_cm = Cfg.DX_km * 100000.f;
    const float Step = DX_cm * HorizontalSampleStep * 4; // sparse
    FVector Origin = WeatherSys->GetDomainOrigin();

    int32 NX = Cfg.GridNX / (HorizontalSampleStep * 4);
    int32 NY = Cfg.GridNY / (HorizontalSampleStep * 4);

    for (int32 iy = 0; iy < NY; ++iy)
        for (int32 ix = 0; ix < NX; ++ix)
        {
            FVector ColPos = Origin + FVector(ix * Step, iy * Step, 0.f);
            if (!IsWithinDebugRadius(ColPos, CamPos)) continue;

            FAtmosphericColumn Col;
            if (!WeatherSys->GetColumnDiagnostics(ColPos, Col)) continue;

            // Only show interesting columns
            if (Col.CAPE < 100.f && Col.SRH_0_3km < 50.f) continue;

            // CAPE/CIN bar (vertical line coloured by CAPE)
            float CapeH = FMath::Clamp(Col.CAPE / 50.f, 0.f, 5000.f) * 100.f;
            FColor CapeColor = (Col.CAPE > 2500.f) ? FColor::Red :
                (Col.CAPE > 1000.f) ? FColor::Orange :
                (Col.CAPE > 500.f) ? FColor::Yellow : FColor::Green;

            DrawDebugLine(W, ColPos, ColPos + FVector(0, 0, CapeH),
                          CapeColor, false, DrawDuration, 0, 200.f);

            // Text label
            FString Txt = FString::Printf(
                TEXT("CAPE:%.0f  CIN:%.0f\nSRH03:%.0f  SRH01:%.0f\nSHEAR:%.0f  SCP:%.1f\nSTP:%.1f  SHIP:%.1f\n%s"),
                Col.CAPE, Col.CIN,
                Col.SRH_0_3km, Col.SRH_0_1km,
                Col.BulkShear_0_6km, Col.SCP,
                Col.STP, Col.SHIP,
                *UEnum::GetValueAsString(Col.ClassifyPhenomenon()));

            DrawDebugString(W, ColPos + FVector(0, 0, CapeH + 10000.f),
                            Txt, nullptr, CapeColor, DrawDuration, false, TextScale * 0.8f);

            // SRH spiral (size proportional to SRH)
            if (Col.SRH_0_3km > 100.f)
            {
                float SpiralR = FMath::Sqrt(Col.SRH_0_3km) * 100.f;
                float Z_1km = 100000.f;
                DrawDebugCircle(W, ColPos + FVector(0, 0, Z_1km), SpiralR,
                                24, FColor::Magenta, false, DrawDuration, 0, 15.f,
                                FVector(0, 0, 1), FVector(1, 0, 0));
            }

            // LCL/LFC markers
            if (Col.LCL_m > 0.f)
            {
                FVector LCLPos = ColPos + FVector(0, 0, Col.LCL_m * 100.f);
                DrawDebugPoint(W, LCLPos, 20.f, FColor::Cyan, false, DrawDuration);
                DrawDebugString(W, LCLPos, TEXT("LCL"), nullptr, FColor::Cyan, DrawDuration);
            }
            if (Col.LFC_m > 0.f)
            {
                FVector LFCPos = ColPos + FVector(0, 0, Col.LFC_m * 100.f);
                DrawDebugPoint(W, LFCPos, 20.f, FColor::Yellow, false, DrawDuration);
                DrawDebugString(W, LFCPos, TEXT("LFC"), nullptr, FColor::Yellow, DrawDuration);
            }
        }
}

// ============================================================
//  RADAR BEAM
// ============================================================

void UWeatherDebugComponent::DrawRadarBeam(UWorld* W)
{
    if (!W) return;
    UWeatherRadar* Radar = WeatherSys->GetRadar();
    if (!Radar || !Radar->IsScanning()) return;

    FVector RadarPos = Radar->GetLocation();
    float   Az = Radar->GetCurrentAzimuth();
    float   El = Radar->GetCurrentElevation();
    float   MaxR_cm = 230.f * 100000.f;

    float Az_rad = FMath::DegreesToRadians(Az);
    float El_rad = FMath::DegreesToRadians(El);

    // Beam centreline
    FVector BeamDir(
        FMath::Sin(Az_rad) * FMath::Cos(El_rad),
        FMath::Cos(Az_rad) * FMath::Cos(El_rad),
        FMath::Sin(El_rad));

    DrawDebugDirectionalArrow(W, RadarPos, RadarPos + BeamDir * MaxR_cm,
                              5000.f, FColor::Cyan, false, DrawDuration, 0, 20.f);

    // Radar location marker
    DrawDebugSphere(W, RadarPos, 50000.f, 16, FColor::White, false, DrawDuration);
    DrawDebugString(W, RadarPos + FVector(0, 0, 60000.f),
                    FString::Printf(TEXT("RADAR  Az:%.0f°  El:%.1f°\nVn:%.0f m/s"),
                    Az, El, Radar->GetNyquistVelocity()),
                    nullptr, FColor::Cyan, DrawDuration, false, TextScale);

    // Scan rings at key ranges (50, 115, 230 km)
    for (float R_km : TArray<float>{ 50.f, 115.f, 230.f })
    {
        DrawDebugCircle(W, RadarPos, R_km * 100000.f, 64, FColor(0, 60, 60),
                        false, DrawDuration, 0, 20.f, FVector(0, 0, 1), FVector(1, 0, 0));
    }

    // Draw latest PPI sweep as colored points
    TArray<FRadarGate> Sweep = Radar->GetPPISweep(El);
    for (const auto& Gate : Sweep)
    {
        if (!Gate.bIsValid || Gate.Reflectivity_dBZ < 10.f) continue;
        FColor C = ReflectivityColor(Gate.Reflectivity_dBZ);
        DrawColoredPoint(W, Gate.WorldPosition, C, 6.f, DrawDuration);
    }
}

// ============================================================
//  SOUNDING  — vertical profile at camera location
// ============================================================

void UWeatherDebugComponent::DrawSounding(UWorld* W, const FVector& CamPos)
{
    if (!W) return;
    const FWeatherSimConfig& Cfg = WeatherSys->GetConfiguration();

    // Draw at a fixed offset from camera
    FVector Base = FVector(CamPos.X, CamPos.Y + 100000.f, 0.f);
    float ScaleH_cmPerK = 10000.f; // cm per degree temperature
    float ScaleV_cmPerM = 5.f;     // cm per metre altitude

    const float MaxAlt = Cfg.GridNZ * Cfg.DZ_m;

    FAtmosphericCell PrevCell;
    FVector PrevPosT, PrevPosTd;
    bool bFirst = true;

    for (int32 z = 0; z < Cfg.GridNZ; z += 2)
    {
        float Z_m = Cfg.DZ_m * z;
        FVector QueryPos = FVector(CamPos.X, CamPos.Y, Z_m * 100.f);

        FAtmosphericCell Cell;
        if (!WeatherSys->GetAtmosphereAtLocation(QueryPos, Cell)) continue;

        float T = Cell.Temperature - 273.15f;     // °C
        float RH = Cell.RelativeHumidity;
        float Td = T - (1.f - RH) * 17.f;          // dew point approx (°C)

        FVector PosT(Base.X + T * ScaleH_cmPerK, Base.Y, Z_m * ScaleV_cmPerM);
        FVector PosTd(Base.X + Td * ScaleH_cmPerK, Base.Y, Z_m * ScaleV_cmPerM);

        if (!bFirst)
        {
            // Temperature (red)
            DrawDebugLine(W, PrevPosT, PosT, FColor::Red, false, DrawDuration, 0, 15.f);
            // Dew point (green)
            DrawDebugLine(W, PrevPosTd, PosTd, FColor::Green, false, DrawDuration, 0, 15.f);
        }

        DrawDebugPoint(W, PosT, 6.f, FColor::Red, false, DrawDuration);
        DrawDebugPoint(W, PosTd, 6.f, FColor::Green, false, DrawDuration);

        PrevPosT = PosT; PrevPosTd = PosTd;
        bFirst = false;

        // Wind barb (every 4 levels)
        if (z % 4 == 0)
        {
            FVector WindBase(Base.X + 10000.f, Base.Y, Z_m * ScaleV_cmPerM);
            float WSpeed = FMath::Sqrt(Cell.U * Cell.U + Cell.V * Cell.V);
            float WDir = FMath::RadiansToDegrees(FMath::Atan2(Cell.U, Cell.V));
            float WDir_r = FMath::DegreesToRadians(WDir);
            FVector WVec = FVector(FMath::Sin(WDir_r), FMath::Cos(WDir_r), 0.f)
                * WSpeed * WindArrowScale * 50.f;
            DrawDebugDirectionalArrow(W, WindBase, WindBase + WVec * 50000.f,
                           1000.f, FColor::White, false, DrawDuration, 0, 10.f);

            if (bShowNumericalValues)
                DrawDebugString(W, WindBase + FVector(20000.f, 0, 0),
                                FString::Printf(TEXT("%.0f°/%.0fm/s"), WDir, WSpeed),
                                nullptr, FColor::White, DrawDuration);
        }

        // Altitude gridlines
        if (z % 10 == 0)
        {
            FVector LineL(Base.X - 20000.f, Base.Y, Z_m * ScaleV_cmPerM);
            FVector LineR(Base.X + 50000.f, Base.Y, Z_m * ScaleV_cmPerM);
            DrawDebugLine(W, LineL, LineR, FColor(40, 40, 40), false, DrawDuration, 0, 5.f);
            DrawDebugString(W, LineL,
                            FString::Printf(TEXT("%.0fm"), Z_m),
                            nullptr, FColor(120, 120, 120), DrawDuration);
        }
    }

    // 0°C isotherm line
    FVector ZeroC(Base.X + 0.f * ScaleH_cmPerK, Base.Y, 0.f);
    DrawDebugLine(W, ZeroC, ZeroC + FVector(0, 0, MaxAlt * ScaleV_cmPerM),
                  FColor::Cyan, false, DrawDuration, 0, 20.f);
    DrawDebugString(W, ZeroC, TEXT("0°C"), nullptr, FColor::Cyan, DrawDuration);

    // Frame label
    DrawDebugString(W, Base + FVector(0, 0, -10000.f),
                    TEXT("SOUNDING  — Red=T  Green=Td  White=Wind"),
                    nullptr, FColor::White, DrawDuration, false, TextScale);
}

// ============================================================
//  STATUS PRINT
// ============================================================

void UWeatherDebugComponent::PrintSimulationStatus() const
{
    if (!WeatherSys || !GEngine) return;

    float SimT = WeatherSys->GetSimulationTime();
    int32 Mesocy = WeatherSys->GetMesocycloneCount();
    TArray<FStormCell> Storms = WeatherSys->GetAllDetectedStorms();

    FAtmosphericColumn Col;
    WeatherSys->GetColumnDiagnostics(GetOwner()->GetActorLocation(), Col);

    FString Msg = FString::Printf(
        TEXT("=== AEROS WEATHER SIM ===\n"
        "Sim Time:      %.1f s\n"
        "Storms:        %d  Mesocyclones: %d\n"
        "CAPE:          %.0f J/kg\n"
        "CIN:           %.0f J/kg\n"
        "SRH 0-3km:     %.0f m²/s²\n"
        "SRH 0-1km:     %.0f m²/s²\n"
        "Shear 0-6km:   %.0f m/s\n"
        "SCP:           %.2f\n"
        "STP:           %.2f\n"
        "SHIP:          %.2f\n"
        "Phenomenon:    %s\n"
        "Precip Rate:   %.1f mm/hr"),
        SimT, Storms.Num(), Mesocy,
        Col.CAPE, Col.CIN,
        Col.SRH_0_3km, Col.SRH_0_1km,
        Col.BulkShear_0_6km,
        Col.SCP, Col.STP, Col.SHIP,
        *UEnum::GetValueAsString(Col.ClassifyPhenomenon()),
        Col.PrecipRate);

    GEngine->AddOnScreenDebugMessage(42, 5.f, FColor::Cyan, Msg);
}

// ============================================================
//  CONTROL API
// ============================================================

void UWeatherDebugComponent::ToggleAllLayers(bool bOn)
{
    Wind.bEnabled = bOn;
    Reflectivity.bEnabled = bOn;
    Temperature.bEnabled = bOn;
    Vorticity.bEnabled = bOn;
    CloudLayer.bEnabled = bOn;
    StormCells.bEnabled = bOn;
    ColumnDiagnostics.bEnabled = bOn;
    RadarBeam.bEnabled = bOn;
    Sounding.bEnabled = bOn;
}

void UWeatherDebugComponent::ToggleLayer(const FString& LayerName, bool bOn)
{
    if (LayerName == TEXT("Wind"))         Wind.bEnabled = bOn;
    else if (LayerName == TEXT("Reflectivity")) Reflectivity.bEnabled = bOn;
    else if (LayerName == TEXT("Temperature"))  Temperature.bEnabled = bOn;
    else if (LayerName == TEXT("Vorticity"))    Vorticity.bEnabled = bOn;
    else if (LayerName == TEXT("Cloud"))        CloudLayer.bEnabled = bOn;
    else if (LayerName == TEXT("Storms"))       StormCells.bEnabled = bOn;
    else if (LayerName == TEXT("Column"))       ColumnDiagnostics.bEnabled = bOn;
    else if (LayerName == TEXT("Radar"))        RadarBeam.bEnabled = bOn;
    else if (LayerName == TEXT("Sounding"))     Sounding.bEnabled = bOn;
}

// ============================================================
//  COLOUR SCALES
// ============================================================

FColor UWeatherDebugComponent::ReflectivityColor(float dBZ)
{
    // NWS standard reflectivity colour table
    if (dBZ < 10.f) return FColor(100, 100, 100);   // light grey
    if (dBZ < 20.f) return FColor(20, 150, 230);   // light blue
    if (dBZ < 30.f) return FColor(20, 230, 20);   // light green
    if (dBZ < 40.f) return FColor(250, 230, 0);   // yellow
    if (dBZ < 45.f) return FColor(250, 130, 0);   // orange
    if (dBZ < 50.f) return FColor(230, 10, 10);   // red
    if (dBZ < 55.f) return FColor(220, 0, 200);   // magenta
    if (dBZ < 60.f) return FColor(255, 255, 255);   // white
    return FColor(255, 100, 200);                    // bright pink (>60 dBZ, hail)
}

FColor UWeatherDebugComponent::TemperatureColor(float TempK, float MinK, float MaxK)
{
    float t = FMath::Clamp((TempK - MinK) / (MaxK - MinK), 0.f, 1.f);
    // Cold=blue → warm=red  (via cyan/green/yellow)
    if (t < 0.25f)
    {
        float f = t / 0.25f;
        return FColor((uint8)(0), (uint8)(f * 255), 255);
    }
    if (t < 0.5f)
    {
        float f = (t - 0.25f) / 0.25f;
        return FColor(0, 255, (uint8)((1.f - f) * 255));
    }
    if (t < 0.75f)
    {
        float f = (t - 0.5f) / 0.25f;
        return FColor((uint8)(f * 255), 255, 0);
    }
    {
        float f = (t - 0.75f) / 0.25f;
        return FColor(255, (uint8)((1.f - f) * 255), 0);
    }
}

FColor UWeatherDebugComponent::VelocityColor(float V_ms, float Nyquist)
{
    // NWS velocity: green (toward) → black (zero) → red (away)
    float t = FMath::Clamp(V_ms / Nyquist, -1.f, 1.f);
    if (t < 0.f)
    {
        float f = -t;
        return FColor(0, (uint8)(f * 255), 0); // green
    }
    return FColor((uint8)(t * 255), 0, 0); // red
}

FColor UWeatherDebugComponent::VorticityColor(float VortZ)
{
    // Cyclonic (positive NH) = red, anticyclonic = blue
    float v = FMath::Clamp(FMath::Abs(VortZ) / 0.05f, 0.f, 1.f);
    if (VortZ > 0.f) return FColor((uint8)(v * 255), 0, 0);  // red
    return FColor(0, 0, (uint8)(v * 255));                    // blue
}

FColor UWeatherDebugComponent::PhenomenonColor(EWeatherPhenomenon P)
{
    switch (P)
    {
        case EWeatherPhenomenon::Clear:          return FColor::White;
        case EWeatherPhenomenon::PartlyCloudy:   return FColor(200, 200, 200);
        case EWeatherPhenomenon::Overcast:       return FColor(128, 128, 128);
        case EWeatherPhenomenon::Drizzle:        return FColor(80, 160, 255);
        case EWeatherPhenomenon::Rain:           return FColor(20, 100, 255);
        case EWeatherPhenomenon::HeavyRain:      return FColor(0, 0, 200);
        case EWeatherPhenomenon::Thunderstorm:   return FColor(255, 200, 0);
        case EWeatherPhenomenon::Supercell:      return FColor(255, 60, 0);
        case EWeatherPhenomenon::Tornado:        return FColor(255, 0, 0);
        case EWeatherPhenomenon::Blizzard:       return FColor(200, 230, 255);
        case EWeatherPhenomenon::Hail:           return FColor(200, 0, 255);
        case EWeatherPhenomenon::TropicalCyclone:return FColor(255, 0, 128);
        case EWeatherPhenomenon::Fog:            return FColor(180, 180, 180);
        default: return FColor::White;
    }
}

// ============================================================
//  HELPERS
// ============================================================

void UWeatherDebugComponent::DrawColoredPoint(UWorld* W, const FVector& Pos,
                                              const FColor& C, float Size, float Dur) const
{
    DrawDebugPoint(W, Pos, Size, C, false, Dur);
}

void UWeatherDebugComponent::DrawArrow(UWorld* W, const FVector& Start,
                                       const FVector& Dir, float Magnitude,
                                       const FColor& C, float Dur) const
{
    float ArrowLen = Magnitude * WindArrowScale * 100.f;
    ArrowLen = FMath::Clamp(ArrowLen, 500.f, 500000.f);
    DrawDebugDirectionalArrow(W, Start, Start + Dir * ArrowLen,
                              ArrowLen * 0.2f, C, false, Dur, 0, 15.f);
}

FColor UWeatherDebugComponent::ValueToColor(float Val, float Min, float Max,
                                            const FColor& CMin, const FColor& CMax) const
{
    float t = FMath::Clamp((Val - Min) / (Max - Min), 0.f, 1.f);
    return FColor(
        (uint8)FMath::Lerp((float)CMin.R, (float)CMax.R, t),
        (uint8)FMath::Lerp((float)CMin.G, (float)CMax.G, t),
        (uint8)FMath::Lerp((float)CMin.B, (float)CMax.B, t));
}

bool UWeatherDebugComponent::IsWithinDebugRadius(const FVector& Pos,
                                                 const FVector& CamPos) const
{
    float R = DebugRadius_km * 100000.f;
    return FVector::Dist2D(Pos, CamPos) <= R;
}

FVector UWeatherDebugComponent::GetCameraPosition() const
{
    if (UWorld* W = GetWorld())
    {
        if (APlayerController* PC = W->GetFirstPlayerController())
        {
            FVector Loc; FRotator Rot;
            PC->GetPlayerViewPoint(Loc, Rot);
            return Loc;
        }
    }
    return FVector::ZeroVector;
}