// Copyright Aeros. All Rights Reserved.
//
// Interface of the playable demo (storm chaser): start menu, briefing, driving HUD (clock, score, speed,
// vehicle state, radar, compass, markers, alerts, viewfinder, photo cards), pause, destruction and the
// end-of-day summary. The technical demo keeps AAerosDemoHUD.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "AerosChaseHUD.generated.h"

class AAerosChasePawn;
class UAerosWeatherSubsystem;

UCLASS()
class AEROSWEATHERDEMO_API AAerosChaseHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawMenu(AAerosChasePawn* P);
	void DrawBriefing(AAerosChasePawn* P);
	void DrawDriving(AAerosChasePawn* P, UAerosWeatherSubsystem* Sub);
	void DrawSummary(AAerosChasePawn* P);
	void DrawRadar(AAerosChasePawn* P, float X, float Y, float Size);
	void DrawCompass(AAerosChasePawn* P, UAerosWeatherSubsystem* Sub, float CX, float Y, float W);
	void DrawWorldMarkers(AAerosChasePawn* P, UAerosWeatherSubsystem* Sub);
	void DrawViewfinder(AAerosChasePawn* P);

	void Panel(float X, float Y, float W, float H, float Alpha = 0.7f);
	void Text(const FString& S, float X, float Y, const FLinearColor& C, float Scale = 1.f, bool bCentre = false, class UFont* Font = nullptr);
	float Wrapped(const FString& S, float X, float Y, float W, const FLinearColor& C, float Scale = 1.f);
	void Bar(float X, float Y, float W, float H, float Value, const FLinearColor& C);
	void Frame(float X, float Y, float W, float H, const FLinearColor& C, float Corner);
};
