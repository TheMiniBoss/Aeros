// Fill out your copyright notice in the Description page of Project Settings.


#include "WeatherSimulationThread.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "GenericPlatform/GenericPlatformProcess.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeatherThread, Log, All);

// ============================================================
// Construction / Destruction
// ============================================================

FWeatherSimulationThread::FWeatherSimulationThread(int32 InGridSizeX, int32 InGridSizeY, float InCellSize)
	: GridSizeX(InGridSizeX)
	, GridSizeY(InGridSizeY)
	, CellSize(InCellSize)
{
	// AutoReset = false : on appelle Reset() manuellement en fin de Run()
	// pour un contrôle explicite du cycle veille/réveil.
	WaitEvent = FGenericPlatformProcess::GetSynchEventFromPool(false);

	Thread = FRunnableThread::Create(this, TEXT("WeatherSimulationThread"), 0, TPri_BelowNormal);

	UE_LOG(LogWeatherThread, Log,
		   TEXT("[WeatherThread] Démarré. Grille=%dx%d, Cellule=%.0fcm (%.1fm)"),
		   GridSizeX, GridSizeY, CellSize, CellSize / 100.f);
}

FWeatherSimulationThread::~FWeatherSimulationThread()
{
	if (Thread)
	{
		// Kill(true) = attendre la fin du thread (join)
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
	if (WaitEvent)
	{
		FGenericPlatformProcess::ReturnSynchEventToPool(WaitEvent);
		WaitEvent = nullptr;
	}
}

// ============================================================
// FRunnable Interface
// ============================================================

bool FWeatherSimulationThread::Init()
{
	return true;
}

uint32 FWeatherSimulationThread::Run()
{
	while (!bStopThread)
	{
		// Le thread dort ici sans consommer de CPU.
		// Réveillé par WaitEvent->Trigger() depuis TriggerCalculation() ou Stop().
		WaitEvent->Wait();

		if (bStopThread) break;

		bIsCalculating = true;

		if (CurrentReadGrid && CurrentWriteGrid)
		{
			const double StartTime = FPlatformTime::Seconds();

			PerformSimulationTick();

			const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

			FScopeLock Lock(&MetricsLock);
			Metrics.LastTickDurationMs = DurationMs;
			++Metrics.TotalTicksComputed;
		}

		bIsCalculating = false;

		// Réinitialise manuellement le sémaphore : prêt pour le prochain signal.
		WaitEvent->Reset();
	}

	UE_LOG(LogWeatherThread, Log, TEXT("[WeatherThread] Arrêt propre."));
	return 0;
}

void FWeatherSimulationThread::Stop()
{
	bStopThread = true;
	// On réveille le thread une dernière fois pour qu'il évalue bStopThread et quitte Run().
	if (WaitEvent) WaitEvent->Trigger();
}

void FWeatherSimulationThread::Exit() {}

// ============================================================
// API GameThread
// ============================================================

void FWeatherSimulationThread::TriggerCalculation(TArray<FWeatherCell>* InReadGrid, TArray<FWeatherCell>* InWriteGrid)
{
	if (bIsCalculating)
	{
		FScopeLock Lock(&MetricsLock);
		++Metrics.SkippedTicks;
		return;
	}

	CurrentReadGrid = InReadGrid;
	CurrentWriteGrid = InWriteGrid;
	WaitEvent->Trigger();
}

void FWeatherSimulationThread::AddGridShift(int32 DeltaX, int32 DeltaY, int32 InGlobalOffsetX, int32 InGlobalOffsetY)
{
	// Protégé car écrit par le GameThread et lu par le thread de simulation.
	FScopeLock Lock(&ShiftLock);
	PendingShiftX += DeltaX;
	PendingShiftY += DeltaY;
	GlobalOffsetX = InGlobalOffsetX;
	GlobalOffsetY = InGlobalOffsetY;
}

FWeatherThreadMetrics FWeatherSimulationThread::GetMetrics()
{
	FScopeLock Lock(&MetricsLock);
	return Metrics;
}

// ============================================================
// Simulation (appelée depuis le thread)
// ============================================================

void FWeatherSimulationThread::PerformSimulationTick()
{
	// On avance le temps d'une seconde
	SimulationTime += 1.f;

	// --- Étape 1 : Décalage infini de la grille ---
	// Modifie CurrentReadGrid en place. C'est sûr : le GameThread ne touche
	// plus à ce buffer depuis l'appel à TriggerCalculation().
	ApplyGridShift();

	// On récupère l'Offset Global (pour que le vent soit raccord avec le scroll infini)
	int32 OffX, OffY;
	{
		FScopeLock Lock(&ShiftLock);
		OffX = GlobalOffsetX;
		OffY = GlobalOffsetY;
	}

	// --- Étape 2 & 3 : Physique cellule par cellule ---
	// On exclut la bordure (1 cellule de marge) gérée séparément.
	for (int32 y = 1; y < GridSizeY - 1; ++y)
	{
		for (int32 x = 1; x < GridSizeX - 1; ++x)
		{
			const int32 Index = y * GridSizeX + x;

			// Copie de base depuis le buffer source
			(*CurrentWriteGrid)[Index] = (*CurrentReadGrid)[Index];

			// Advection (transport par le vent) — lit depuis Read, écrit dans Write
			ComputeAdvection(x, y, Index);

			// Thermodynamique — lit et écrit dans Write (post-advection)
			// On passe maintenant x, y, et l'Offset à la Thermodynamique !
			ComputeThermodynamics(x, y, Index, OffX, OffY);
		}
	}

	// --- Étape 4 : Bordure Génératrice (Inflow Boundary) ---
	// Au lieu de détruire la météo, on force le bord extrême de la carte à lire 
	// le générateur procédural en continu. Le vent aspirera ces données naturellement.
	for (int32 i = 0; i < GridSizeX * GridSizeY; ++i)
	{
		const int32 x = i % GridSizeX;
		const int32 y = i / GridSizeX;

		// Si on est sur l'anneau extérieur de la grille...
		if (x == 0 || y == 0 || x == GridSizeX - 1 || y == GridSizeY - 1)
		{
			// On génère la météo pure du monde à cet endroit précis
			(*CurrentWriteGrid)[i] = GenerateProceduralCell(x + OffX, y + OffY);
		}
	}
}

// ============================================================
// Étape 1 — ApplyGridShift
// Décale les données de CurrentReadGrid pour suivre le joueur.
// Les nouvelles cellules découvertes en bordure sont générées procéduralement.
// ============================================================
void FWeatherSimulationThread::ApplyGridShift()
{
	// Capture atomique et remise à zéro du décalage en attente
	int32 ShiftX, ShiftY, OffX, OffY;
	{
		FScopeLock Lock(&ShiftLock);
		ShiftX = PendingShiftX;
		ShiftY = PendingShiftY;
		OffX = GlobalOffsetX;
		OffY = GlobalOffsetY;
		PendingShiftX = 0;
		PendingShiftY = 0;
	}

	if (ShiftX == 0 && ShiftY == 0) return;

	// Copie temporaire pour lire les anciennes valeurs sans conflits
	TArray<FWeatherCell> TempGrid = *CurrentReadGrid;

	int32 NewCellsGenerated = 0;

	for (int32 y = 0; y < GridSizeY; ++y)
	{
		for (int32 x = 0; x < GridSizeX; ++x)
		{
			const int32 Index = y * GridSizeX + x;

			// Où lire dans l'ancienne grille ?
			// Si le joueur avance de +1 cellule en X, ShiftX=+1 :
			// la cellule (x) doit maintenant contenir ce qui était en (x+1).
			const int32 SrcX = x + ShiftX;
			const int32 SrcY = y + ShiftY;

			if (SrcX >= 0 && SrcX < GridSizeX && SrcY >= 0 && SrcY < GridSizeY)
			{
				// Zone connue : on déplace les données existantes
				(*CurrentReadGrid)[Index] = TempGrid[SrcY * GridSizeX + SrcX];
			}
			else
			{
				// Nouvelle zone découverte : génération procédurale
				// On utilise les coordonnées GLOBALES pour un bruit continu et cohérent
				const int32 WorldCellX = x + OffX;
				const int32 WorldCellY = y + OffY;
				(*CurrentReadGrid)[Index] = GenerateProceduralCell(WorldCellX, WorldCellY);
				++NewCellsGenerated;
			}
		}
	}

	{
		FScopeLock Lock(&MetricsLock);
		Metrics.LastNewCellsCount = NewCellsGenerated;
		Metrics.LastShiftX = ShiftX;
		Metrics.LastShiftY = ShiftY;
	}
}

// ============================================================
// Génération procédurale d'une cellule (coordonnées monde absolues)
// Deux couches de Perlin pour une structure nuageuse naturelle.
// ============================================================
FWeatherCell FWeatherSimulationThread::GenerateProceduralCell(int32 WorldCellX, int32 WorldCellY) const
{
	FWeatherCell Cell;

	// Deux fréquences : grandes masses + détails fins
	const float CoarseScale = 0.040f;
	const float FineScale = 0.120f;
	const float TempScale = 0.015f;
	const float PressScale = 0.008f;

	const float NoiseCoarse = FMath::PerlinNoise2D(FVector2D(WorldCellX * CoarseScale, WorldCellY * CoarseScale));
	const float NoiseFine = FMath::PerlinNoise2D(FVector2D(WorldCellX * FineScale, WorldCellY * FineScale));
	const float NoiseTemp = FMath::PerlinNoise2D(FVector2D(WorldCellX * TempScale, WorldCellY * TempScale));
	const float NoisePress = FMath::PerlinNoise2D(FVector2D(WorldCellX * PressScale, WorldCellY * PressScale));

	// Humidité : combinaison pondérée des deux couches [-1,1] → [0,1]
	const float RawHumidity = ((NoiseCoarse * 0.7f) + (NoiseFine * 0.3f) + 1.f) * 0.5f;

	// Seuil de nuages : en dessous de 0.55, ciel dégagé
	Cell.Humidity = RawHumidity > 0.55f ? RawHumidity : 0.f;
	Cell.Temperature = 20.f + NoiseTemp * 8.f;                 // ±8°C autour de 20°C
	Cell.Pressure = 1013.f + NoisePress * 12.f;             // ±12 hPa

	// Vent constant (calibré pour traverser 1 cellule par seconde)
	const float WindSpeed = CellSize / 100.f;                  // cm → m/s
	Cell.WindVector = FVector2D(WindSpeed, WindSpeed * 0.4f); // Légère composante Nord

	return Cell;
}

// ============================================================
// Étape 2 — ComputeAdvection (transport semi-lagrangien)
// On remonte le vecteur vent pour trouver la cellule source.
// Interpolation bilinéaire pour un résultat lisse.
// ============================================================
void FWeatherSimulationThread::ComputeAdvection(int32 x, int32 y, int32 Index)
{
	const FWeatherCell& InCell = (*CurrentReadGrid)[Index];
	FWeatherCell& OutCell = (*CurrentWriteGrid)[Index];

	// DeltaTime fixe = 1 seconde (fréquence de tick du Subsystem)
	constexpr float DeltaTime = 1.f;

	// Position source : on "remonte" le vent (advection inverse)
	// WindVector est en m/s ; on divise par CellSize (en cm) après conversion pour avoir une position en cellules
	const float SrcU = static_cast<float>(x) - (InCell.WindVector.X * 100.f * DeltaTime / CellSize);
	const float SrcV = static_cast<float>(y) - (InCell.WindVector.Y * 100.f * DeltaTime / CellSize);

	// Coins de l'interpolation bilinéaire
	const int32 x0 = FMath::Clamp(FMath::FloorToInt(SrcU), 0, GridSizeX - 1);
	const int32 y0 = FMath::Clamp(FMath::FloorToInt(SrcV), 0, GridSizeY - 1);
	const int32 x1 = FMath::Clamp(x0 + 1, 0, GridSizeX - 1);
	const int32 y1 = FMath::Clamp(y0 + 1, 0, GridSizeY - 1);

	const float Fx = SrcU - FMath::FloorToFloat(SrcU);
	const float Fy = SrcV - FMath::FloorToFloat(SrcV);

	const FWeatherCell& C00 = (*CurrentReadGrid)[y0 * GridSizeX + x0];
	const FWeatherCell& C10 = (*CurrentReadGrid)[y0 * GridSizeX + x1];
	const FWeatherCell& C01 = (*CurrentReadGrid)[y1 * GridSizeX + x0];
	const FWeatherCell& C11 = (*CurrentReadGrid)[y1 * GridSizeX + x1];

	// Lambda d'interpolation bilinéaire pour éviter la répétition
	auto Bilerp = [Fx, Fy](float v00, float v10, float v01, float v11) -> float
	{
		return FMath::Lerp(FMath::Lerp(v00, v10, Fx), FMath::Lerp(v01, v11, Fx), Fy);
	};

	// Les champs transportés par le vent
	OutCell.Temperature = Bilerp(C00.Temperature, C10.Temperature, C01.Temperature, C11.Temperature);
	OutCell.Humidity = Bilerp(C00.Humidity, C10.Humidity, C01.Humidity, C11.Humidity);
	OutCell.PrecipitationIntensity = Bilerp(C00.PrecipitationIntensity, C10.PrecipitationIntensity, C01.PrecipitationIntensity, C11.PrecipitationIntensity);
	// Note : on n'advecte pas CAPE ni UpdraftVelocity (dérivés, recalculés en thermodynamique)
}

// ============================================================
// Étape 3 — ComputeThermodynamics
// Évaporation, instabilité convective (CAPE), précipitations, grêle.
// Lit et écrit dans CurrentWriteGrid (post-advection).
// ============================================================
void FWeatherSimulationThread::ComputeThermodynamics(int32 x, int32 y, int32 Index, int32 OffX, int32 OffY)
{
	FWeatherCell& Cell = (*CurrentWriteGrid)[Index];

	// --- 1. Évaporation ---
	// L'air chaud s'humidifie progressivement (source virtuelle du sol)
	const float EvapRate = FMath::Max(0.f, Cell.Temperature * 0.0004f);
	Cell.Humidity = FMath::Clamp(Cell.Humidity + EvapRate, 0.f, 1.f);

	// --- 2. CAPE (Convective Available Potential Energy) ---
	// L'instabilité se développe si l'air est chaud ET humide
	if (Cell.Temperature > 20.f && Cell.Humidity > 0.60f)
	{
		// On utilise FMath::Pow() sur l'humidité pour que le CAPE n'explose 
		// vraiment que quand on s'approche des 90% d'humidité !
		Cell.CAPE = (Cell.Temperature - 20.f) * FMath::Pow(Cell.Humidity, 3.f) * 1500.f;
	}
	else
	{
		Cell.CAPE = 0.f;
	}

	// --- 3. Précipitations ---
	// La saturation (≥98%) déclenche la pluie et vide le nuage
	if (Cell.Humidity >= 0.98f)
	{
		// Intensité proportionnelle au surplus d'humidité
		Cell.PrecipitationIntensity = FMath::Clamp((Cell.Humidity - 0.98f) * 500.f, 0.f, 50.f);
		Cell.Humidity -= 0.04f;  // La pluie désature le nuage
	}
	else
	{
		Cell.PrecipitationIntensity = 0.f;
	}

	// --- 4. Updrafts et Grêle ---
	if (Cell.CAPE > 800.f)
	{
		// On réduit le multiplicateur de l'updraft (0.35 au lieu de 0.5)
		Cell.UpdraftVelocity = FMath::Sqrt(2.f * Cell.CAPE) * 0.35f;

		// La grêle nécessite un updraft très violent (> 18 m/s au lieu de 15)
		Cell.HailstoneDiameter = Cell.UpdraftVelocity > 18.f
			? (Cell.UpdraftVelocity - 18.f) * 2.0f
			: 0.f;
	}
	else
	{
		Cell.UpdraftVelocity = 0.f;
		Cell.HailstoneDiameter = 0.f;
	}

	// --- 5. Vent Local et Rafales ---

	// Le vent directeur DYNAMIQUE (Change très lentement de direction au fil du temps)
	// On utilise un bruit 1D très lent pour générer un angle entre -PI et +PI (-180° à 180°)
	const float WindAngle = FMath::PerlinNoise1D(SimulationTime * 0.002f) * PI;

	// La force de base du vent (ex: 10 m/s)
	const float WindSpeed = CellSize / 100.f;

	// On utilise la Trigonométrie (Cos/Sin) pour transformer cet angle en vecteur 2D
	const float BaseWindU = FMath::Cos(WindAngle) * WindSpeed;
	const float BaseWindV = FMath::Sin(WindAngle) * WindSpeed;

	// Coordonnées absolues pour que les rafales soient fixes dans le monde infini
	const float WorldX = x + OffX;
	const float WorldY = y + OffY;

	// On décale la lecture du bruit avec SimulationTime pour que les rafales voyagent
	const float GustScale = 0.08f; // Taille géographique des bourrasques
	const float TimeShift = SimulationTime * 0.8f; // Vitesse d'évolution du vent

	// Bruit de Perlin (génère des valeurs entre -1.0 et 1.0)
	const float GustU = FMath::PerlinNoise2D(FVector2D(WorldX * GustScale + TimeShift, WorldY * GustScale));
	const float GustV = FMath::PerlinNoise2D(FVector2D(WorldX * GustScale, WorldY * GustScale + TimeShift));

	// Turbulences liées aux Orages (Le vent s'affole sous la pluie et le CAPE)
	// Si le ciel est bleu, Multiplier = 1. S'il y a un gros orage, ça peut monter à 4 ou 5 !
	const float StormTurbulence = 1.0f + (Cell.PrecipitationIntensity * 0.15f) + (Cell.CAPE * 0.001f);

	// Force de base des rafales (ex: +/- 2 m/s), multipliée par la tempête
	const float GustStrength = 2.0f * StormTurbulence;

	// On applique le nouveau vent à la cellule pour le prochain tick
	Cell.WindVector.X = BaseWindU + (GustU * GustStrength);
	Cell.WindVector.Y = BaseWindV + (GustV * GustStrength);
}
