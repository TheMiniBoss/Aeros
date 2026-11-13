// Copyright Aeros. All Rights Reserved.

#include "AerosDemoHUD.h"
#include "AerosDemoPawn.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherLibrary.h"
#include "AerosWeatherRadar.h"
#include "AerosCloudManager.h"
#include "AerosHazardManager.h"
#include "AerosTornado.h"
#include "AerosVolcano.h"
#include "AerosEarthquake.h"
#include "AerosLiveWeather.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"

// Unity build: helpers live in a uniquely named namespace (see CLAUDE.md).
namespace AerosDemoHudInternal
{
	const FLinearColor Dim(0.65f, 0.7f, 0.75f);
	const FLinearColor Accent(0.4f, 0.8f, 1.f);
	const FLinearColor Warn(1.f, 0.75f, 0.2f);
	const FLinearColor Danger(1.f, 0.25f, 0.25f);
	const FLinearColor Good(0.35f, 0.9f, 0.45f);

	FString Compass(float Deg)
	{
		static const TCHAR* Names[] = { TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"), TEXT("S"), TEXT("SO"), TEXT("O"), TEXT("NO") };
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

	float Km(const FVector& A, const FVector& B) { return float(FVector::Dist2D(A, B)) * 1e-5f; }

	FLinearColor RiskColor(float V)
	{
		if (V < 0.25f) return Good;
		if (V < 0.5f) return FLinearColor(0.95f, 0.9f, 0.3f);
		if (V < 0.75f) return Warn;
		return Danger;
	}

	const TCHAR* RiskWord(float V)
	{
		if (V < 0.1f) return TEXT("nul");
		if (V < 0.25f) return TEXT("faible");
		if (V < 0.5f) return TEXT("mod\u00E9r\u00E9");
		if (V < 0.75f) return TEXT("fort");
		return TEXT("extr\u00EAme");
	}

	FString PhenomenonFr(EAerosPhenomenon P)
	{
		switch (P)
		{
		case EAerosPhenomenon::Clear: return TEXT("Ciel d\u00E9gag\u00E9");
		case EAerosPhenomenon::PartlyCloudy: return TEXT("Partiellement nuageux");
		case EAerosPhenomenon::Overcast: return TEXT("Couvert");
		case EAerosPhenomenon::Fog: return TEXT("Brouillard");
		case EAerosPhenomenon::Drizzle: return TEXT("Bruine");
		case EAerosPhenomenon::Rain: return TEXT("Pluie");
		case EAerosPhenomenon::HeavyRain: return TEXT("Forte pluie");
		case EAerosPhenomenon::Snow: return TEXT("Neige");
		case EAerosPhenomenon::Blizzard: return TEXT("Blizzard");
		case EAerosPhenomenon::FreezingRain: return TEXT("Pluie vergla\u00E7ante");
		case EAerosPhenomenon::Thunderstorm: return TEXT("Orage");
		case EAerosPhenomenon::SevereThunderstorm: return TEXT("Orage violent");
		case EAerosPhenomenon::Hail: return TEXT("Gr\u00EAle");
		case EAerosPhenomenon::Supercell: return TEXT("Supercellule");
		case EAerosPhenomenon::Tornado: return TEXT("Tornade");
		case EAerosPhenomenon::TropicalStorm: return TEXT("Temp\u00EAte tropicale");
		case EAerosPhenomenon::Hurricane: return TEXT("Ouragan");
		case EAerosPhenomenon::Sandstorm: return TEXT("Temp\u00EAte de sable");
		case EAerosPhenomenon::VolcanicAshfall: return TEXT("Pluie de cendres");
		case EAerosPhenomenon::Flood: return TEXT("Inondation");
		case EAerosPhenomenon::FlashFlood: return TEXT("Crue \u00E9clair");
		case EAerosPhenomenon::StormSurge: return TEXT("Onde de temp\u00EAte");
		case EAerosPhenomenon::Tsunami: return TEXT("Tsunami");
		case EAerosPhenomenon::Earthquake: return TEXT("S\u00E9isme");
		default: return TEXT("?");
		}
	}

	FString ScenarioFr(EAerosScenario S)
	{
		switch (S)
		{
		case EAerosScenario::Climate: return TEXT("Climat (saisons)");
		case EAerosScenario::FairWeather: return TEXT("Beau temps");
		case EAerosScenario::SummerThunderstorms: return TEXT("Orages d'\u00E9t\u00E9");
		case EAerosScenario::SupercellOutbreak: return TEXT("Supercellules");
		case EAerosScenario::WinterStorm: return TEXT("Temp\u00EAte de neige");
		case EAerosScenario::TropicalSeason: return TEXT("Saison tropicale");
		case EAerosScenario::FrontalRain: return TEXT("Pluie frontale");
		case EAerosScenario::Custom: return TEXT("Personnalis\u00E9");
		case EAerosScenario::Blizzard: return TEXT("Blizzard");
		case EAerosScenario::DesertDust: return TEXT("D\u00E9sert");
		case EAerosScenario::Monsoon: return TEXT("Mousson");
		default: return TEXT("?");
		}
	}

	FString SeasonFr(EAerosSeason S)
	{
		switch (S)
		{
		case EAerosSeason::Winter: return TEXT("hiver");
		case EAerosSeason::Spring: return TEXT("printemps");
		case EAerosSeason::Summer: return TEXT("\u00E9t\u00E9");
		default: return TEXT("automne");
		}
	}

	FString PrecipFr(EAerosPrecipType T)
	{
		switch (T)
		{
		case EAerosPrecipType::Rain: return TEXT("Pluie");
		case EAerosPrecipType::Snow: return TEXT("Neige");
		case EAerosPrecipType::Sleet: return TEXT("Gr\u00E9sil");
		case EAerosPrecipType::FreezingRain: return TEXT("Pluie vergla\u00E7ante");
		case EAerosPrecipType::Hail: return TEXT("Gr\u00EAle");
		default: return TEXT("Pr\u00E9cipitations");
		}
	}

	FString StormTypeFr(EAerosStormType T)
	{
		switch (T)
		{
		case EAerosStormType::Supercell: return TEXT("Supercellule");
		case EAerosStormType::Multicell: return TEXT("Orage multicellulaire");
		default: return TEXT("Orage");
		}
	}

	FLinearColor StormColor(const FAerosStormData& S)
	{
		if (S.Type == EAerosStormType::Supercell) return Danger;
		if (S.MaxHail > 15.f) return Warn;
		return FLinearColor(0.85f, 0.85f, 1.f);
	}
}

void AAerosDemoHUD::BeginPlay()
{
	Super::BeginPlay();
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub || bBound) return;
	bBound = true;
	Sub->OnStormFormed.AddDynamic(this, &AAerosDemoHUD::HandleStormFormed);
	Sub->OnTornadoTouchdown.AddDynamic(this, &AAerosDemoHUD::HandleTornado);
	Sub->OnCycloneFormed.AddDynamic(this, &AAerosDemoHUD::HandleCyclone);
	Sub->OnEarthquake.AddDynamic(this, &AAerosDemoHUD::HandleEarthquake);
	Sub->OnTsunami.AddDynamic(this, &AAerosDemoHUD::HandleTsunami);
	Sub->OnVolcanicEruption.AddDynamic(this, &AAerosDemoHUD::HandleEruption);
	Sub->OnPhenomenonChanged.AddDynamic(this, &AAerosDemoHUD::HandlePhenomenon);
}

// =====================================================================
//  Drawing primitives
// =====================================================================

void AAerosDemoHUD::DrawPanel(float X, float Y, float W, float H)
{
	DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.66f), X, Y, W, H);
}

float AAerosDemoHUD::Line(float X, float Y, const FString& Text, const FLinearColor& Color, float Scale)
{
	DrawText(Text, Color, X, Y, GEngine->GetSmallFont(), Scale);
	return Y + 15.f * Scale;
}

float AAerosDemoHUD::Paragraph(float X, float Y, float Width, const FString& Text, const FLinearColor& Color)
{
	// Greedy word wrap with the real glyph widths
	TArray<FString> Words;
	Text.ParseIntoArray(Words, TEXT(" "));
	FString Current;
	for (const FString& Word : Words)
	{
		const FString Candidate = Current.IsEmpty() ? Word : Current + TEXT(" ") + Word;
		float W = 0.f, H = 0.f;
		GetTextSize(Candidate, W, H, GEngine->GetSmallFont());
		if (W > Width && !Current.IsEmpty())
		{
			Y = Line(X, Y, Current, Color);
			Current = Word;
		}
		else
		{
			Current = Candidate;
		}
	}
	if (!Current.IsEmpty()) Y = Line(X, Y, Current, Color);
	return Y;
}

float AAerosDemoHUD::Gauge(float X, float Y, float Width, const FString& Label, float Value)
{
	using namespace AerosDemoHudInternal;
	Value = FMath::Clamp(Value, 0.f, 1.f);
	const float LabelW = 110.f, BarW = Width - LabelW - 70.f;
	DrawText(Label, FLinearColor::White, X, Y, GEngine->GetSmallFont());
	DrawRect(FLinearColor(1, 1, 1, 0.12f), X + LabelW, Y + 3.f, BarW, 9.f);
	DrawRect(RiskColor(Value), X + LabelW, Y + 3.f, FMath::Max(2.f, BarW * Value), 9.f);
	DrawText(RiskWord(Value), RiskColor(Value), X + LabelW + BarW + 8.f, Y, GEngine->GetSmallFont());
	return Y + 16.f;
}

void AAerosDemoHUD::DrawWindRose(float CX, float CY, float R, const FVector& Wind)
{
	// Camera-relative: up = where the camera looks. The arrow shows where the wind blows to.
	FVector Fwd(1, 0, 0);
	if (const APlayerController* PC = GetOwningPlayerController()) Fwd = PC->GetControlRotation().Vector().GetSafeNormal2D();
	const FVector Right(-Fwd.Y, Fwd.X, 0.0);
	const FVector2D Dir(float(FVector::DotProduct(Wind, Right)), float(-FVector::DotProduct(Wind, Fwd)));

	FVector2D Prev(CX + R, CY);
	for (int32 K = 1; K <= 24; ++K)
	{
		const float A = 2.f * PI * K / 24.f;
		const FVector2D P(CX + FMath::Cos(A) * R, CY + FMath::Sin(A) * R);
		DrawLine(Prev.X, Prev.Y, P.X, P.Y, FLinearColor(1, 1, 1, 0.35f));
		Prev = P;
	}
	DrawText(TEXT("avant"), FLinearColor(1, 1, 1, 0.5f), CX - 14.f, CY - R - 14.f, GEngine->GetSmallFont(), 0.8f);
	if (Dir.Size() < 0.3f) { DrawText(TEXT("calme"), FLinearColor::White, CX - 16.f, CY - 6.f, GEngine->GetSmallFont()); return; }
	const FVector2D U = Dir.GetSafeNormal();
	const float Len = R * FMath::Clamp(0.4f + Dir.Size() / 30.f, 0.4f, 0.95f);
	const FVector2D Tip(CX + U.X * Len, CY + U.Y * Len), Tail(CX - U.X * Len, CY - U.Y * Len);
	const FVector2D Side(-U.Y, U.X);
	const FLinearColor C = Dir.Size() > 20.f ? AerosDemoHudInternal::Danger : (Dir.Size() > 10.f ? AerosDemoHudInternal::Warn : AerosDemoHudInternal::Accent);
	DrawLine(Tail.X, Tail.Y, Tip.X, Tip.Y, C, 3.f);
	DrawLine(Tip.X, Tip.Y, Tip.X - U.X * 10.f + Side.X * 7.f, Tip.Y - U.Y * 10.f + Side.Y * 7.f, C, 3.f);
	DrawLine(Tip.X, Tip.Y, Tip.X - U.X * 10.f - Side.X * 7.f, Tip.Y - U.Y * 10.f - Side.Y * 7.f, C, 3.f);
}

// =====================================================================
//  Main
// =====================================================================

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
		if (Flash > 0.01f) DrawRect(FLinearColor(0.9f, 0.92f, 1.f, Flash * 0.12f), 0, 0, SW, SH); // subtle: the 3D light and clouds do the rest
	}

	if (!Sub || !Sub->IsSimulationRunning())
	{
		Line(20, 20, TEXT("Aeros: simulation not running (add an Aeros Weather Controller or use AerosDemoGameMode)."), AerosDemoHudInternal::Warn);
		return;
	}

	DrawCaption();
	if (Mode == EAerosHudMode::Hidden) return;
	if (Mode == EAerosHudMode::Help) { DrawHelp(20, 20); return; }

	const FAerosWeatherSample S = Sub->GetWeatherAtLocation(Sub->GetViewerLocation());
	const float RadarSize = FMath::Min(SH * 0.36f, 320.f);
	DrawRadar(Sub, SW - RadarSize - 20.f, 20.f, RadarSize);
	if (Mode == EAerosHudMode::RadarOnly) return;

	DrawWorldMarkers(Sub);
	if (Mode == EAerosHudMode::Simple) DrawSimple(Sub, S);
	else DrawExpert(Sub, S);
	DrawWarning(Sub, S);
	DrawFeed();

	if (const AAerosDemoPawn* Pawn = Cast<AAerosDemoPawn>(GetOwningPawn()); Pawn && Pawn->MessageTime > 0.f)
	{
		DrawText(Pawn->LastMessage, FLinearColor(1, 1, 1, FMath::Min(Pawn->MessageTime, 1.f)), SW * 0.5f - 250.f, SH - 80.f, GEngine->GetMediumFont());
	}
	Line(20.f, SH - 26.f, TEXT("F1/Tab affichage   F2 visite guid\u00E9e   ZQSD bouger   &\u00E9\"'(-\u00E8_\u00E7\u00E0 sc\u00E9narios   G s\u00E9isme  Y tsunami  U volcan  O d\u00E9luge  L haboob  W m\u00E9t\u00E9o r\u00E9elle"), AerosDemoHudInternal::Dim);
}

// =====================================================================
//  Simple mode: plain language
// =====================================================================

void AAerosDemoHUD::DrawSimple(UAerosWeatherSubsystem* Sub, const FAerosWeatherSample& S)
{
	using namespace AerosDemoHudInternal;
	const FAerosDateTime T = Sub->GetDateTime();
	const float X = 24.f, W = 360.f;
	float Y = 22.f;

	// ---- Here and now
	const FString Explanation = Explain(Sub, S);
	DrawPanel(X - 10, Y - 8, W + 20, 250.f + 15.f * FMath::Max(0, Explanation.Len() / 55 - 2));
	static const TCHAR* Months[] = { TEXT("janv."), TEXT("f\u00E9vr."), TEXT("mars"), TEXT("avril"), TEXT("mai"), TEXT("juin"), TEXT("juil."), TEXT("ao\u00FBt"), TEXT("sept."), TEXT("oct."), TEXT("nov."), TEXT("d\u00E9c.") };
	const int32 HH = int32(T.Hour), MM = int32((T.Hour - HH) * 60.f);
	Y = Line(X, Y, FString::Printf(TEXT("%02d:%02d  %d %s (%s)   temps x%.0f%s   %s"), HH, MM, T.DayOfMonth, Months[FMath::Clamp(T.Month - 1, 0, 11)],
		*SeasonFr(T.Season), Sub->GetTimeScale(), Sub->IsPaused() ? TEXT(" PAUSE") : TEXT(""), *ScenarioFr(Sub->GetScenario())), Dim);
	Y += 4.f;
	DrawText(PhenomenonFr(S.Phenomenon), UAerosWeatherLibrary::IsSevere(S) ? Danger : Accent, X, Y, GEngine->GetMediumFont(), 1.5f);
	Y += 34.f;

	// Big numbers + wind rose
	DrawText(FString::Printf(TEXT("%.0f\u00B0C"), S.Temperature), FLinearColor::White, X, Y - 4.f, GEngine->GetMediumFont(), 1.8f);
	Line(X + 90.f, Y + 2.f, FString::Printf(TEXT("ressenti %.0f\u00B0C"), S.FeelsLike), Dim);
	Line(X + 90.f, Y + 17.f, FString::Printf(TEXT("humidit\u00E9 %.0f%%"), S.RelativeHumidity * 100.f), Dim);
	DrawWindRose(X + W - 40.f, Y + 18.f, 26.f, S.Wind);
	Y += 40.f;
	Y = Line(X, Y, FString::Printf(TEXT("Vent %.0f km/h venant du %s"), S.WindSpeed * 3.6f, *Compass(WindFrom(S.Wind))), S.WindSpeed > 20.f ? Warn : FLinearColor::White);
	const FString Precip = S.PrecipitationRate < 0.1f ? FString(TEXT("Pas de pr\u00E9cipitation")) :
		FString::Printf(TEXT("%s : %.1f mm/h%s"), *PrecipFr(S.PrecipitationType), S.PrecipitationRate,
			S.HailDiameter > 0.f ? *FString::Printf(TEXT(", gr\u00EAlons %.0f mm"), S.HailDiameter) : TEXT(""));
	Y = Line(X, Y, Precip, S.PrecipitationRate > 10.f ? Warn : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("Nuages %.0f%%   visibilit\u00E9 %.1f km"), S.CloudCover * 100.f, S.Visibility * 0.001f), Dim);
	Y += 6.f;
	Y = Line(X, Y, TEXT("CE QUI SE PASSE"), Accent);
	Y = Paragraph(X, Y, W, Explanation, FLinearColor::White);

	// ---- Risk gauges
	Y += 20.f;
	float TornadoNear = 0.f;
	for (const AAerosTornado* Tor : Sub->GetActiveTornadoes())
	{
		TornadoNear = FMath::Max(TornadoNear, FMath::Clamp(1.f - Km(Tor->GetActorLocation(), Sub->GetViewerLocation()) / 20.f, 0.f, 1.f));
	}
	const float Shake = Sub->GetGroundShakingAt(Sub->GetViewerLocation());
	DrawPanel(X - 10, Y - 8, W + 20, 8 * 16.f + 26.f);
	Y = Line(X, Y, TEXT("RISQUES ICI"), Accent);
	Y = Gauge(X, Y, W, TEXT("Orage / foudre"), FMath::Max3(S.ConvectiveActivity, FMath::Clamp(S.CAPE / 3500.f, 0.f, 0.7f), S.LightningRate / 10.f));
	Y = Gauge(X, Y, W, TEXT("Gr\u00EAle"), FMath::Max(S.HailDiameter / 50.f, S.SHIP / 2.f));
	Y = Gauge(X, Y, W, TEXT("Tornade"), FMath::Max3(S.STP / 3.f, S.Mesocyclone * 0.8f, TornadoNear));
	Y = Gauge(X, Y, W, TEXT("Vent violent"), S.WindSpeed / 35.f);
	Y = Gauge(X, Y, W, TEXT("Inondation"), FMath::Max(S.FloodDepth / 1.f, S.SeaLevelAnomaly / 2.f));
	Y = Gauge(X, Y, W, TEXT("Visibilit\u00E9 r\u00E9duite"), 1.f - S.Visibility / 10000.f);
	Y = Gauge(X, Y, W, TEXT("Neige / glace"), FMath::Max(S.SnowDepth / 30.f, S.BlowingSnow));
	Y = Gauge(X, Y, W, TEXT("Secousse"), Shake / 0.3f);
}

FString AAerosDemoHUD::Explain(UAerosWeatherSubsystem* Sub, const FAerosWeatherSample& S) const
{
	using namespace AerosDemoHudInternal;
	TArray<FString> Out;
	const FVector Viewer = Sub->GetViewerLocation();

	// Disasters first: they are the most urgent thing to understand
	for (const AAerosTornado* Tor : Sub->GetActiveTornadoes())
	{
		const float D = Km(Tor->GetActorLocation(), Viewer);
		if (D < 40.f)
		{
			Out.Add(FString::Printf(TEXT("Une tornade EF%d touche le sol \u00E0 %.0f km (%s). Le vent y tourne \u00E0 plus de %.0f km/h et aspire les objets."),
				Tor->GetEFRating(), D, *Compass(Bearing(Viewer, Tor->GetActorLocation())), Tor->MaxWind * 3.6f));
			break;
		}
	}
	const float Shake = Sub->GetGroundShakingAt(Viewer);
	if (Shake > 0.01f) Out.Add(TEXT("Le sol tremble : les ondes du s\u00E9isme arrivent (d'abord les ondes P, rapides, puis les ondes S, plus fortes)."));
	if (S.SeaLevelAnomaly > 0.4f) Out.Add(FString::Printf(TEXT("La mer monte de %.1f m au-dessus de la normale : l'eau envahit la c\u00F4te."), S.SeaLevelAnomaly));
	else if (S.SeaLevelAnomaly < -0.4f) Out.Add(TEXT("La mer se retire anormalement : c'est le signe qu'une vague de tsunami arrive."));
	if (S.FloodDepth > 0.1f) Out.Add(FString::Printf(TEXT("Le sol ne peut plus absorber l'eau : %.1f m d'eau coulent vers les zones basses."), S.FloodDepth));
	if (S.AshFall > 0.02f) Out.Add(TEXT("Le vent d'altitude pousse le panache du volcan jusqu'ici : des cendres tombent."));
	if (S.Dust > 3.f) Out.Add(TEXT("Le vent fort soul\u00E8ve le sable du sol sec : un mur de poussi\u00E8re r\u00E9duit la visibilit\u00E9."));
	if (S.BlowingSnow > 0.3f) Out.Add(TEXT("Le vent soul\u00E8ve la neige d\u00E9j\u00E0 tomb\u00E9e : on ne voit presque plus rien (voile blanc)."));

	// Then the weather itself
	if (S.PrecipitationType == EAerosPrecipType::Snow && S.PrecipitationRate > 0.1f)
		Out.Add(TEXT("Il fait moins de 0\u00B0C de la base du nuage jusqu'au sol : les flocons ne fondent pas."));
	else if (S.HailDiameter > 5.f)
		Out.Add(FString::Printf(TEXT("Le courant ascendant de l'orage (%.0f m/s) garde les gr\u00EAlons en l'air assez longtemps pour qu'ils grossissent."), S.Updraft));
	else if (S.ConvectiveActivity > 0.3f)
		Out.Add(TEXT("Un orage est au-dessus de toi : l'air chaud monte tr\u00E8s vite, se refroidit et forme une tour de nuage qui produit pluie et foudre."));
	else if (S.PrecipitationRate > 0.5f)
		Out.Add(TEXT("L'air monte lentement sur une grande zone (front) : il se refroidit, l'humidit\u00E9 condense et il pleut longtemps."));

	if (S.CAPE > 800.f && S.ConvectiveActivity < 0.3f)
	{
		FString Line1 = FString::Printf(TEXT("L'air pr\u00E8s du sol est chaud et humide (CAPE %.0f) : s'il est soulev\u00E9, il montera seul et formera des orages."), S.CAPE);
		if (S.CIN < -60.f) Line1 += TEXT(" Pour l'instant, une couche d'air chaud en altitude le bloque (CIN).");
		Out.Add(Line1);
	}
	if (S.BulkShear06 > 18.f && S.CAPE > 500.f)
		Out.Add(TEXT("Le vent change de force et de direction avec l'altitude : les orages peuvent se mettre \u00E0 tourner (supercellules)."));
	if (S.STP > 1.f)
		Out.Add(TEXT("Toutes les conditions des tornades sont r\u00E9unies : base des nuages basse et vent qui tourne pr\u00E8s du sol."));

	if (Out.Num() == 0)
	{
		if (S.CloudCover < 0.2f) Out.Add(TEXT("L'air descend doucement et s'ass\u00E8che : aucun nuage ne peut se former. Le soleil chauffe le sol."));
		else if (S.CloudCover < 0.7f) Out.Add(TEXT("Le soleil chauffe le sol, des bulles d'air chaud montent et forment des petits nuages (cumulus)."));
		else Out.Add(TEXT("Une couche de nuages couvre le ciel, mais l'air ne monte pas assez pour faire pleuvoir."));
	}

	// Nearest notable storm, to give the player a direction to look at
	FAerosStormData Storm;
	if (Sub->GetNearestStorm(Viewer, Storm, 120.f) && Km(Storm.Location, Viewer) > Storm.RadiusKm)
	{
		Out.Add(FString::Printf(TEXT("%s le plus proche : %.0f km au %s (touche F pour y aller)."), *StormTypeFr(Storm.Type),
			Km(Storm.Location, Viewer), *Compass(Bearing(Viewer, Storm.Location))));
	}

	while (Out.Num() > 3) Out.RemoveAt(Out.Num() - 2);
	return FString::Join(Out, TEXT(" "));
}

// =====================================================================
//  Expert mode: the full numbers
// =====================================================================

void AAerosDemoHUD::DrawExpert(UAerosWeatherSubsystem* Sub, const FAerosWeatherSample& S)
{
	using namespace AerosDemoHudInternal;
	const FAerosDateTime T = Sub->GetDateTime();
	const FAerosSimStats Stats = Sub->GetStats();
	const FVector Viewer = Sub->GetViewerLocation();

	float X = 20.f, Y = 20.f;
	DrawPanel(X - 8, Y - 6, 330, 118);
	Y = Line(X, Y, TEXT("AEROS WEATHER (expert)"), Accent, 1.2f);
	const int32 HH = int32(T.Hour), MM = int32((T.Hour - HH) * 60.f);
	Y = Line(X, Y, FString::Printf(TEXT("Day %d (%02d/%02d)  %02d:%02d   %s"), T.DayOfYear, T.DayOfMonth, T.Month, HH, MM, *UAerosWeatherLibrary::SeasonToText(T.Season).ToString()));
	Y = Line(X, Y, FString::Printf(TEXT("Sun elevation %.0f deg, azimuth %.0f deg"), T.SunElevation, T.SunAzimuth), Dim);
	Y = Line(X, Y, FString::Printf(TEXT("Scenario: %s   time x%.0f%s"), *UAerosWeatherLibrary::ScenarioToText(Sub->GetScenario()).ToString(), Sub->GetTimeScale(), Sub->IsPaused() ? TEXT("  [PAUSED]") : TEXT("")));
	Y = Line(X, Y, FString::Printf(TEXT("Grid %d cells  step %.1f ms  lag %.0f s"), Stats.NumCells, Stats.StepMs, Stats.LagSeconds), Dim);
	Y = Line(X, Y, FString::Printf(TEXT("Storms %d  cyclones %d  tornadoes %d"), Stats.NumStorms, Stats.NumCyclones, Sub->GetActiveTornadoes().Num()), Dim);

	Y += 14.f;
	DrawPanel(X - 8, Y - 6, 330, 172);
	const FLinearColor PhCol = UAerosWeatherLibrary::IsSevere(S) ? Danger : Accent;
	Y = Line(X, Y, UAerosWeatherLibrary::PhenomenonToText(S.Phenomenon).ToString().ToUpper(), PhCol, 1.2f);
	Y = Line(X, Y, FString::Printf(TEXT("Temp %.1f C (feels %.0f)  Dew %.1f C  RH %.0f%%"), S.Temperature, S.FeelsLike, S.DewPoint, S.RelativeHumidity * 100.f));
	Y = Line(X, Y, FString::Printf(TEXT("Pressure %.1f hPa   Ground %.1f C"), S.Pressure, S.GroundTemperature));
	Y = Line(X, Y, FString::Printf(TEXT("Wind %.0f km/h from %s (%.0f deg)"), S.WindSpeed * 3.6f, *Compass(WindFrom(S.Wind)), WindFrom(S.Wind)));
	Y = Line(X, Y, FString::Printf(TEXT("Clouds %.0f%%  base %.0f m  top %.0f m"), S.CloudCover * 100.f, S.CloudBase, S.CloudTop));
	Y = Line(X, Y, FString::Printf(TEXT("Precip %.1f mm/h %s   %.0f dBZ"), S.PrecipitationRate, *UAerosWeatherLibrary::PrecipitationTypeToText(S.PrecipitationType).ToString(), S.Reflectivity),
		S.PrecipitationRate > 10.f ? Warn : FLinearColor::White);
	if (S.HailDiameter > 0.f) Y = Line(X, Y, FString::Printf(TEXT("HAIL %.0f mm"), S.HailDiameter), S.HailDiameter >= 25.f ? Danger : Warn);
	Y = Line(X, Y, FString::Printf(TEXT("Visibility %.1f km   Lightning %.1f /min"), S.Visibility * 0.001f, S.LightningRate), Dim);
	Y = Line(X, Y, FString::Printf(TEXT("Ground: wet %.0f%%  snow %.1f cm  hail %.0f%%"), S.Wetness * 100.f, S.SnowDepth, S.HailCover * 100.f), Dim);

	Y += 14.f;
	DrawPanel(X - 8, Y - 6, 330, 128);
	Y = Line(X, Y, TEXT("SOUNDING"), Accent);
	Y = Line(X, Y, FString::Printf(TEXT("CAPE %.0f J/kg   CIN %.0f J/kg   LI %.1f"), S.CAPE, S.CIN, S.LiftedIndex), S.CAPE > 2500.f ? Warn : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("LCL %.0f m   LFC %.0f m   EL %.0f m"), S.LCL, S.LFC, S.EquilibriumLevel));
	Y = Line(X, Y, FString::Printf(TEXT("Freezing level %.0f m   Updraft %.0f m/s"), S.FreezingLevel, S.Updraft));
	Y = Line(X, Y, FString::Printf(TEXT("SRH 0-1 %.0f   0-3 %.0f m2/s2   Shear 0-6 %.0f m/s"), S.SRH01, S.SRH03, S.BulkShear06));
	Y = Line(X, Y, FString::Printf(TEXT("STP %.1f   SCP %.1f   SHIP %.1f   Meso %.0f%%"), S.STP, S.SCP, S.SHIP, S.Mesocyclone * 100.f), S.STP > 1.f ? Danger : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("Convective activity %.0f%%"), S.ConvectiveActivity * 100.f), Dim);

	Y += 14.f;
	const float Shake = Sub->GetGroundShakingAt(Viewer);
	DrawPanel(X - 8, Y - 6, 330, 128);
	Y = Line(X, Y, TEXT("HAZARDS"), Accent);
	Y = Line(X, Y, FString::Printf(TEXT("Flood %.2f m  current %.1f m/s  soil %.0f%%"), S.FloodDepth, S.FloodFlow.Size(), S.SoilMoisture * 100.f), S.FloodDepth > 0.15f ? Danger : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("Sea level %+.2f m  elevation %.0f m %s"), S.SeaLevelAnomaly, S.Elevation, S.bOcean ? TEXT("(ocean)") : TEXT("")), FMath::Abs(S.SeaLevelAnomaly) > 0.5f ? Danger : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("Dust %.1f g/m2   blowing snow %.0f%%"), S.Dust, S.BlowingSnow * 100.f), S.Dust > 6.f ? Warn : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("Ash fall %.2f mm/h   ash on ground %.1f mm"), S.AshFall, S.AshDepth), S.AshFall > 0.05f ? Warn : FLinearColor::White);
	Y = Line(X, Y, FString::Printf(TEXT("Ground shaking %.2f g  (Mercalli %d)"), Shake, Shake > 0.001f ? AAerosEarthquake::MercalliFromPGA(Shake) : 1), Shake > 0.1f ? Danger : FLinearColor::White);
	if (UAerosLiveWeather* Live = Sub->GetLiveWeather(); Live && (Live->IsActive() || Live->IsBusy()))
	{
		Y = Line(X, Y, FString::Printf(TEXT("LIVE %s"), *Live->GetStatus()), Accent);
	}

	// Nearby storms under the radar
	TArray<FAerosStormData> Storms = Sub->GetStorms();
	Storms.Sort([&Viewer](const FAerosStormData& A, const FAerosStormData& B) { return FVector::DistSquared2D(A.Location, Viewer) < FVector::DistSquared2D(B.Location, Viewer); });
	const float RadarSize = FMath::Min(Canvas->ClipY * 0.36f, 320.f);
	const float SX = Canvas->ClipX - RadarSize - 20.f;
	float SY = 20.f + RadarSize + 30.f;
	DrawPanel(SX - 8, SY - 6, RadarSize + 8, 20.f + 15.f * FMath::Min(Storms.Num(), 6) + 4.f);
	SY = Line(SX, SY, TEXT("NEAREST STORMS"), Accent);
	for (int32 K = 0; K < FMath::Min(Storms.Num(), 6); ++K)
	{
		const FAerosStormData& St = Storms[K];
		SY = Line(SX, SY, FString::Printf(TEXT("#%d %s %3.0f km %s  %2.0f dBZ  hail %2.0f"), St.Id,
			St.Type == EAerosStormType::Supercell ? TEXT("SUP") : (St.Type == EAerosStormType::Multicell ? TEXT("MUL") : TEXT("SGL")),
			Km(St.Location, Viewer), *Compass(Bearing(Viewer, St.Location)), St.MaxReflectivity, St.MaxHail), StormColor(St));
	}
}

// =====================================================================
//  World markers: what is where
// =====================================================================

void AAerosDemoHUD::DrawMarker(const FVector& World, const FString& Label, const FLinearColor& Color, float Size, bool bArrowWhenOffscreen)
{
	APlayerController* PC = GetOwningPlayerController();
	if (!PC) return;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;
	FVector2D Screen;
	const bool bFront = PC->ProjectWorldLocationToScreen(World, Screen, true);
	const bool bOnScreen = bFront && Screen.X > 20.f && Screen.X < SW - 20.f && Screen.Y > 20.f && Screen.Y < SH - 20.f;

	if (bOnScreen)
	{
		// Diamond + label
		const float X = float(Screen.X), Y = float(Screen.Y);
		DrawLine(X, Y - Size, X + Size, Y, Color, 2.f);
		DrawLine(X + Size, Y, X, Y + Size, Color, 2.f);
		DrawLine(X, Y + Size, X - Size, Y, Color, 2.f);
		DrawLine(X - Size, Y, X, Y - Size, Color, 2.f);
		float TW = 0.f, TH = 0.f;
		GetTextSize(Label, TW, TH, GEngine->GetSmallFont());
		// Stack the label upward until it no longer overlaps another one (max 4 tries, then skip it)
		float LY = Y - Size - TH - 6.f;
		for (int32 Try = 0; Try < 4; ++Try)
		{
			const FBox2D Box(FVector2D(X - TW * 0.5f - 4.f, LY), FVector2D(X + TW * 0.5f + 4.f, LY + TH + 2.f));
			bool bOverlap = false;
			for (const FBox2D& Other : LabelRects) if (Other.Intersect(Box)) { bOverlap = true; break; }
			if (!bOverlap)
			{
				LabelRects.Add(Box);
				if (Try > 0) DrawLine(X, Y - Size, X, LY + TH + 2.f, FLinearColor(Color.R, Color.G, Color.B, 0.5f));
				DrawRect(FLinearColor(0, 0, 0, 0.55f), float(Box.Min.X), float(Box.Min.Y), TW + 8.f, TH + 2.f);
				DrawText(Label, Color, X - TW * 0.5f, LY + 1.f, GEngine->GetSmallFont());
				break;
			}
			LY -= TH + 6.f;
		}
		return;
	}
	if (!bArrowWhenOffscreen) return;

	// Off screen: arrow on the screen edge pointing toward it (camera-relative bearing)
	const FVector Fwd = PC->GetControlRotation().Vector().GetSafeNormal2D();
	const FVector Right(-Fwd.Y, Fwd.X, 0.0);
	FVector To = World - (PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : PC->GetFocalLocation());
	To.Z = 0.0;
	const FVector2D Dir = FVector2D(float(FVector::DotProduct(To, Right)), float(-FVector::DotProduct(To, Fwd))).GetSafeNormal();
	if (Dir.IsNearlyZero()) return;
	const float Scale = FMath::Min((SW * 0.5f - 40.f) / FMath::Max(FMath::Abs(Dir.X), 0.01f), (SH * 0.5f - 60.f) / FMath::Max(FMath::Abs(Dir.Y), 0.01f));
	const FVector2D P(SW * 0.5f + Dir.X * Scale, SH * 0.5f + Dir.Y * Scale);
	const FVector2D Side(-Dir.Y, Dir.X);
	const FVector2D Base = P - Dir * 14.f;
	DrawLine(P.X, P.Y, Base.X + Side.X * 8.f, Base.Y + Side.Y * 8.f, Color, 3.f);
	DrawLine(P.X, P.Y, Base.X - Side.X * 8.f, Base.Y - Side.Y * 8.f, Color, 3.f);
	DrawLine(Base.X + Side.X * 8.f, Base.Y + Side.Y * 8.f, Base.X - Side.X * 8.f, Base.Y - Side.Y * 8.f, Color, 3.f);
	float TW = 0.f, TH = 0.f;
	GetTextSize(Label, TW, TH, GEngine->GetSmallFont(), 0.85f);
	const float LX = FMath::Clamp(float(Base.X - Dir.X * 10.f) - TW * 0.5f, 10.f, SW - TW - 10.f);
	const float LY = FMath::Clamp(float(Base.Y - Dir.Y * 18.f) - TH * 0.5f, 10.f, SH - TH - 30.f);
	const FBox2D Box(FVector2D(LX, LY), FVector2D(LX + TW, LY + TH));
	for (const FBox2D& Other : LabelRects) if (Other.Intersect(Box)) return; // arrow only, label would overlap
	LabelRects.Add(Box);
	DrawText(Label, Color, LX, LY, GEngine->GetSmallFont(), 0.85f);
}

void AAerosDemoHUD::DrawWorldMarkers(UAerosWeatherSubsystem* Sub)
{
	using namespace AerosDemoHudInternal;
	const FVector Viewer = Sub->GetViewerLocation();
	LabelRects.Reset();

	// Most important first (they get the free label space): tornadoes, hurricanes, volcanoes, quakes, then storms
	TArray<AAerosTornado*> Tornadoes = Sub->GetActiveTornadoes();
	Tornadoes.Sort([&Viewer](const AAerosTornado& A, const AAerosTornado& B) { return FVector::DistSquared2D(A.GetActorLocation(), Viewer) < FVector::DistSquared2D(B.GetActorLocation(), Viewer); });
	for (int32 K = 0; K < Tornadoes.Num(); ++K)
	{
		const AAerosTornado* Tor = Tornadoes[K];
		const float D = Km(Tor->GetActorLocation(), Viewer);
		if (D > 80.f) break;
		DrawMarker(Tor->GetActorLocation() + FVector(0, 0, 30000.0), FString::Printf(TEXT("TORNADE EF%d  %.1f km"), Tor->GetEFRating(), D), Danger, 12.f, K == 0 && D < 40.f);
	}
	for (const FAerosCycloneData& Cy : Sub->GetCyclones())
	{
		DrawMarker(FVector(Cy.Location.X, Cy.Location.Y, 300000.0), Cy.Category > 0 ? FString::Printf(TEXT("OURAGAN cat. %d  %.0f km  (%.0f km/h)"), Cy.Category, Km(Cy.Location, Viewer), Cy.MaxWind * 3.6f)
			: FString::Printf(TEXT("Temp\u00EAte tropicale  %.0f km"), Km(Cy.Location, Viewer)), Danger, 14.f, true);
	}
	for (const AAerosVolcano* V : Sub->GetVolcanoes())
	{
		const TCHAR* State = V->State == EAerosVolcanoState::Erupting ? TEXT("EN \u00C9RUPTION") : (V->State == EAerosVolcanoState::Unrest ? TEXT("se r\u00E9veille") :
			(V->State == EAerosVolcanoState::Waning ? TEXT("se calme") : TEXT("endormi")));
		const bool bActive = V->State != EAerosVolcanoState::Dormant;
		DrawMarker(V->GetVentLocation() + FVector(0, 0, 20000.0), FString::Printf(TEXT("Volcan %s  %.0f km"), State, Km(V->GetActorLocation(), Viewer)),
			bActive ? Danger : Dim, 9.f, bActive);
	}
	if (const UAerosHazardManager* Haz = Sub->GetHazardManager())
	{
		// Only fresh, felt earthquakes (aftershocks and old events would bury everything else)
		for (const AAerosEarthquake* Q : Haz->GetEarthquakes())
		{
			if (!Q || Q->Magnitude < 4.5f || Q->GetGameTimeSinceCreation() > 60.f) continue;
			DrawMarker(Q->GetActorLocation() + FVector(0, 0, 10000.0), FString::Printf(TEXT("\u00C9picentre M%.1f  %.0f km"), Q->Magnitude, Km(Q->GetActorLocation(), Viewer)), Warn, 9.f, Q->Magnitude >= 6.f);
		}
	}

	// Storms: the 5 nearest within 150 km, at cloud-base height above the updraft (no edge arrows)
	TArray<FAerosStormData> Storms = Sub->GetStorms();
	Storms.Sort([&Viewer](const FAerosStormData& A, const FAerosStormData& B) { return FVector::DistSquared2D(A.Location, Viewer) < FVector::DistSquared2D(B.Location, Viewer); });
	for (int32 K = 0; K < FMath::Min(Storms.Num(), 5); ++K)
	{
		const FAerosStormData& St = Storms[K];
		if (Km(St.Location, Viewer) > 150.f) break;
		FString Label = FString::Printf(TEXT("%s  %.0f km"), *StormTypeFr(St.Type), Km(St.Location, Viewer));
		if (St.MaxHail > 10.f) Label += FString::Printf(TEXT("  gr\u00EAle %.0f mm"), St.MaxHail);
		if (St.Mesocyclone > 0.5f) Label += TEXT("  rotation");
		DrawMarker(FVector(St.UpdraftLocation.X, St.UpdraftLocation.Y, 250000.0), Label, StormColor(St), St.Type == EAerosStormType::Supercell ? 10.f : 7.f, false);
	}
}

// =====================================================================
//  Warning banner, event feed, caption
// =====================================================================

void AAerosDemoHUD::DrawWarning(UAerosWeatherSubsystem* Sub, const FAerosWeatherSample& S)
{
	using namespace AerosDemoHudInternal;
	const FVector Viewer = Sub->GetViewerLocation();
	const float Shake = Sub->GetGroundShakingAt(Viewer);
	FString Warning;
	for (const AAerosTornado* Tor : Sub->GetActiveTornadoes())
	{
		const float D = Km(Tor->GetActorLocation(), Viewer);
		if (D < 30.f) { Warning = FString::Printf(TEXT("ALERTE TORNADE  EF%d  %.1f km %s"), Tor->GetEFRating(), D, *Compass(Bearing(Viewer, Tor->GetActorLocation()))); break; }
	}
	if (Warning.IsEmpty())
	{
		for (const FAerosCycloneData& Cy : Sub->GetCyclones())
		{
			const float D = Km(Cy.Location, Viewer);
			if (D < Cy.RadiusMaxWindKm * 6.f)
			{
				Warning = FString::Printf(TEXT("ALERTE %s  %.0f hPa  %.0f km/h  oeil \u00E0 %.0f km %s"), Cy.Category > 0 ? *FString::Printf(TEXT("OURAGAN CAT. %d"), Cy.Category) : TEXT("TEMP\u00CATE TROPICALE"),
					Cy.CentralPressure, Cy.MaxWind * 3.6f, D, *Compass(Bearing(Viewer, Cy.Location)));
				break;
			}
		}
	}
	if (Warning.IsEmpty() && Shake > 0.02f)
	{
		Warning = FString::Printf(TEXT("S\u00C9ISME  intensit\u00E9 %d (Mercalli)  %.2f g"), AAerosEarthquake::MercalliFromPGA(Shake), Shake);
	}
	if (Warning.IsEmpty())
	{
		FVector Coast(0.0), Offshore(0.0);
		if (S.Phenomenon == EAerosPhenomenon::Tsunami || (Sub->FindNearestOcean(Viewer, Coast, Offshore, 10.f) && FMath::Abs(Sub->GetWeatherAtLocation(Offshore).SeaLevelAnomaly) > 0.4f
			&& FVector::Dist2D(Coast, Viewer) < 6000000.0))
		{
			Warning = FString::Printf(TEXT("ALERTE TSUNAMI  mer %+.1f m  c\u00F4te \u00E0 %.0f km %s"), Sub->GetWeatherAtLocation(Offshore).SeaLevelAnomaly,
				Km(Coast, Viewer), *Compass(Bearing(Viewer, Coast)));
		}
	}
	if (Warning.IsEmpty())
	{
		for (const AAerosVolcano* V : Sub->GetVolcanoes())
		{
			const float D = Km(V->GetActorLocation(), Viewer);
			if (V->State == EAerosVolcanoState::Erupting && D < 80.f)
			{
				Warning = FString::Printf(TEXT("\u00C9RUPTION VOLCANIQUE  VEI %d  panache %.0f km  \u00E0 %.0f km %s"), V->CurrentVEI, V->GetPlumeHeight() * 0.001f, D, *Compass(Bearing(Viewer, V->GetActorLocation())));
				break;
			}
			if (V->State == EAerosVolcanoState::Unrest && D < 80.f) { Warning = TEXT("VOLCAN EN ALERTE : \u00C9RUPTION IMMINENTE"); break; }
		}
	}
	if (Warning.IsEmpty())
	{
		switch (S.Phenomenon)
		{
		case EAerosPhenomenon::FlashFlood: Warning = TEXT("CRUE \u00C9CLAIR : MONTE EN HAUTEUR"); break;
		case EAerosPhenomenon::Flood: Warning = TEXT("ALERTE INONDATION"); break;
		case EAerosPhenomenon::StormSurge: Warning = TEXT("ALERTE ONDE DE TEMP\u00CATE"); break;
		case EAerosPhenomenon::Blizzard: Warning = TEXT("ALERTE BLIZZARD : VOILE BLANC"); break;
		case EAerosPhenomenon::Sandstorm: Warning = TEXT("ALERTE TEMP\u00CATE DE SABLE"); break;
		case EAerosPhenomenon::VolcanicAshfall: Warning = TEXT("PLUIE DE CENDRES"); break;
		default: break;
		}
	}
	if (Warning.IsEmpty() && UAerosWeatherLibrary::IsSevere(S)) Warning = TEXT("TEMPS VIOLENT : METS-TOI \u00C0 L'ABRI");
	if (Warning.IsEmpty()) return;

	const float SW = Canvas->ClipX;
	float TW = 0.f, TH = 0.f;
	GetTextSize(Warning, TW, TH, GEngine->GetMediumFont());
	const float Blink = FMath::Fmod(GetWorld()->GetTimeSeconds(), 1.f) < 0.6f ? 1.f : 0.5f;
	DrawRect(FLinearColor(0.55f, 0.f, 0.f, 0.75f * Blink), SW * 0.5f - TW * 0.5f - 14.f, 14.f, TW + 28.f, TH + 10.f);
	DrawText(Warning, FLinearColor::White, SW * 0.5f - TW * 0.5f, 19.f, GEngine->GetMediumFont(), 1.f);
}

void AAerosDemoHUD::PushEvent(const FString& Text, const FLinearColor& Color)
{
	FFeedEntry E;
	E.Text = Text;
	E.Color = Color;
	E.Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	Feed.Add(E);
	if (Feed.Num() > 6) Feed.RemoveAt(0);
}

void AAerosDemoHUD::DrawFeed()
{
	const float Now = GetWorld()->GetTimeSeconds();
	Feed.RemoveAll([Now](const FFeedEntry& E) { return Now - E.Time > 25.f; });
	if (Feed.Num() == 0) return;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;
	const float W = 520.f, X = SW - W - 20.f;
	float Y = SH - 40.f - Feed.Num() * 17.f;
	DrawPanel(X - 8, Y - 22, W + 16, Feed.Num() * 17.f + 26.f);
	Line(X, Y - 18.f, TEXT("\u00C9V\u00C9NEMENTS"), AerosDemoHudInternal::Accent);
	for (const FFeedEntry& E : Feed)
	{
		const float Alpha = FMath::Clamp((25.f - (Now - E.Time)) / 5.f, 0.f, 1.f);
		FLinearColor C = E.Color;
		C.A = Alpha;
		DrawText(E.Text, C, X, Y, GEngine->GetSmallFont());
		Y += 17.f;
	}
}

void AAerosDemoHUD::DrawCaption()
{
	const AAerosDemoPawn* Pawn = Cast<AAerosDemoPawn>(GetOwningPawn());
	if (!Pawn || Pawn->TourCaption.IsEmpty()) return;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;
	const float W = FMath::Min(760.f, SW - 80.f);
	const float X = SW * 0.5f - W * 0.5f;
	const float Y = 62.f;
	DrawPanel(X - 14, Y - 10, W + 28, 104.f);
	DrawText(Pawn->TourTitle, AerosDemoHudInternal::Accent, X, Y, GEngine->GetMediumFont());
	Paragraph(X, Y + 28.f, W, Pawn->TourCaption, FLinearColor::White);
}

// =====================================================================
//  Event handlers -> feed
// =====================================================================

void AAerosDemoHUD::HandleStormFormed(const FAerosStormData& Storm)
{
	using namespace AerosDemoHudInternal;
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	const float D = Km(Storm.Location, Sub->GetViewerLocation());
	if (Storm.Type == EAerosStormType::SingleCell && D > 40.f) return; // too many small cells otherwise
	PushEvent(FString::Printf(TEXT("Un orage se forme \u00E0 %.0f km au %s"), D, *Compass(Bearing(Sub->GetViewerLocation(), Storm.Location))), FLinearColor::White);
}

void AAerosDemoHUD::HandleTornado(AAerosTornado* Tornado)
{
	using namespace AerosDemoHudInternal;
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub || !Tornado) return;
	PushEvent(FString::Printf(TEXT("Une tornade EF%d touche le sol \u00E0 %.0f km : la rotation de l'orage est descendue jusqu'au sol"),
		Tornado->GetEFRating(), Km(Tornado->GetActorLocation(), Sub->GetViewerLocation())), Danger);
}

void AAerosDemoHUD::HandleCyclone(const FAerosCycloneData& Cyclone)
{
	using namespace AerosDemoHudInternal;
	PushEvent(FString::Printf(TEXT("Une temp\u00EAte tropicale na\u00EEt sur la mer chaude (%.0f hPa)"), Cyclone.CentralPressure), Warn);
}

void AAerosDemoHUD::HandleEarthquake(const FAerosEarthquakeData& Quake)
{
	using namespace AerosDemoHudInternal;
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	const float D = Sub ? Km(Quake.Epicenter, Sub->GetViewerLocation()) : 0.f;
	PushEvent(FString::Printf(TEXT("S\u00E9isme M%.1f \u00E0 %.0f km, %.0f km de profondeur%s"), Quake.Magnitude, D, Quake.DepthKm,
		Quake.bTsunamigenic ? TEXT(" : le fond de la mer s'est soulev\u00E9") : TEXT("")), Warn);
}

void AAerosDemoHUD::HandleTsunami(FVector Source, float Magnitude)
{
	PushEvent(FString::Printf(TEXT("Tsunami : la vague part du large (M%.1f) et ralentit en grandissant pr\u00E8s de la c\u00F4te"), Magnitude), AerosDemoHudInternal::Danger);
}

void AAerosDemoHUD::HandleEruption(AAerosVolcano* Volcano, int32 VEI)
{
	PushEvent(FString::Printf(TEXT("\u00C9ruption VEI %d : colonne de cendres, coul\u00E9es de lave et bombes volcaniques"), VEI), AerosDemoHudInternal::Danger);
}

void AAerosDemoHUD::HandlePhenomenon(EAerosPhenomenon OldPhenomenon, EAerosPhenomenon NewPhenomenon)
{
	PushEvent(FString::Printf(TEXT("Ici : %s -> %s"), *AerosDemoHudInternal::PhenomenonFr(OldPhenomenon), *AerosDemoHudInternal::PhenomenonFr(NewPhenomenon)), AerosDemoHudInternal::Dim);
}

// =====================================================================
//  Radar + help
// =====================================================================

void AAerosDemoHUD::DrawRadar(UAerosWeatherSubsystem* Sub, float X, float Y, float Size)
{
	AAerosWeatherRadar* Radar = nullptr;
	for (TActorIterator<AAerosWeatherRadar> It(GetWorld()); It; ++It) { Radar = *It; break; }
	DrawPanel(X - 8, Y - 6, Size + 16, Size + 46);
	DrawRect(FLinearColor(0.01f, 0.02f, 0.03f, 0.9f), X, Y, Size, Size);
	if (!Radar || !Radar->GetRadarTexture()) return;

	DrawTexture(Radar->GetRadarTexture(), X, Y, Size, Size, 0, 0, 1, 1, FLinearColor::White, BLEND_Translucent);

	const float CX = X + Size * 0.5f, CY = Y + Size * 0.5f, R = Size * 0.5f;
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
	const float Az = FMath::DegreesToRadians(Radar->GetBeamAzimuth());
	DrawLine(CX, CY, CX + FMath::Sin(Az) * R, CY - FMath::Cos(Az) * R, FLinearColor(0.3f, 1.f, 0.5f, 0.8f), 2.f);
	DrawRect(FLinearColor::White, CX - 2, CY - 2, 4, 4);
	if (const APlayerController* PC = GetOwningPlayerController())
	{
		// View cone: what the camera sees
		const float Yaw = FMath::DegreesToRadians(AerosDemoHudInternal::Bearing(FVector::ZeroVector, PC->GetControlRotation().Vector()));
		for (const float Off : { -0.5f, 0.5f })
		{
			DrawLine(CX, CY, CX + FMath::Sin(Yaw + Off) * R * 0.35f, CY - FMath::Cos(Yaw + Off) * R * 0.35f, FLinearColor(1, 1, 1, 0.7f), 1.5f);
		}
	}
	for (const FAerosStormData& St : Sub->GetStorms())
	{
		FVector2D UV;
		if (St.Mesocyclone < 0.4f || !Radar->WorldToRadarUV(St.UpdraftLocation, UV)) continue;
		const float PX = X + float(UV.X) * Size, PY = Y + float(UV.Y) * Size;
		DrawLine(PX - 6, PY - 6, PX + 6, PY + 6, FLinearColor(1, 1, 0));
		DrawLine(PX - 6, PY + 6, PX + 6, PY - 6, FLinearColor(1, 1, 0));
	}
	DrawText(TEXT("N"), FLinearColor::White, CX - 4, Y + 2, GEngine->GetSmallFont());

	// Plain legend
	const float LY = Y + Size + 6.f;
	DrawText(FString::Printf(TEXT("RADAR %.0f km (R change)"), Radar->RangeKm), AerosDemoHudInternal::Dim, X, LY, GEngine->GetSmallFont());
	const float LX = X + Size - 140.f;
	for (int32 K = 0; K < 14; ++K)
	{
		DrawRect(AAerosWeatherRadar::ReflectivityColor(5.f + K * 5.f), LX + K * 10.f, LY + 3.f, 10.f, 8.f);
	}
	DrawText(TEXT("faible"), AerosDemoHudInternal::Dim, LX, LY + 13.f, GEngine->GetSmallFont(), 0.8f);
	DrawText(TEXT("gr\u00EAle"), AerosDemoHudInternal::Dim, LX + 110.f, LY + 13.f, GEngine->GetSmallFont(), 0.8f);
	DrawText(TEXT("X jaune = orage qui tourne"), AerosDemoHudInternal::Dim, X, LY + 15.f, GEngine->GetSmallFont(), 0.8f);
}

void AAerosDemoHUD::DrawHelp(float X, float Y)
{
	DrawPanel(X - 8, Y - 6, 660, 540);
	const TCHAR* Lines[] = {
		TEXT("AEROS WEATHER DEMO - COMMANDES (clavier AZERTY)"),
		TEXT(""),
		TEXT("Souris regarder, Z Q S D (ou fl\u00E8ches) bouger, Espace/E monter, A/C descendre"),
		TEXT("Shift x10, Ctrl x100 vitesse"),
		TEXT("F1 / Tab   affichage : simple / expert / radar seul / cach\u00E9 / aide"),
		TEXT("F2         visite guid\u00E9e (montre chaque ph\u00E9nom\u00E8ne et l'explique)"),
		TEXT(""),
		TEXT("SC\u00C9NARIOS  &  Climat (saisons)      \u00E9  Beau temps          \"  Orages d'\u00E9t\u00E9"),
		TEXT("           '  Supercellules         (  Temp\u00EAte de neige    -  Saison tropicale"),
		TEXT("           \u00E8  Pluie frontale        _  Blizzard            \u00E7  D\u00E9sert / temp\u00EAtes de sable"),
		TEXT("           \u00E0  Mousson / inondations"),
		TEXT(""),
		TEXT("M\u00C9T\u00C9O       B bulle chaude (orage)   T tornade   H ouragan (+ onde de temp\u00EAte)"),
		TEXT("D\u00C9SASTRES   G s\u00E9isme M6.6              Y s\u00E9isme sous-marin M9 + tsunami"),
		TEXT("            U \u00E9ruption du volcan (VEI 3, 4, 5, 6, 2...)   O d\u00E9luge 250 mm (crue)"),
		TEXT("            L rafale descendante (haboob sur sol sec)"),
		TEXT("M\u00C9T\u00C9O R\u00C9ELLE  W : Paris, Oklahoma City, Miami, Tokyo, Reykjavik... (Open-Meteo, temps r\u00E9el)"),
		TEXT("VOYAGE      F orage le plus proche   V volcan   I c\u00F4te (face \u00E0 la mer)"),
		TEXT("TEMPS       = / ) (ou pav\u00E9 + / -) vitesse x2 / \u00F72    P pause"),
		TEXT("            N / M  -1 / +1 heure      J / K  -1 / +1 mois (saisons)"),
		TEXT("AFFICHAGE   R radar : r\u00E9flectivit\u00E9 / vitesse / type / risques    X debug 3D (vent, zones)"),
		TEXT(""),
		TEXT("Les losanges \u00E0 l'\u00E9cran montrent les orages, tornades, ouragans, volcans et s\u00E9ismes."),
		TEXT("Hors de l'\u00E9cran, une fl\u00E8che au bord indique leur direction."),
		TEXT(""),
		TEXT("Console : aeros.Scenario 0-10, aeros.TimeScale X, aeros.Time jour heure, aeros.Quake M km,"),
		TEXT("          aeros.Tsunami M, aeros.Flood mm, aeros.Haboob, aeros.Eruption VEI, aeros.Live lat lon nom"),
	};
	for (const TCHAR* L : Lines) Y = Line(X, Y, L, FLinearColor::White);
}
