// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WeatherTypes.h"
#include "WeatherDebugComponent.generated.h"

class UWeatherSubsystem;

// ============================================================
//  WEATHER DEBUG COMPONENT
//  Attach to any actor. Renders the simulation state using
//  DrawDebugHelpers — visible in editor and in-game.
//  Toggle layers on/off at runtime via Blueprint or console.
// ============================================================

UENUM(BlueprintType)
enum class EDebugColorScheme : uint8
{
    Reflectivity,        // Green→Yellow→Red (NWS standard)
    Velocity,            // Blue (toward) → Red (away)
    Temperature,         // Cold blue → hot red
    Humidity,            // Dry tan → wet blue
    TKE,                 // Black → white
    CloudFraction,       // Clear → white
    Vorticity,           // Cyclonic red, anticyclonic blue
    Phenomenon,          // Color per EWeatherPhenomenon
};

USTRUCT(BlueprintType)
struct FWeatherDebugLayer
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Opacity = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float PointSize = 8.f;
};

UCLASS(ClassGroup = (Weather), meta = (BlueprintSpawnableComponent))
class AEROS_API UWeatherDebugComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UWeatherDebugComponent();

    virtual void BeginPlay()        override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

    // ── Layers ───────────────────────────────────────────
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer Wind;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer Reflectivity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer Temperature;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer Vorticity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer CloudLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer StormCells;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer ColumnDiagnostics; // CAPE/SRH text

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer RadarBeam;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Layers")
    FWeatherDebugLayer Sounding; // vertical T/Td profile

    // ── Grid sampling settings ───────────────────────────
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Grid")
    int32 HorizontalSampleStep = 8; // sample every N cells (higher = fewer points)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Grid")
    int32 VerticalSampleStep = 4;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Grid")
    float DebugRadius_km = 50.f; // only draw within this radius of camera

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Grid")
    float AltitudeSlice_m = -1.f; // -1 = all levels; >=0 = specific altitude

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Grid")
    bool  bShowOnlyHighestRefl = true; // reflectivity: show composite not all levels

    // ── Display settings ─────────────────────────────────
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Display")
    EDebugColorScheme ColorScheme = EDebugColorScheme::Reflectivity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Display")
    float WindArrowScale = 0.0001f; // m/s → cm (Unreal)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Display")
    float TextScale = 1.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Display")
    bool  bShowNumericalValues = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Display")
    float DrawDuration = 0.05f; // seconds (<=0 = 1 frame)

    // ── Console variable toggles ──────────────────────────
    UFUNCTION(BlueprintCallable, Category = "Debug|Control")
    void ToggleAllLayers(bool bOn);

    UFUNCTION(BlueprintCallable, Category = "Debug|Control")
    void ToggleLayer(const FString& LayerName, bool bOn);

    UFUNCTION(BlueprintCallable, Category = "Debug|Control")
    void SetColorScheme(EDebugColorScheme Scheme) { ColorScheme = Scheme; }

    UFUNCTION(BlueprintCallable, Category = "Debug|Control")
    void SetAltitudeSlice(float Altitude_m) { AltitudeSlice_m = Altitude_m; }

    UFUNCTION(BlueprintCallable, Category = "Debug|Control")
    void PrintSimulationStatus() const;

    // ── Colour utilities (static, usable anywhere) ────────
    UFUNCTION(BlueprintCallable, Category = "Debug|Color")
    static FColor ReflectivityColor(float dBZ);

    UFUNCTION(BlueprintCallable, Category = "Debug|Color")
    static FColor TemperatureColor(float TempK, float MinK = 250.f, float MaxK = 310.f);

    UFUNCTION(BlueprintCallable, Category = "Debug|Color")
    static FColor VelocityColor(float V_ms, float Nyquist = 25.f);

    UFUNCTION(BlueprintCallable, Category = "Debug|Color")
    static FColor VorticityColor(float VortZ);

    UFUNCTION(BlueprintCallable, Category = "Debug|Color")
    static FColor PhenomenonColor(EWeatherPhenomenon P);

private:
    void DrawWindLayer(UWorld* W, const FVector& CamPos);
    void DrawReflectivity(UWorld* W, const FVector& CamPos);
    void DrawTemperatureLayer(UWorld* W, const FVector& CamPos);
    void DrawVorticityLayer(UWorld* W, const FVector& CamPos);
    void DrawCloudLayer(UWorld* W, const FVector& CamPos);
    void DrawStormCells(UWorld* W);
    void DrawColumnDiagnostics(UWorld* W, const FVector& CamPos);
    void DrawRadarBeam(UWorld* W);
    void DrawSounding(UWorld* W, const FVector& CamPos);

    // Helpers
    void DrawColoredPoint(UWorld* W, const FVector& Pos, const FColor& C,
                          float Size, float Duration) const;
    void DrawArrow(UWorld* W, const FVector& Start, const FVector& Dir,
                   float Magnitude, const FColor& C, float Duration) const;
    FColor ValueToColor(float Val, float Min, float Max,
                        const FColor& CMin, const FColor& CMax) const;
    bool IsWithinDebugRadius(const FVector& Pos, const FVector& CamPos) const;
    FVector GetCameraPosition() const;

    UPROPERTY()
    UWeatherSubsystem* WeatherSys = nullptr;

    // Frame rate limiter for debug drawing
    float DrawAccum = 0.f;
    float DrawInterval = 0.05f; // max 20fps debug drawing
};