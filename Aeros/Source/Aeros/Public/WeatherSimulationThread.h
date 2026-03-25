// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "WeatherTypes.h"

class FRunnableThread;
class FEvent;

// ============================================================
// Métriques exposées au GameThread (lecture thread-safe via GetMetrics())
// ============================================================
struct FWeatherThreadMetrics
{
    double LastTickDurationMs = 0.0;   // Durée du dernier tick de simulation
    int32  TotalTicksComputed = 0;     // Nombre total de ticks calculés
    int32  SkippedTicks = 0;     // Ticks sautés (thread encore occupé)
    int32  LastNewCellsCount = 0;     // Nouvelles cellules générées au dernier shift
    int32  LastShiftX = 0;     // Amplitude du dernier décalage appliqué
    int32  LastShiftY = 0;
};

// ============================================================
// Thread de simulation météorologique (FRunnable)
// Calcul asynchrone de l'advection semi-lagrangienne et de la thermodynamique.
// Le GameThread fournit deux buffers en alternance (double-buffering).
// ============================================================
class AEROS_API FWeatherSimulationThread : public FRunnable
{
    // --- Infra thread ---
    FRunnableThread* Thread = nullptr;
    FEvent* WaitEvent = nullptr;  // Sémaphore manuel (AutoReset=false)

    // --- État thread-safe ---
    FThreadSafeBool bStopThread{ false };
    FThreadSafeBool bIsCalculating{ false };

    // --- Paramètres grille (immuables après construction) ---
    int32 GridSizeX = 0;
    int32 GridSizeY = 0;
    float CellSize = 0.f;  // En Unreal Units (cm)

    // --- Buffers fournis par le Subsystem ---
    // Le thread ne fait QUE lire CurrentReadGrid et écrire dans CurrentWriteGrid.
    // EXCEPTION : ApplyGridShift modifie CurrentReadGrid en début de tick,
    // ce qui est sûr car le GameThread ne touche plus à ce buffer une fois TriggerCalculation() appelé.
    TArray<FWeatherCell>* CurrentReadGrid = nullptr;
    TArray<FWeatherCell>* CurrentWriteGrid = nullptr;

    // --- Commande de décalage de grille (protégée par mutex) ---
    // Écrit par le GameThread dans AddGridShift(), lu par le thread dans ApplyGridShift().
    FCriticalSection ShiftLock;
    int32 PendingShiftX = 0;
    int32 PendingShiftY = 0;
    int32 GlobalOffsetX = 0;  // Offset absolu de la grille (en cellules) dans le monde infini
    int32 GlobalOffsetY = 0;

    // --- Métriques (protégées par mutex pour lecture externe) ---
    FCriticalSection MetricsLock;
    FWeatherThreadMetrics Metrics;

    float SimulationTime = 0.f; // Chronomètre interne pour animer les rafales

public:
    FWeatherSimulationThread(int32 InGridSizeX, int32 InGridSizeY, float InCellSize);
    virtual ~FWeatherSimulationThread();

    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    virtual void Exit() override;

    // Lance un tick de simulation asynchrone. Ignoré si le thread est encore actif.
    void TriggerCalculation(TArray<FWeatherCell>* InReadGrid, TArray<FWeatherCell>* InWriteGrid);
    // Enregistre un décalage de grille à appliquer au prochain tick.
    // Thread-safe (protégé par mutex).
    void AddGridShift(int32 DeltaX, int32 DeltaY, int32 InGlobalOffsetX, int32 InGlobalOffsetY);

    bool IsCalculating() const { return bIsCalculating; }

    // Copie thread-safe des métriques courantes.
    FWeatherThreadMetrics GetMetrics();

    // Génère une cellule météo procédurale à partir de coordonnées monde absolues.
    FWeatherCell GenerateProceduralCell(int32 WorldCellX, int32 WorldCellY) const;

private:
    // --- Étapes internes du tick (appelées depuis le thread) ---

    // Étape 1 : Décale les données dans CurrentReadGrid pour centrer le joueur.
    //           Remplit les nouvelles zones avec du bruit procédural.
    void ApplyGridShift();

    // Étape 2 (par cellule) : Advection semi-lagrangienne (transport par le vent).
    void ComputeAdvection(int32 x, int32 y, int32 Index);

    // Étape 3 (par cellule) : Thermodynamique (évaporation, CAPE, précipitations, grêle).
    void ComputeThermodynamics(int32 x, int32 y, int32 Index, int32 OffX, int32 OffY);

    void PerformSimulationTick();
};
