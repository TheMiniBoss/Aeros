// Copyright Aeros. All Rights Reserved.
//
// Canvas HUD for the demo (no UMG asset required).
//   Simple mode: plain-language weather card, "what is happening" explanation, risk gauges,
//                on-screen markers for storms / tornadoes / hurricanes / volcanoes / quakes,
//                and an event feed.
//   Expert mode: the full numbers (sounding, severe indices, hazards).
// Both modes show the live Doppler radar, warnings and the guided-tour caption.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "AerosTypes.h"
#include "AerosDemoHUD.generated.h"

class UAerosWeatherSubsystem;
class AAerosTornado;
class AAerosVolcano;

UENUM()
enum class EAerosHudMode : uint8
{
	Simple,
	Expert,
	RadarOnly,
	Hidden,
	Help,
	Count UMETA(Hidden)
};

UCLASS()
class AEROSWEATHERDEMO_API AAerosDemoHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;

	void CycleMode() { Mode = static_cast<EAerosHudMode>((uint8(Mode) + 1) % uint8(EAerosHudMode::Count)); }
	EAerosHudMode GetMode() const { return Mode; }

	/** Adds a line to the event feed (bottom left). */
	void PushEvent(const FString& Text, const FLinearColor& Color);

private:
	struct FFeedEntry
	{
		FString Text;
		FLinearColor Color;
		float Time = 0.f;
	};

	void DrawPanel(float X, float Y, float W, float H);
	float Line(float X, float Y, const FString& Text, const FLinearColor& Color = FLinearColor::White, float Scale = 1.f);
	float Paragraph(float X, float Y, float Width, const FString& Text, const FLinearColor& Color);
	float Gauge(float X, float Y, float Width, const FString& Label, float Value);
	void DrawWindRose(float CX, float CY, float R, const FVector& Wind);

	void DrawSimple(UAerosWeatherSubsystem* Sub, const FAerosWeatherSample& S);
	void DrawExpert(UAerosWeatherSubsystem* Sub, const FAerosWeatherSample& S);
	void DrawRadar(UAerosWeatherSubsystem* Sub, float X, float Y, float Size);
	void DrawWorldMarkers(UAerosWeatherSubsystem* Sub);
	void DrawMarker(const FVector& World, const FString& Label, const FLinearColor& Color, float Size, bool bArrowWhenOffscreen);
	void DrawWarning(UAerosWeatherSubsystem* Sub, const FAerosWeatherSample& S);
	void DrawFeed();
	void DrawCaption();
	void DrawHelp(float X, float Y);

	FString Explain(UAerosWeatherSubsystem* Sub, const FAerosWeatherSample& S) const;

	UFUNCTION() void HandleStormFormed(const FAerosStormData& Storm);
	UFUNCTION() void HandleTornado(AAerosTornado* Tornado);
	UFUNCTION() void HandleCyclone(const FAerosCycloneData& Cyclone);
	UFUNCTION() void HandleEarthquake(const FAerosEarthquakeData& Quake);
	UFUNCTION() void HandleTsunami(FVector Source, float Magnitude);
	UFUNCTION() void HandleEruption(AAerosVolcano* Volcano, int32 VEI);
	UFUNCTION() void HandlePhenomenon(EAerosPhenomenon OldPhenomenon, EAerosPhenomenon NewPhenomenon);

	EAerosHudMode Mode = EAerosHudMode::Simple;
	TArray<FFeedEntry> Feed;
	TArray<FBox2D> LabelRects; // marker labels drawn this frame (overlap avoidance)
	bool bBound = false;
};
