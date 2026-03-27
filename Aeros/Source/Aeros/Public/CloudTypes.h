// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CloudTypes.generated.h"

// ============================================================
// ECloudState — Machine à états d'un nuage individuel
//
// Transitions pilotées par FWeatherCell (calculées dans UCloudManager) :
//
//   Humidity < 0.10                           → Clear
//   Humidity ≥ 0.10, CAPE <  500              → Cumulus
//   Humidity ≥ 0.55, CAPE  500-2000           → Cumulonimbus
//   Humidity ≥ 0.80, CAPE > 2000, Precip > 0  → StormActive
//   UpdraftVelocity > 50 m/s, CAPE > 2500     → Tornado
// ============================================================
UENUM(BlueprintType)
enum class ECloudState : uint8
{
	Clear         UMETA(DisplayName = "Clear"),
	Cumulus       UMETA(DisplayName = "Cumulus"),
	Cumulonimbus  UMETA(DisplayName = "Cumulonimbus"),
	StormActive   UMETA(DisplayName = "Storm Active"),
	Tornado       UMETA(DisplayName = "Tornado"),
};

// ============================================================
// ECloudLOD — Niveau de détail du rendu
//
// LOD0 : nuage proche  → 5 couches sprites croisées + VFX complets
// LOD1 : nuage moyen   → 3 couches sprites + VFX réduits
// LOD2 : nuage lointain → 1 sprite impostor, pas de VFX
// Hidden: hors portée  → tout désactivé
// ============================================================
UENUM(BlueprintType)
enum class ECloudLOD : uint8
{
	LOD0_Full      UMETA(DisplayName = "LOD0 - Full"),
	LOD1_Medium    UMETA(DisplayName = "LOD1 - Medium"),
	LOD2_Impostor  UMETA(DisplayName = "LOD2 - Impostor"),
	Hidden         UMETA(DisplayName = "Hidden"),
};

// ============================================================
// FCloudVisualParams
// Données pré-calculées transmises par UCloudManager à ACloudActor.
// ACloudActor ne connaît pas FWeatherCell : il ne fait que du rendu.
// ============================================================
USTRUCT(BlueprintType)
struct FCloudVisualParams
{
	GENERATED_BODY()

	UPROPERTY() ECloudState State = ECloudState::Clear;
	UPROPERTY() ECloudLOD   LOD = ECloudLOD::Hidden;

	// Position monde du pied du nuage (en cm)
	UPROPERTY() FVector     WorldCenter = FVector::ZeroVector;

	// ── Paramètres matériaux ──────────────────────────────────
	// [0,1] Opacité globale          ← Humidity
	UPROPERTY() float       Opacity = 0.f;
	// [0,1] Noirceur de la base      ← CAPE normalisé
	UPROPERTY() float       Darkness = 0.f;
	// [1,4] Hauteur du nuage         ← UpdraftVelocity
	UPROPERTY() float       VerticalScale = 1.f;
	// [0,1] Couverture latérale      ← Humidity (nuage "bouffi")
	UPROPERTY() float       HorizontalPuff = 1.f;

	// ── Paramètres VFX Niagara ────────────────────────────────
	// [0,1] Intensité de pluie       ← PrecipitationIntensity
	UPROPERTY() float       RainIntensity = 0.f;
	// [mm]  Diamètre grêle           ← HailstoneDiameter
	UPROPERTY() float       HailDiameter = 0.f;
	// [0,1] Force tornade            ← UpdraftVelocity normalisé
	UPROPERTY() float       TornadoStrength = 0.f;
};

// ============================================================
// Seuils de transition et constantes de rendu
// Centralisés ici pour pouvoir les tweaker sans toucher au code métier.
// ============================================================
namespace CloudThresholds
{
	// Transitions d'état
	static constexpr float HumidityCumulus = 0.10f;
	static constexpr float HumidityCumulonimbus = 0.55f;
	static constexpr float HumidityStorm = 0.80f;
	static constexpr float CAPECumulonimbus = 500.f;   // J/kg
	static constexpr float CAPEStorm = 2000.f;
	static constexpr float CAPETornado = 2500.f;
	static constexpr float UpdraftTornado = 50.f;    // m/s

	// Rendu
	static constexpr float BaseCloudAltitude = 20000.f; // 200m en cm (pied du nuage)
	static constexpr float CloudWidthMultiplier = 2.8f;    // x CellSize → largeur du sprite
	static constexpr float MaxVerticalScale = 4.5f;    // cumulonimbus au maximum
	static constexpr float MinOpacity = 0.35f;
	static constexpr float MaxOpacity = 0.92f;
}