// Copyright Aeros. All Rights Reserved.

#include "AerosChaseHUD.h"
#include "AerosStormNames.h"
#include "AerosDebugUpdates.h"
#include "AerosChasePawn.h"
#include "AerosChaseRules.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherRadar.h"
#include "AerosTornado.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

namespace AerosChaseHudInternal
{
	const FLinearColor Ink(0.92f, 0.95f, 1.f, 1.f);
	const FLinearColor Dim(0.65f, 0.72f, 0.82f, 1.f);
	const FLinearColor Accent(1.f, 0.78f, 0.3f, 1.f);      // amber: the chaser's colour
	const FLinearColor Danger(1.f, 0.35f, 0.3f, 1.f);
	const FLinearColor Good(0.45f, 0.95f, 0.55f, 1.f);
	const FLinearColor Storm(1.f, 0.6f, 0.25f, 1.f);

	FString Clock(float Hour)
	{
		const int32 H = FMath::FloorToInt(Hour);
		const int32 M = FMath::FloorToInt((Hour - H) * 60.f);
		return FString::Printf(TEXT("%02d:%02d"), H, M);
	}

	const TCHAR* RankName(int32 Index)
	{
		static const TCHAR* Names[5] = { TEXT("D\u00E9butant"), TEXT("Observateur"), TEXT("Chasseur"), TEXT("Chasseur confirm\u00E9"), TEXT("Chasseur l\u00E9gendaire") };
		return Names[FMath::Clamp(Index, 0, 4)];
	}
}

// ---------------------------------------------------------------- helpers

void AAerosChaseHUD::Panel(float X, float Y, float W, float H, float Alpha)
{
	DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, Alpha), X, Y, W, H);
	DrawRect(FLinearColor(1.f, 0.78f, 0.3f, 0.8f * Alpha), X, Y, 3.f, H);
}

void AAerosChaseHUD::Text(const FString& S, float X, float Y, const FLinearColor& C, float Scale, bool bCentre, UFont* Font)
{
	UFont* F = Font ? Font : GEngine->GetMediumFont();
	if (bCentre)
	{
		float TW = 0.f, TH = 0.f;
		GetTextSize(S, TW, TH, F, Scale);
		X -= TW * 0.5f;
	}
	DrawText(S, FLinearColor(0, 0, 0, 0.6f * C.A), X + 1.5f, Y + 1.5f, F, Scale);
	DrawText(S, C, X, Y, F, Scale);
}

float AAerosChaseHUD::Wrapped(const FString& S, float X, float Y, float W, const FLinearColor& C, float Scale)
{
	TArray<FString> Words;
	S.ParseIntoArray(Words, TEXT(" "));
	FString Line;
	UFont* F = GEngine->GetMediumFont();
	for (const FString& Word : Words)
	{
		const FString Try = Line.IsEmpty() ? Word : Line + TEXT(" ") + Word;
		float TW = 0.f, TH = 0.f;
		GetTextSize(Try, TW, TH, F, Scale);
		if (TW > W && !Line.IsEmpty())
		{
			Text(Line, X, Y, C, Scale);
			Y += TH + 4.f;
			Line = Word;
		}
		else
		{
			Line = Try;
		}
	}
	if (!Line.IsEmpty())
	{
		float TW = 0.f, TH = 0.f;
		GetTextSize(Line, TW, TH, F, Scale);
		Text(Line, X, Y, C, Scale);
		Y += TH + 4.f;
	}
	return Y;
}

void AAerosChaseHUD::Bar(float X, float Y, float W, float H, float Value, const FLinearColor& C)
{
	DrawRect(FLinearColor(1, 1, 1, 0.12f), X, Y, W, H);
	DrawRect(C, X, Y, W * FMath::Clamp(Value, 0.f, 1.f), H);
}

void AAerosChaseHUD::Frame(float X, float Y, float W, float H, const FLinearColor& C, float Corner)
{
	const float T = 2.f;
	// Four corner brackets
	DrawRect(C, X, Y, Corner, T);             DrawRect(C, X, Y, T, Corner);
	DrawRect(C, X + W - Corner, Y, Corner, T); DrawRect(C, X + W - T, Y, T, Corner);
	DrawRect(C, X, Y + H - T, Corner, T);      DrawRect(C, X, Y + H - Corner, T, Corner);
	DrawRect(C, X + W - Corner, Y + H - T, Corner, T); DrawRect(C, X + W - T, Y + H - Corner, T, Corner);
}

// ---------------------------------------------------------------- main

void AAerosChaseHUD::DrawHUD()
{
	Super::DrawHUD();
	APlayerController* PC = GetOwningPlayerController();
	AAerosChasePawn* P = PC ? Cast<AAerosChasePawn>(PC->GetPawn()) : nullptr;
	if (!P || !Canvas || P->PhotoHideFrames > 0) return;   // a clean photo is being taken
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	using namespace AerosChaseHudInternal;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;

	switch (P->State)
	{
	case EAerosChaseState::Menu:
		DrawMenu(P);
		break;
	case EAerosChaseState::Briefing:
		DrawBriefing(P);
		break;
	case EAerosChaseState::Driving:
		if (Sub) DrawDriving(P, Sub);
		break;
	case EAerosChaseState::Destroyed:
		DrawRect(FLinearColor(0.35f, 0.02f, 0.02f, 0.55f), 0, 0, SW, SH);
		Text(TEXT("V\u00C9HICULE D\u00C9TRUIT"), SW * 0.5f, SH * 0.4f, Danger, 2.4f, true, GEngine->GetLargeFont());
		Text(FString::Printf(TEXT("P\u00E9nalit\u00E9 : -%d pts. Un nouveau v\u00E9hicule arrive..."), P->LastPenalty), SW * 0.5f, SH * 0.4f + 60.f, Ink, 1.2f, true);
		break;
	case EAerosChaseState::Paused:
		if (Sub) DrawDriving(P, Sub);
		DrawRect(FLinearColor(0, 0, 0, 0.55f), 0, 0, SW, SH);
		Text(TEXT("PAUSE"), SW * 0.5f, SH * 0.38f, Accent, 2.4f, true, GEngine->GetLargeFont());
		Text(TEXT("\u00C9chap : reprendre      Entr\u00E9e : finir la journ\u00E9e      Retour arri\u00E8re : menu principal"), SW * 0.5f, SH * 0.38f + 64.f, Ink, 1.1f, true);
		break;
	case EAerosChaseState::Summary:
		DrawSummary(P);
		break;
	}
	if (P->ShutterFlash > 0.f) DrawRect(FLinearColor(1, 1, 1, P->ShutterFlash * 0.8f), 0, 0, SW, SH);
}

// ---------------------------------------------------------------- menu, briefing, summary

void AAerosChaseHUD::DrawMenu(AAerosChasePawn* P)
{
	using namespace AerosChaseHudInternal;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;
	DrawRect(FLinearColor(0.01f, 0.02f, 0.04f, 0.45f), 0, 0, SW, SH);
	const float X = SW * 0.08f;
	float Y = SH * 0.22f;
	Text(TEXT("AEROS"), X, Y, Accent, 3.2f, false, GEngine->GetLargeFont());
	Y += 90.f;
	Text(TEXT("M\u00E9t\u00E9o et catastrophes naturelles simul\u00E9es"), X, Y, Dim, 1.2f);
	Y += 70.f;
	const TCHAR* Items[3] = { TEXT("Chasseur d'orages"), TEXT("D\u00E9mo technique (vol libre)"), TEXT("Quitter") };
	const TCHAR* Help[3] = {
		TEXT("Une apr\u00E8s-midi de supercellules : trouve les orages, photographie les tornades, ne te fais pas prendre."),
		TEXT("Le simulateur \u00E0 nu : vol libre, panneaux d'experts, sc\u00E9narios et catastrophes \u00E0 la demande."),
		TEXT("") };
	for (int32 K = 0; K < 3; ++K)
	{
		const bool bSel = P->MenuIndex == K;
		if (bSel) DrawRect(FLinearColor(1.f, 0.78f, 0.3f, 0.18f), X - 14.f, Y - 6.f, 560.f, 46.f);
		Text(bSel ? FString(TEXT("> ")) + Items[K] : FString(TEXT("  ")) + Items[K], X, Y, bSel ? Accent : Ink, 1.6f);
		Y += 56.f;
	}
	Y += 20.f;
	Wrapped(Help[P->MenuIndex], X, Y, 560.f, Dim, 1.f);
	Text(TEXT("Z / S ou fl\u00E8ches : choisir      Entr\u00E9e : valider"), X, SH - 60.f, Dim, 1.f);
}

void AAerosChaseHUD::DrawBriefing(AAerosChasePawn* P)
{
	using namespace AerosChaseHudInternal;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;
	DrawRect(FLinearColor(0.01f, 0.02f, 0.04f, 0.5f), 0, 0, SW, SH);
	const float W = FMath::Min(760.f, SW - 80.f), X = (SW - W) * 0.5f;
	float Y = SH * 0.24f;
	Panel(X - 24.f, Y - 24.f, W + 48.f, 400.f, 0.82f);
	Text(TEXT("BRIEFING M\u00C9T\u00C9O"), X, Y, Accent, 1.8f, false, GEngine->GetLargeFont());
	Y += 60.f;
	Y = Wrapped(P->BriefingText, X, Y, W, Ink, 1.1f) + 16.f;
	Text(TEXT("Points : tornade (plus elle est forte et proche, plus \u00E7a paie), nuage-mur, \u00E9clair au bon moment, supercellule enti\u00E8re vue de loin, travers\u00E9e de gr\u00EAle."), X, Y, Dim, 0.9f);
	Y += 26.f;
	Text(TEXT("Dangers : vents de tornade, gr\u00EAle > 2 cm, foudre \u00E0 moins de 300 m, routes inond\u00E9es."), X, Y, Dim, 0.9f);
	Y += 26.f;
	Text(TEXT("Trop loin ? T = trajet rapide \u00E0 4 km de la tornade (sinon de la supercellule) la plus proche, l'horloge avance de 10 minutes."), X, Y, Dim, 0.9f);
	Y += 46.f;
	Text(TEXT("Les orages se forment..."), X, Y, Ink, 1.f);
	Bar(X, Y + 30.f, W, 10.f, P->BriefingProgress, Accent);
	Text(FString::Printf(TEXT("%s"), *Clock(P->GetHour())), X + W, Y, Dim, 1.f);
	if (P->bBriefingReady)
	{
		Text(TEXT("Une supercellule tourne d\u00E9j\u00E0.  Entr\u00E9e : partir maintenant"), X, Y + 60.f, Accent, 1.f);
	}
}

void AAerosChaseHUD::DrawSummary(AAerosChasePawn* P)
{
	using namespace AerosChaseHudInternal;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;
	DrawRect(FLinearColor(0.01f, 0.02f, 0.04f, 0.6f), 0, 0, SW, SH);
	const float W = FMath::Min(760.f, SW - 80.f), X = (SW - W) * 0.5f;
	float Y = SH * 0.12f;
	const float PanelBottom = Y - 24.f + SH * 0.78f;
	Panel(X - 24.f, Y - 24.f, W + 48.f, SH * 0.78f, 0.85f);
	Text(TEXT("FIN DE LA JOURN\u00C9E"), X, Y, Accent, 1.8f, false, GEngine->GetLargeFont());
	if (!P->EndReason.IsEmpty()) Text(P->EndReason, X + 330.f, Y + 10.f, Dim, 1.1f);
	Y += 64.f;
	Text(FString::Printf(TEXT("%d points"), P->Score), X, Y, Ink, 2.2f, false, GEngine->GetLargeFont());
	Y += 58.f;
	Text(FString::Printf(TEXT("Rang : %s"), *FString(RankName(AerosChase::RankIndex(P->Score)))), X, Y, Accent, 1.3f, false);
	Y += 42.f;
	Text(FString::Printf(TEXT("Photos retenues : %d      C\u0153urs de gr\u00EAle : %d      V\u00E9hicules d\u00E9truits : %d"), P->Album.Num(), P->HailIntercepts, P->Destroyed), X, Y, Dim, 1.f);
	Y += 40.f;
	TArray<FAerosChasePhoto> Sorted = P->Album;
	Sorted.Sort([](const FAerosChasePhoto& A, const FAerosChasePhoto& B) { return A.Points > B.Points; });
	if (Sorted.Num() == 0) Text(TEXT("Aucune photo cette fois. La prochaine fois, approche-toi !"), X, Y, Ink, 1.f);
	for (int32 K = 0; K < Sorted.Num() && K < 8; ++K)
	{
		Text(FString::Printf(TEXT("%s   %s"), *Clock(Sorted[K].Hour), *Sorted[K].Label), X, Y, Ink, 1.f);
		Text(FString::Printf(TEXT("%d pts"), Sorted[K].Points), X + W - 90.f, Y, Accent, 1.f);
		Y += 30.f;
	}
	Text(TEXT("Les photos sont dans Saved/Screenshots/.../AerosChase"), X, PanelBottom - 92.f, Dim, 0.85f);
	const TCHAR* Items[2] = { TEXT("Rejouer"), TEXT("Menu principal") };
	for (int32 K = 0; K < 2; ++K)
	{
		const bool bSel = P->SummaryIndex == K;
		Text(bSel ? FString(TEXT("> ")) + Items[K] : FString(TEXT("  ")) + Items[K], X + K * 260.f, PanelBottom - 58.f, bSel ? Accent : Ink, 1.3f);
	}
}

// ---------------------------------------------------------------- driving

void AAerosChaseHUD::DrawDriving(AAerosChasePawn* P, UAerosWeatherSubsystem* Sub)
{
	using namespace AerosChaseHudInternal;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;

	// Damage vignette and low health pulse
	const float Hurt = FMath::Max(P->DamageFlash, P->Health < 30.f ? 0.25f + 0.15f * FMath::Sin(GetWorld()->GetTimeSeconds() * 6.f) : 0.f);
	if (Hurt > 0.01f)
	{
		const float B = 90.f;
		const FLinearColor R(0.8f, 0.05f, 0.05f, 0.35f * Hurt);
		DrawRect(R, 0, 0, SW, B); DrawRect(R, 0, SH - B, SW, B); DrawRect(R, 0, B, B, SH - 2 * B); DrawRect(R, SW - B, B, B, SH - 2 * B);
	}

	DrawWorldMarkers(P, Sub);
	if (P->bZoom) DrawViewfinder(P);
	else DrawRect(FLinearColor(1, 1, 1, 0.6f), SW * 0.5f - 2.f, SH * 0.5f - 2.f, 4.f, 4.f);

	// Clock and score (top left)
	Panel(20.f, 20.f, 300.f, 112.f);
	const float Hour = P->GetHour();
	Text(Clock(Hour), 34.f, 28.f, Ink, 2.f, false, GEngine->GetLargeFont());
	const float Left = FMath::Max(0.f, AAerosChasePawn::DayEndHour - Hour);
	Text(FString::Printf(TEXT("coucher du soleil dans %d h %02d"), FMath::FloorToInt(Left), FMath::FloorToInt(FMath::Frac(Left) * 60.f)), 34.f, 82.f, Dim, 0.9f);
	Text(FString::Printf(TEXT("%d pts"), P->Score), 300.f, 34.f, Accent, 1.5f);
	Text(AerosChaseHudInternal::RankName(AerosChase::RankIndex(P->Score)), 300.f, 64.f, Dim, 0.85f);

	// Compass + alerts (top centre)
	DrawCompass(P, Sub, SW * 0.5f, 22.f, FMath::Min(640.f, SW * 0.42f));
	float AY = 74.f;
	for (const FString& A : P->Alerts)
	{
		const bool bBad = A.StartsWith(TEXT("DANGER")) || A.Contains(TEXT("touch\u00E9"));
		Text(A, SW * 0.5f, AY, bBad ? Danger : Accent, 1.1f, true);
		AY += 30.f;
	}

	// Radar (top right)
	const float RS = FMath::Min(SH * 0.3f, 260.f);
	DrawRadar(P, SW - RS - 24.f, 24.f, RS);

	// Speed and vehicle (bottom left)
	Panel(20.f, SH - 132.f, 300.f, 112.f);
	Text(FString::Printf(TEXT("%.0f"), FMath::Abs(P->SpeedMs) * 3.6f), 34.f, SH - 128.f, Ink, 2.4f, false, GEngine->GetLargeFont());
	Text(TEXT("km/h"), 140.f, SH - 100.f, Dim, 1.f);
	Text(TEXT("V\u00E9hicule"), 34.f, SH - 62.f, Dim, 0.9f);
	const FLinearColor HC = P->Health > 60.f ? Good : (P->Health > 30.f ? Accent : Danger);
	Bar(120.f, SH - 56.f, 180.f, 12.f, P->Health / 100.f, HC);

	// Photo card (bottom centre)
	if (!P->LastPhotoText.IsEmpty() && P->LastPhotoTime < 5.f)
	{
		const float A = FMath::Clamp(5.f - P->LastPhotoTime, 0.f, 1.f);
		const bool bGain = P->LastPhotoText.StartsWith(TEXT("+"));
		float TW = 0.f, TH = 0.f;
		GetTextSize(P->LastPhotoText, TW, TH, GEngine->GetMediumFont(), 1.3f);
		DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.75f * A), SW * 0.5f - TW * 0.5f - 18.f, SH - 190.f, TW + 36.f, TH + 18.f);
		Text(P->LastPhotoText, SW * 0.5f, SH - 182.f, bGain ? FLinearColor(Accent.R, Accent.G, Accent.B, A) : FLinearColor(Ink.R, Ink.G, Ink.B, A), 1.3f, true);
	}
	if (P->ToastTime > 0.f)
	{
		Text(P->Toast, SW * 0.5f, SH - 120.f, FLinearColor(Ink.R, Ink.G, Ink.B, FMath::Min(P->ToastTime, 1.f)), 1.f, true);
	}
	// Controls reminder for the first seconds
	if (P->StateTime < 25.f)
	{
		Text(TEXT("Z/S avancer-freiner   Q/D tourner   souris regarder   clic droit zoom   clic gauche ou Espace photo   T trajet rapide (+10 min)   \u00C9chap pause"),
			SW * 0.5f, SH - 40.f, FLinearColor(Dim.R, Dim.G, Dim.B, FMath::Clamp(25.f - P->StateTime, 0.f, 1.f)), 0.9f, true);
	}

	// aeros.DebugUpdates 1 / -AerosDebugUpdates: when the weather on screen is next updated
	AerosDebugUpdatesInternal::Draw(this, Canvas, Sub, 30.f, SH - 140.f);
}

void AAerosChaseHUD::DrawViewfinder(AAerosChasePawn* P)
{
	using namespace AerosChaseHudInternal;
	const float SW = Canvas->ClipX, SH = Canvas->ClipY;
	const float M = 70.f;
	// Central quarter: where the subject is worth the most
	const float QW = (SW - 2.f * M) * 0.25f, QH = (SH - 2.f * M) * 0.25f;
	Frame(SW * 0.5f - QW * 0.5f, SH * 0.5f - QH * 0.5f, QW, QH, FLinearColor(1.f, 0.78f, 0.3f, 0.6f), 16.f);
	DrawRect(FLinearColor(1, 1, 1, 0.8f), SW * 0.5f - 12.f, SH * 0.5f - 1.f, 24.f, 2.f);
	DrawRect(FLinearColor(1, 1, 1, 0.8f), SW * 0.5f - 1.f, SH * 0.5f - 12.f, 2.f, 24.f);
	const FString Est = P->Preview.Points > 0 ? FString::Printf(TEXT("%s   ~%d pts"), *P->Preview.Label, P->Preview.Points)
		: (P->Preview.Label.IsEmpty() ? FString(TEXT("Rien d'int\u00E9ressant dans le cadre")) : P->Preview.Label);
	Text(Est, SW * 0.5f, SH - M - 40.f, P->Preview.Points > 0 ? Accent : Dim, 1.1f, true);
}

void AAerosChaseHUD::DrawCompass(AAerosChasePawn* P, UAerosWeatherSubsystem* Sub, float CX, float Y, float W)
{
	using namespace AerosChaseHudInternal;
	APlayerController* PC = GetOwningPlayerController();
	const float ViewYaw = PC ? PC->GetControlRotation().Yaw : P->GetHeadingDeg();
	// Unreal yaw 0 = east (+X); bearing 0 = north: bearing = yaw + 90
	const float ViewBearing = FMath::Fmod(ViewYaw + 90.f + 360.f, 360.f);
	const float Span = 120.f;
	Panel(CX - W * 0.5f, Y, W, 40.f, 0.55f);
	auto ToX = [&](float Bearing, float& OutX) -> bool
	{
		const float D = FRotator::NormalizeAxis(Bearing - ViewBearing);
		if (FMath::Abs(D) > Span * 0.5f) return false;
		OutX = CX + D / Span * W;
		return true;
	};
	const TCHAR* Cards[8] = { TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"), TEXT("S"), TEXT("SO"), TEXT("O"), TEXT("NO") };
	for (int32 K = 0; K < 8; ++K)
	{
		float X;
		if (ToX(K * 45.f, X)) Text(Cards[K], X, Y + 8.f, K % 2 == 0 ? Ink : Dim, 0.9f, true);
	}
	for (int32 B = 0; B < 360; B += 15)
	{
		float X;
		if (B % 45 != 0 && ToX(float(B), X)) DrawRect(FLinearColor(1, 1, 1, 0.35f), X, Y + 30.f, 1.f, 6.f);
	}
	// Storms (orange) and tornadoes (red) on the strip
	for (const FAerosStormData& S : Sub->GetStorms())
	{
		if (S.Type != EAerosStormType::Supercell) continue;
		float X;
		if (ToX(P->BearingTo(S.UpdraftLocation), X)) DrawRect(Storm, X - 3.f, Y + 2.f, 6.f, 6.f);
	}
	for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It)
	{
		float X;
		if (It->GetStrength() > 0.1f && ToX(P->BearingTo(It->GetActorLocation()), X)) DrawRect(Danger, X - 4.f, Y + 1.f, 8.f, 8.f);
	}
	DrawRect(Accent, CX - 1.f, Y + 28.f, 2.f, 12.f);
}

void AAerosChaseHUD::DrawWorldMarkers(AAerosChasePawn* P, UAerosWeatherSubsystem* Sub)
{
	using namespace AerosChaseHudInternal;
	auto Mark = [&](const FVector& World, const FString& Label, const FLinearColor& C)
	{
		const FVector S = Project(World);
		if (S.Z <= 0.f) return;   // behind the camera
		if (S.X < 0 || S.Y < 0 || S.X > Canvas->ClipX || S.Y > Canvas->ClipY) return;
		DrawRect(C, S.X - 1.f, S.Y - 14.f, 2.f, 10.f);
		Text(Label, S.X, S.Y - 38.f, C, 0.85f, true);
	};
	for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It)
	{
		if (It->GetStrength() < 0.1f) continue;
		const float D = float(FVector::Dist2D(It->GetActorLocation(), P->GetActorLocation())) * 1e-5f;
		if (D > 40.f) continue;
		const int32 Trend = It->GetEFTrend();
		const TCHAR* How = Trend > 0 ? TEXT(" (se renforce)") : (Trend < 0 ? TEXT(" (faiblit)") : TEXT(""));
		Mark(It->GetActorLocation() + FVector(0, 0, 60000.0), FString::Printf(TEXT("TORNADE EF%d%s  %.1f km"), It->GetEFRating(), How, D), Danger);
	}
	for (const FAerosStormData& S : Sub->GetStorms())
	{
		if (S.Type != EAerosStormType::Supercell) continue;
		const float D = float(FVector::Dist2D(S.UpdraftLocation, P->GetActorLocation())) * 1e-5f;
		if (D > 90.f || D < 4.f) continue;
		Mark(S.UpdraftLocation + FVector(0, 0, 900000.0), FString::Printf(TEXT("%s  %.0f km"), *AerosStormNames::LabelFr(S), D), Storm);
	}
}

void AAerosChaseHUD::DrawRadar(AAerosChasePawn* P, float X, float Y, float Size)
{
	using namespace AerosChaseHudInternal;
	Panel(X - 8.f, Y - 8.f, Size + 16.f, Size + 40.f, 0.65f);
	DrawRect(FLinearColor(0.01f, 0.02f, 0.03f, 0.9f), X, Y, Size, Size);
	AAerosWeatherRadar* Radar = nullptr;
	for (TActorIterator<AAerosWeatherRadar> It(GetWorld()); It; ++It) { Radar = *It; break; }
	if (Radar && Radar->GetRadarTexture())
	{
		DrawTexture(Radar->GetRadarTexture(), X, Y, Size, Size, 0, 0, 1, 1, FLinearColor::White, BLEND_Translucent);
	}
	const float CX = X + Size * 0.5f, CY = Y + Size * 0.5f, R = Size * 0.5f;
	for (int32 Ring = 1; Ring <= 2; ++Ring)
	{
		const float RR = R * Ring / 2.f;
		FVector2D Prev(CX + RR, CY);
		for (int32 K = 1; K <= 40; ++K)
		{
			const float A = 2.f * PI * K / 40.f;
			const FVector2D Q(CX + FMath::Cos(A) * RR, CY + FMath::Sin(A) * RR);
			DrawLine(Prev.X, Prev.Y, Q.X, Q.Y, FLinearColor(0.2f, 0.35f, 0.3f, 0.6f));
			Prev = Q;
		}
	}
	// The vehicle and where the camera looks
	if (const APlayerController* PC = GetOwningPlayerController())
	{
		const float B = FMath::DegreesToRadians(PC->GetControlRotation().Yaw + 90.f);
		DrawLine(CX, CY, CX + FMath::Sin(B) * R * 0.35f, CY - FMath::Cos(B) * R * 0.35f, FLinearColor(1.f, 0.78f, 0.3f, 0.9f), 2.f);
	}
	DrawRect(Accent, CX - 3.f, CY - 3.f, 6.f, 6.f);
	Text(FString::Printf(TEXT("RADAR  %.0f km"), Radar ? Radar->RangeKm : 160.f), X, Y + Size + 8.f, Dim, 0.8f);
}
