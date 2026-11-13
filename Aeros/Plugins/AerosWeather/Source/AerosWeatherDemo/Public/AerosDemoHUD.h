// Copyright Aeros. All Rights Reserved.
//
// Canvas HUD for the demo: clock + season, local weather, sounding indices, a live Doppler
// radar, storm list, warnings, lightning flash and key help. No UMG asset required.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "AerosDemoHUD.generated.h"

UCLASS()
class AEROSWEATHERDEMO_API AAerosDemoHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

	/** 0 = full, 1 = radar only, 2 = hidden, 3 = help */
	void CycleMode() { Mode = (Mode + 1) % 4; }

private:
	void DrawPanel(float X, float Y, float W, float H);
	float Line(float X, float Y, const FString& Text, const FLinearColor& Color = FLinearColor::White, float Scale = 1.f);
	void DrawRadar(class UAerosWeatherSubsystem* Sub, float X, float Y, float Size);
	void DrawHelp(float X, float Y);

	int32 Mode = 0;
};
