// Copyright Aeros. All Rights Reserved.
//
// Debug overlay shared by both demo HUDs: what updates the weather on screen and when the next update
// comes (aeros.DebugUpdates 1, or -AerosDebugUpdates). Header-only, demo module.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "AerosWeatherSubsystem.h"
#include "AerosCloudManager.h"

namespace AerosDebugUpdatesInternal
{
	inline bool IsOn()
	{
		static TAutoConsoleVariable<int32> CVar(TEXT("aeros.DebugUpdates"), FParse::Param(FCommandLine::Get(), TEXT("AerosDebugUpdates")) ? 1 : 0,
			TEXT("1: show when the weather on screen is next updated (snapshot, storm tracking, 3D cloud grid)"));
		return CVar.GetValueOnGameThread() != 0;
	}

	/** Draws the overlay at (X, Y) going up, returns nothing. Real seconds = simulated / time scale. */
	inline void Draw(AHUD* Hud, UCanvas* Canvas, UAerosWeatherSubsystem* Sub, float X, float Y)
	{
		if (!Hud || !Canvas || !Sub || !IsOn()) return;
		const UAerosWeatherSubsystem::FUpdateTimes T = Sub->GetUpdateTimes();
		const float Scale = FMath::Max(T.TimeScale, 0.001f);
		auto Real = [Scale](double Sim) { return float(FMath::Max(Sim, 0.0) / Scale); };
		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("MISES \u00C0 JOUR (temps x%.0f)"), T.TimeScale));
		Lines.Add(FString::Printf(TEXT("Affichage entre deux \u00E9tats : t = %.0f s -> t+1 = %.0f s, %.0f %% du chemin"), T.FromTime, T.ToTime, T.Alpha * 100.f));
		Lines.Add(FString::Printf(TEXT("Prochain d\u00E9part vers t+2 dans %.0f s simul\u00E9es (%.1f s r\u00E9elles), simulation \u00E0 %.0f s (avance %.0f s)"),
			FMath::Max(T.ToTime - T.DisplayTime, 0.0), Real(T.ToTime - T.DisplayTime), T.SimTime, T.SimTime - T.DisplayTime));
		Lines.Add(FString::Printf(TEXT("Orages suivis \u00E0 chaque \u00E9tat (%d), positions interpol\u00E9es t -> t+1"), T.NumStorms));
		if (const UAerosCloudManager* Clouds = Sub->GetCloudManager())
		{
			const float Swap = Clouds->GetSecondsSinceVolumeSwap();
			Lines.Add(Swap < 0.f ? FString(TEXT("Grille 3D des nuages : pas encore construite"))
				: FString::Printf(TEXT("Grille 3D des nuages : \u00E9chang\u00E9e il y a %.1f s (fondu 1 s, %d \u00E9changes)"), Swap, Clouds->GetCloudVolumeSwaps()));
		}
		const bool bJustTracked = T.Alpha < 0.05f;
		UFont* Font = GEngine->GetSmallFont();
		const float LineH = 16.f;
		Hud->DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), X - 6.f, Y - LineH * Lines.Num() - 6.f, 560.f, LineH * Lines.Num() + 10.f);
		for (int32 K = 0; K < Lines.Num(); ++K)
		{
			const FLinearColor C = K == 0 ? FLinearColor(1.f, 0.78f, 0.35f) : (K == 1 && bJustTracked ? FLinearColor(1.f, 0.4f, 0.35f) : FLinearColor(0.9f, 0.92f, 0.95f));
			Hud->DrawText(Lines[K], C, X, Y - LineH * (Lines.Num() - K), Font, 1.f);
		}
	}
}
