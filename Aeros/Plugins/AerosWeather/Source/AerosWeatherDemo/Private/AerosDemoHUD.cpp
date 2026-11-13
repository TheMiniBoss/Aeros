// Copyright Aeros. All Rights Reserved.

#include "AerosDemoHUD.h"
#include "AerosDemoPawn.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherLibrary.h"
#include "AerosWeatherRadar.h"
#include "AerosCloudManager.h"
#include "AerosTornado.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"

namespace
{
	const FLinearColor Dim(0.65f, 0.7f, 0.75f);
	const FLinearColor Accent(0.4f, 0.8f, 1.f);
	const FLinearColor Warn(1.f, 0.75f, 0.2f);
	const FLinearColor Danger(1.f, 0.25f, 0.25f);

	FString Compass(float Deg)
	{
		static const TCHAR* Names[] = { TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"), TEXT("S"), TEXT("SW"), TEXT("W"), TEXT("NW") };
		return Names[int32(FMath::Fmod(Deg + 22.5f + 360.f, 360.f) / 45.f) % 8];
	}

	/** Bearing (deg from north, clockwise) from A to B in Unreal world space (-Y = north). */
	float Bearing(const FVector& A, const FVector& B)
	{
		const FVector D = B - A;
		return FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(float(D.X), float(-D.Y))) + 360.f, 360.f);
	}

	/** Wind "from" direction for a world wind vector. */
	float WindFrom(const FVector& W)
	{
		return FMath::Fmod(Bearing(FVector::ZeroVector, W) + 180.f, 360.f);
	}
}

void AAerosDemoHUD::DrawPanel(float X, float Y, float W, float H)
{
	DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.62f), X, Y, W, H);
}

float AAerosDemoHUD::Line(float X, float Y, const FString& Text, const FLinearColor& Color, float Scale)
{
	DrawText(Text, Color, X, Y, GEngine->GetSmallFont(), Scale);
	return Y + 15.f * Scale;
}

void AAerosDemoHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas) return;
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;

	// Lightning flash over everything
	if (Sub && Sub->GetCloudManager())
	{
		const float Flash = Sub->GetCloudManager()->GetLightningFlash();
		if (Flash > 0.01f) DrawRect(FLinearColor(0.9f, 0.92f, 1.f, Flash * 0.35f), 0, 0, SW, SH);
	}

	if (!Sub || !Sub->IsSimulationRunning())
	{
		Line(20, 20, TEXT("Aeros: simulation not running (add an Aeros Weather Controller or use AerosDemoGameMode)."), Warn);
		return;
	}
	if (Mode == 2) return;
	if (Mode == 3) { DrawHelp(20, 20); return; }

	const AAerosDemoPawn* Pawn = Cast<AAerosDemoPawn>(GetOwningPawn());
	const FVector Viewer = Sub->GetViewerLocation();
	const FAerosWeatherSample S = Sub->GetWeatherAtLocation(Viewer);
	const FAerosDateTime T = Sub->GetDateTime();
	const FAerosSimStats Stats = Sub->GetStats();

	const float RadarSize = FMath::Min(SH * 0.42f, 360.f);
	DrawRadar(Sub, SW - RadarSize - 20.f, 20.f, RadarSize);
	if (Mode == 1) return;

	// ---- Clock / simulation -------------------------------------------------------------
	float X = 20.f, Y = 20.f;
	DrawPanel(X - 8, Y - 6, 330, 118);
	Y = Line(X, Y, TEXT("AEROS WEATHER"), Accent, 1.2f);
	const int32 HH = int32(T.Hour), MM = int32((T.Hour - HH) * 60.f);
	Y = Line(X, Y, FString::Printf(TEXT("Day %d (%02d/%02d)  %02d:%02d   %s"), T.DayOfYear, T.DayOfMonth, T.Month, HH, MM, *UAerosWeatherLibrary::SeasonToText(T.Season).ToString()));
	Y = Line(X, Y, FString::Printf(TEXT("Sun elevation %.0f deg, azimuth %.0f deg"), T.SunElevation, T.SunAzimuth), Dim);
	Y = Line(X, Y, FString::Printf(TEXT("Scenario: %s   time x%.0f%s"), *UAerosWeatherLibrary::ScenarioToText(Sub->GetScenario()).ToString(), Sub->GetTimeScale(), Sub->IsPaused() ? TEXT("  [PAUSED]") : TEXT("")));
	Y = Line(X, Y, FString::Printf(TEXT("Grid %d cells  step %.1f ms  lag %.0f s"), Stats.NumCells, Stats.StepMs, Stats.LagSeconds), Dim);
	Y = Line(X, Y, FString::Printf(TEXT("Storms %d  cyclones %d  tornadoes %d"), Stats.NumStorms, Stats.NumCyclones, Sub->GetActiveTornadoes().Num()), Dim);

	// ---- Local weather ----------------------------------------------------------------------
	Y += 14.f;
	DrawPanel(X - 8, Y - 6, 330, 172);
	const FLinearColor PhCol = UAerosWeatherLibrary::IsSevere(S) ? Danger : Accent;
	Y = Line(X, Y, UAerosWeatherLibrary::PhenomenonToText(S.Phenomenon).ToString().ToUpper(), PhCol, 1.2f);
	Y = Line(X, Y, FString::Printf(TEXT("Temp %.1f C   Dew point %.1f C   RH %.0f%%"), S.Temperature, S.DewPoint, S.RelativeHumidity * 100.f));
	Y = Line(X, Y, FString::Printf(TEXT("Pressure %.1f hPa   Ground %.1f C"), S.Pressure, S.GroundTemperature));
	Y = Line(X, Y, FString::Printf(TEXT("Wind %.0f km/h from %s (%.0f deg)"), S.WindSpeed * 3.6f, *Compass(WindFrom(S.Wind)), WindFrom(S.Wind)));
	Y = Line(X, Y, FString::Printf(TEXT("Clouds %.0f%%  base %.0f m  top %.0f m"), S.CloudCover * 100.f, S.CloudBase, S.CloudTop));
	Y = Line(X, Y, FString::Printf(TEXT("Precip %.1f mm/h %s   %.0f dBZ"), S.PrecipitationRate, *UAerosWeatherLibrary::PrecipitationTypeToText(S.PrecipitationType).ToString(), S.Reflectivity),
		S.PrecipitationRate > 10.f ? Warn : FLinearColor::White);
	if (S.HailDiameter > 0.f) Y = Line(X, Y, FString::Printf(TEXT("HAIL %.0f mm"), S.HailDiameter), S.HailDiameter >= 25.f ? Danger : Warn);
	Y = Line(X, Y, FString::Printf(TEXT("Visibility %.1f km   Lightning %.1f /min"), S.Visibility * 0.001f, S.LightningRate), Dim);
	Y = Line(X, Y, FString::Printf(TEXT("Ground: wet %.0f%%  snow %.1f cm  hail %.0f%%"), S.Wetness * 100.f, S.SnowDepth, S.HailCover * 100.f), Dim);

	// ---- Sounding ---------------------------------------------------------------------------
	Y += 14.f;
	DrawPanel(X - 8, Y - 6, 330, 128);
	Y = Line(X, Y, TEXT("SOUNDING"), Accent);
	Y = Line(X, Y, FString::Printf(TEXT("CAPE %.0f J/kg   CIN %.0f J/kg   LI %.1f"), S.CAPE, S.CIN, S.LiftedIndex), S.CAPE > 2500.f ? Warn : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("LCL %.0f m   LFC %.0f m   EL %.0f m"), S.LCL, S.LFC, S.EquilibriumLevel));
	Y = Line(X, Y, FString::Printf(TEXT("Freezing level %.0f m   Updraft %.0f m/s"), S.FreezingLevel, S.Updraft));
	Y = Line(X, Y, FString::Printf(TEXT("SRH 0-1 %.0f   0-3 %.0f m2/s2   Shear 0-6 %.0f m/s"), S.SRH01, S.SRH03, S.BulkShear06));
	Y = Line(X, Y, FString::Printf(TEXT("STP %.1f   SCP %.1f   SHIP %.1f   Meso %.0f%%"), S.STP, S.SCP, S.SHIP, S.Mesocyclone * 100.f), S.STP > 1.f ? Danger : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("Convective activity %.0f%%"), S.ConvectiveActivity * 100.f), Dim);

	// ---- Nearby storms -------------------------------------------------------------------------
	TArray<FAerosStormData> Storms = Sub->GetStorms();
	Storms.Sort([&Viewer](const FAerosStormData& A, const FAerosStormData& B) { return FVector::DistSquared2D(A.Location, Viewer) < FVector::DistSquared2D(B.Location, Viewer); });
	const float SX = SW - RadarSize - 20.f;
	float SY = 20.f + RadarSize + 30.f;
	DrawPanel(SX - 8, SY - 6, RadarSize + 8, 20.f + 15.f * FMath::Min(Storms.Num(), 6) + 4.f);
	SY = Line(SX, SY, TEXT("NEAREST STORMS"), Accent);
	for (int32 K = 0; K < FMath::Min(Storms.Num(), 6); ++K)
	{
		const FAerosStormData& St = Storms[K];
		const float D = float(FVector::Dist2D(St.Location, Viewer)) * 1e-5f;
		const FLinearColor C = St.Type == EAerosStormType::Supercell ? Danger : (St.MaxHail > 0.f ? Warn : FLinearColor::White);
		SY = Line(SX, SY, FString::Printf(TEXT("#%d %s %3.0f km %s  %2.0f dBZ  hail %2.0f"), St.Id,
			St.Type == EAerosStormType::Supercell ? TEXT("SUP") : (St.Type == EAerosStormType::Multicell ? TEXT("MUL") : TEXT("SGL")),
			D, *Compass(Bearing(Viewer, St.Location)), St.MaxReflectivity, St.MaxHail), C);
	}

	// ---- Warnings -------------------------------------------------------------------------------
	FString Warning;
	for (const AAerosTornado* Tor : Sub->GetActiveTornadoes())
	{
		const float D = float(FVector::Dist2D(Tor->GetActorLocation(), Viewer)) * 1e-5f;
		if (D < 30.f) { Warning = FString::Printf(TEXT("TORNADO WARNING  EF%d  %.1f km %s"), Tor->GetEFRating(), D, *Compass(Bearing(Viewer, Tor->GetActorLocation()))); break; }
	}
	if (Warning.IsEmpty())
	{
		for (const FAerosCycloneData& Cy : Sub->GetCyclones())
		{
			const float D = float(FVector::Dist2D(Cy.Location, Viewer)) * 1e-5f;
			if (D < Cy.RadiusMaxWindKm * 6.f)
			{
				Warning = FString::Printf(TEXT("%s WARNING  %.0f hPa  %.0f km/h  eye %.0f km %s"), Cy.Category > 0 ? *FString::Printf(TEXT("HURRICANE CAT %d"), Cy.Category) : TEXT("TROPICAL STORM"),
					Cy.CentralPressure, Cy.MaxWind * 3.6f, D, *Compass(Bearing(Viewer, Cy.Location)));
				break;
			}
		}
	}
	if (Warning.IsEmpty() && UAerosWeatherLibrary::IsSevere(S)) Warning = TEXT("SEVERE WEATHER: TAKE SHELTER");
	if (!Warning.IsEmpty())
	{
		const float Blink = FMath::Fmod(GetWorld()->GetTimeSeconds(), 1.f) < 0.6f ? 1.f : 0.5f;
		DrawRect(FLinearColor(0.5f, 0.f, 0.f, 0.7f * Blink), SW * 0.5f - 260.f, 14.f, 520.f, 30.f);
		DrawText(Warning, FLinearColor::White, SW * 0.5f - 250.f, 20.f, GEngine->GetMediumFont(), 1.f);
	}

	// ---- Messages + hint -------------------------------------------------------------------------
	if (Pawn && Pawn->MessageTime > 0.f)
	{
		DrawText(Pawn->LastMessage, FLinearColor(1, 1, 1, FMath::Min(Pawn->MessageTime, 1.f)), SW * 0.5f - 250.f, SH - 80.f, GEngine->GetMediumFont());
	}
	Line(20.f, SH - 26.f, TEXT("F1/Tab: help & HUD   1-7 scenarios   B bubble   T tornado   H hurricane   F fly to storm   +/- time   R radar"), Dim);
}

void AAerosDemoHUD::DrawRadar(UAerosWeatherSubsystem* Sub, float X, float Y, float Size)
{
	AAerosWeatherRadar* Radar = nullptr;
	for (TActorIterator<AAerosWeatherRadar> It(GetWorld()); It; ++It) { Radar = *It; break; }
	DrawPanel(X - 8, Y - 6, Size + 16, Size + 30);
	DrawRect(FLinearColor(0.01f, 0.02f, 0.03f, 0.9f), X, Y, Size, Size);
	if (!Radar || !Radar->GetRadarTexture()) return;

	DrawTexture(Radar->GetRadarTexture(), X, Y, Size, Size, 0, 0, 1, 1, FLinearColor::White, BLEND_Translucent);

	const float CX = X + Size * 0.5f, CY = Y + Size * 0.5f, R = Size * 0.5f;
	// Range rings
	for (int32 Ring = 1; Ring <= 3; ++Ring)
	{
		const float RR = R * Ring / 3.f;
		FVector2D Prev(CX + RR, CY);
		for (int32 K = 1; K <= 48; ++K)
		{
			const float A = 2.f * PI * K / 48.f;
			const FVector2D P(CX + FMath::Cos(A) * RR, CY + FMath::Sin(A) * RR);
			DrawLine(Prev.X, Prev.Y, P.X, P.Y, FLinearColor(0.2f, 0.35f, 0.3f, 0.6f));
			Prev = P;
		}
	}
	// Beam
	const float Az = FMath::DegreesToRadians(Radar->GetBeamAzimuth());
	DrawLine(CX, CY, CX + FMath::Sin(Az) * R, CY - FMath::Cos(Az) * R, FLinearColor(0.3f, 1.f, 0.5f, 0.8f), 2.f);
	// Viewer + view direction
	DrawRect(FLinearColor::White, CX - 2, CY - 2, 4, 4);
	if (const APlayerController* PC = GetOwningPlayerController())
	{
		const FVector Fwd = PC->GetControlRotation().Vector().GetSafeNormal2D();
		DrawLine(CX, CY, CX + float(Fwd.X) * 18.f, CY + float(Fwd.Y) * 18.f, FLinearColor::White, 1.5f);
	}
	// Storm markers (supercells / mesocyclones)
	for (const FAerosStormData& St : Sub->GetStorms())
	{
		FVector2D UV;
		if (St.Mesocyclone < 0.4f || !Radar->WorldToRadarUV(St.UpdraftLocation, UV)) continue;
		const float PX = X + float(UV.X) * Size, PY = Y + float(UV.Y) * Size;
		DrawLine(PX - 6, PY - 6, PX + 6, PY + 6, FLinearColor(1, 1, 0));
		DrawLine(PX - 6, PY + 6, PX + 6, PY - 6, FLinearColor(1, 1, 0));
	}
	DrawText(TEXT("N"), FLinearColor::White, CX - 4, Y + 2, GEngine->GetSmallFont());
	DrawText(FString::Printf(TEXT("%s  %.0f km"), *StaticEnum<EAerosRadarProduct>()->GetDisplayNameTextByValue(int64(Radar->Product)).ToString(), Radar->RangeKm),
		Dim, X, Y + Size + 6, GEngine->GetSmallFont());

	// Colour legend
	const float LX = X + Size - 150.f, LY = Y + Size + 6.f;
	for (int32 K = 0; K < 14; ++K)
	{
		DrawRect(AAerosWeatherRadar::ReflectivityColor(5.f + K * 5.f), LX + K * 10.f, LY + 2.f, 10.f, 8.f);
	}
}

void AAerosDemoHUD::DrawHelp(float X, float Y)
{
	DrawPanel(X - 8, Y - 6, 560, 380);
	const TCHAR* Lines[] = {
		TEXT("AEROS WEATHER DEMO - CONTROLS"),
		TEXT(""),
		TEXT("Mouse look, WASD move, E/Space up, Q/C down, Shift x10, Ctrl x100 speed"),
		TEXT("F1 / Tab      cycle HUD: full -> radar only -> hidden -> this help"),
		TEXT("1 Climate (seasons)   2 Fair weather   3 Summer thunderstorms"),
		TEXT("4 Supercell outbreak  5 Winter storm   6 Tropical season   7 Frontal rain"),
		TEXT("B             inject a thermal bubble 15 km ahead (triggers a storm if unstable)"),
		TEXT("T             force a tornado under the strongest storm (server)"),
		TEXT("H             seed a hurricane 250 km east (ocean)"),
		TEXT("F             teleport near the nearest storm"),
		TEXT("+ / -         simulation speed x2 / /2      P pause weather"),
		TEXT("N / M         -1 / +1 hour          J / K   -1 / +1 month (seasons)"),
		TEXT("R             radar product: reflectivity / velocity / precipitation type"),
		TEXT("V             debug draw (storm circles, wind arrows)"),
		TEXT(""),
		TEXT("Console: aeros.Scenario N, aeros.TimeScale X, aeros.Time day hour,"),
		TEXT("         aeros.Bubble, aeros.Hurricane, aeros.Tornado, stat Aeros"),
		TEXT(""),
		TEXT("Tip: the clock runs at x60 by default. Use - to slow down to real time"),
		TEXT("to chase a tornado; use + to watch storms form and seasons change."),
	};
	for (const TCHAR* L : Lines) Y = Line(X, Y, L, FLinearColor::White);
}
