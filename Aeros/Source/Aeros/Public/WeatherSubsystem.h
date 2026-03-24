// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WeatherTypes.h"
#include "WeatherSimulationThread.h"
#include "Engine/Texture2D.h"
#include "WeatherSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogWeather, Log, All);

// ============================================================
// UWeatherSubsystem — Cerveau principal du système Aeros
//
// Gère :
//   - La grille atmosphérique (double buffer, 128x128 cellules)
//   - Le thread de simulation asynchrone
//   - Le scroll infini centré sur le joueur
//   - La texture radar encodée (R=Temp, G=Humidité, B=Pluie, A=CAPE)
//   - Le debug visuel (écran + lignes)
// ============================================================
UCLASS()
class AEROS_API UWeatherSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

	// === Paramètres de grille ===
	int32 GridSizeX = 128;
	int32 GridSizeY = 128;

	// CellSize en Unreal Units (cm). 1000 = 10 mètres par cellule.
	// La grille couvre 128 * 10m = 1.28 km.
	// Le vent est calibré en m/s de façon à ce que 1 m/s = 1 cellule/seconde visuellement.
	float CellSize = 1000.f;

	// === Double buffering ===
	// Invariant : bDisplayBufferIsA=true → on AFFICHE A, le thread ÉCRIT dans B.
	// Le flip se fait en DÉBUT de OnWeatherTick (après vérification que le thread est libre).
	TArray<FWeatherCell> GridBufferA;
	TArray<FWeatherCell> GridBufferB;
	bool bDisplayBufferIsA = true;

	// === Suivi de position (scroll infini) ===
	FIntPoint LastPlayerGridPos = FIntPoint(0, 0);

	// GlobalGridOffset : coordonnées monde (en cellules) du coin (0,0) de la grille locale.
	// Invariant : la cellule locale (x,y) représente la cellule monde (x + Offset.X, y + Offset.Y).
	FIntPoint GlobalGridOffset = FIntPoint(0, 0);

	bool bIsFirstTick = true;

	// === Thread de simulation ===
	FWeatherSimulationThread* SimulationThread = nullptr;

	// Timer du tick météo (1 seconde par défaut)
	FTimerHandle WeatherTickHandle;

	// Cache pré-alloué pour la mise à jour de texture (évite le malloc chaque tick)
	TArray<FColor> RadarPixelCache;

public:
	// === Texture Radar (Blueprint accessible) ===
	// Encodage : R=Température, G=Humidité, B=Précipitations, A=CAPE normalisé
	UPROPERTY(BlueprintReadOnly, Category = "Weather|Radar")
	UTexture2D* RadarTexture = nullptr;

	// Position monde (cm) du coin (0,0) de la texture radar.
	// Permet au Material de faire correspondre UV ↔ monde.
	UPROPERTY(BlueprintReadOnly, Category = "Weather|Radar")
	FVector2D RadarWorldOrigin = FVector2D::ZeroVector;

	// === Debug ===
	// Active/désactive le panneau de debug à l'écran (GEngine->AddOnScreenDebugMessage)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Debug")
	bool bShowDebugHUD = true;

	// Dessine des marqueurs de debug sur la cellule sous le joueur
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Debug")
	bool bShowDebugCell = false;

	// Nombre de cellules de grille à visualiser autour du joueur (0 = désactivé)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Debug")
	int32 DebugGridRadius = 0;

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// === API publique ===

	// Retourne les données météo à une position monde. Prend en compte le GlobalGridOffset.
	// Retourne false si la position est hors de la grille.
	UFUNCTION(BlueprintCallable, Category = "Weather|Query")
	bool GetWeatherAtLocation(FVector WorldLocation, FWeatherCell& OutCell) const;

	// Convertit une position monde (cm) en coordonnées de cellule locales (peut être hors grille).
	UFUNCTION(BlueprintCallable, Category = "Weather|Query")
	FIntPoint WorldToLocalCell(FVector WorldLocation) const;

	// Convertit une cellule locale en position monde (coin bas-gauche de la cellule, cm).
	UFUNCTION(BlueprintCallable, Category = "Weather|Query")
	FVector LocalCellToWorld(FIntPoint LocalCell) const;

	// Accès en lecture seule à l'offset global
	FIntPoint GetGlobalGridOffset() const { return GlobalGridOffset; }
	float     GetCellSize() const { return CellSize; }

private:
	void OnWeatherTick();
	void UpdateRadarTexture(const TArray<FWeatherCell>& DisplayGrid);
	void InitializeGridWithProcedural();
	void DrawDebugInfo(const FWeatherCell& PlayerCell);

	// Vérifie qu'un couple (LocalX, LocalY) est dans les bornes de la grille
	bool IsValidIndex(int32 LocalX, int32 LocalY) const;
};