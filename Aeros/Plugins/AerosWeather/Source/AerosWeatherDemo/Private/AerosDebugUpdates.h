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
#include "AerosTornado.h"

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
		Lines.Add(T.bWaiting ? FString(TEXT("ATTENTE : la simulation est en retard, l'affichage s'arr\u00EAte (sans sauter)"))
			: FString::Printf(TEXT("Affichage %.0f s derri\u00E8re la simulation (vise 2 pas)"), T.SimTime - T.DisplayTime));

		// Jump detector: biggest move from one frame to the next of the nearest storm's tower and of the
		// nearest tornado, over the last 5 real seconds (logged when it passes 150 m)
		{
			static FVector PrevStorm = FVector::ZeroVector, PrevTornado = FVector::ZeroVector;
			static int32 PrevStormId = -1;
			static float WorstStormM = 0.f, WorstTornadoM = 0.f, Window = 0.f;
			static double LastTime = 0.0;
			const double Now = FPlatformTime::Seconds();
			const float Dt = LastTime > 0.0 ? float(Now - LastTime) : 0.f;
			LastTime = Now;
			Window += Dt;
			if (Window > 5.f) { Window = 0.f; WorstStormM = 0.f; WorstTornadoM = 0.f; }
			const FVector Viewer = Sub->GetViewerLocation();
			FAerosStormData Near;
			if (Sub->GetNearestStorm(Viewer, Near, 150.f))
			{
				if (Near.Id == PrevStormId && Dt < 0.5f)
				{
					// A jump: more than 2.5x what the storm's speed covers in this frame (and 150 m)
					const float M = float(FVector::Dist2D(Near.UpdraftLocation, PrevStorm)) * 0.01f;
					const float Expected = float(Near.Velocity.Size2D()) * T.TimeScale * Dt;
					const float Excess = FMath::Max(M - 2.5f * Expected, 0.f);
					WorstStormM = FMath::Max(WorstStormM, Excess);
					if (Excess > 150.f) UE_LOG(LogTemp, Display, TEXT("Aeros jump: storm %d tower moved %.0f m in one frame (expected %.0f m; display %.1f s, alpha %.2f, waiting %d)"), Near.Id, M, Expected, T.DisplayTime, T.Alpha, int32(T.bWaiting));
				}
				PrevStorm = Near.UpdraftLocation;
				PrevStormId = Near.Id;
			}
			const TArray<AAerosTornado*> Tornadoes = Sub->GetActiveTornadoes();
			const AAerosTornado* NearT = nullptr;
			for (const AAerosTornado* Tw : Tornadoes) if (Tw && (!NearT || FVector::DistSquared2D(Tw->GetActorLocation(), Viewer) < FVector::DistSquared2D(NearT->GetActorLocation(), Viewer))) NearT = Tw;
			static const AAerosTornado* PrevT = nullptr;
			if (NearT)
			{
				// Same tornado only; a jump is more than 2.5x its storm-driven motion this frame (and 150 m)
				const float M = (NearT != PrevT || Dt >= 0.5f) ? 0.f : float(FVector::Dist2D(NearT->GetActorLocation(), PrevTornado)) * 0.01f;
				const float Expected = float(NearT->TrackVelocity.Size2D()) * T.TimeScale * Dt;
				const float Excess = FMath::Max(M - 2.5f * Expected, 0.f);
				WorstTornadoM = FMath::Max(WorstTornadoM, Excess);
				if (Excess > 150.f) UE_LOG(LogTemp, Display, TEXT("Aeros jump: tornado moved %.0f m in one frame (expected %.0f m)"), M, Expected);
				PrevTornado = NearT->GetActorLocation();
			}
			PrevT = NearT;
			Lines.Add(FString::Printf(TEXT("Saut image \u00E0 image au-del\u00E0 du mouvement normal (5 s) : tour %.0f m, tornade %.0f m"), WorstStormM, WorstTornadoM));
		}
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
			const FLinearColor C = K == 0 ? FLinearColor(1.f, 0.78f, 0.35f) : ((K == 1 && bJustTracked) || (K == 4 && T.bWaiting) ? FLinearColor(1.f, 0.4f, 0.35f) : FLinearColor(0.9f, 0.92f, 0.95f));
			Hud->DrawText(Lines[K], C, X, Y - LineH * (Lines.Num() - K), Font, 1.f);
		}
	}
}
