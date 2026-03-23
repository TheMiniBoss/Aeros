// Fill out your copyright notice in the Description page of Project Settings.


#include "WeatherSimulationThread.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "GenericPlatform/GenericPlatformProcess.h"

FWeatherSimulationThread::FWeatherSimulationThread(int InGridSizeX, int InGridSizeY, float InCellSize) : GridSizeX(InGridSizeX), GridSizeY(InGridSizeY), CellSize(InCellSize)
{
	// Création d'un événement de synchronisation (Sémaphore) avec Auto-Reset = true
	WaitEvent = FGenericPlatformProcess::GetSynchEventFromPool(true);

	bStopThread = false;
	bIsCalculating = false;
	CurrentReadGrid = nullptr;
	CurrentWriteGrid = nullptr;

	// Création et lancement immédiat du thread OS
	Thread = FRunnableThread::Create(this, TEXT("WeatherSimulationThread"), 0, TPri_BelowNormal);
}

FWeatherSimulationThread::~FWeatherSimulationThread()
{
	if (Thread)
	{
		// Demande l'arrêt et attend que le thread finisse son exécution (join)
		Thread->Kill(true);
		delete Thread;
	}

	if (WaitEvent)
	{
		// Nettoyage de la mémoire
		FGenericPlatformProcess::ReturnSynchEventToPool(WaitEvent);
		WaitEvent = nullptr;
	}
}

bool FWeatherSimulationThread::Init()
{
	return true;
}

uint32 FWeatherSimulationThread::Run()
{
	while (!bStopThread)
	{
		// Le thread se met en sommeil ici. Il ne consomme pas de CPU.
		// Il sera réveillé par WaitEvent->Trigger() appelé depuis TriggerCalculation() ou Stop().
		WaitEvent->Wait();

		// Si on a été réveillé pour détruire le thread, on sort de la boucle.
		if (bStopThread) break;

		// On verrouille l'état
		bIsCalculating = true;

		// Exécution de la simulation physique lourde
		if (CurrentReadGrid && CurrentWriteGrid)
		{
			PerformSimulationTick();
		}

		// Fin du calcul, on libère l'état et on se prépare à redormir
		bIsCalculating = false;
		WaitEvent->Reset();
	}
	return 0;
}

void FWeatherSimulationThread::Stop()
{
	bStopThread = true;

	if (WaitEvent)
	{
		// On réveille le thread une dernière fois pour qu'il puisse évaluer (bStopThread == true) et quitter le Run()
		WaitEvent->Trigger();
	}
}

void FWeatherSimulationThread::Exit()
{
	// Exécuté juste avant la mort du thread. Parfait pour libérer des ressources spécifiques au thread si besoin.
}

void FWeatherSimulationThread::TriggerCalculation(TArray<FWeatherCell>* InReadGrid, TArray<FWeatherCell>* InWriteGrid)
{
	// Si le thread n'a pas fini le calcul précédent, on abandonne ce tick de simulation
	if (bIsCalculating) return;

	CurrentReadGrid = InReadGrid;
	CurrentWriteGrid = InWriteGrid;

	// Réveille le thread suspendu sur WaitEvent->Wait()
	WaitEvent->Trigger();
}

void FWeatherSimulationThread::PerformSimulationTick()
{
	for (int32 y = 0; y < GridSizeY; ++y)
	{
		for (int32 x = 0; x < GridSizeX; ++x)
		{
			int32 Index = y * GridSizeX + x;

			(*CurrentWriteGrid)[Index] = (*CurrentReadGrid)[Index];
			ComputeAdvection(x, y, Index);
			ComputeThermodynamics(Index);

			// --- NOUVEAU : BORDURE DE SÉCURITÉ ---
			// Si on est sur les bords extrêmes de la carte, on détruit les nuages
			if (x == 0 || y == 0 || x == GridSizeX - 1 || y == GridSizeY - 1)
			{
				(*CurrentWriteGrid)[Index].Humidity = 0.0f;
				// Optionnel : on remet à froid pour ne pas étirer la couleur rouge non plus
				(*CurrentWriteGrid)[Index].Temperature = -5.0f;
			}
		}
	}
}

void FWeatherSimulationThread::ComputeThermodynamics(int Index)
{
	FWeatherCell& OutCell = (*CurrentWriteGrid)[Index];
	FWeatherCell& InCell = (*CurrentReadGrid)[Index];

	int32 x = Index % GridSizeX;
	int32 y = Index / GridSizeX;

	// 1. LE VOLCAN (Source de chaleur)
	// 1. LE VOLCAN (Source de chaleur intermittente)
	if (x >= 2 && x <= 5 && y >= 2 && y <= 5)
	{
		// On crée un "Timer" basé sur l'index temporel (ou un Random plus strict)
		// 10% de chance d'émettre un gros nuage
		if (FMath::FRand() > 0.7f)
		{
			OutCell.Temperature = 40.f;
			OutCell.Humidity = 1.0f; // Plein de vapeur
		}
		else
		{
			// On force l'air à être sec le reste du temps pour bien séparer les nuages
			OutCell.Temperature = -5.f;
			OutCell.Humidity = 0.0f;
		}
	}

	// 2. ÉVAPORATION (Génération d'humidité)
	// Plus il fait chaud, plus l'eau s'évapore du sol.
	// On ajoute un tout petit peu d'humidité à chaque frame, plafonné à 1.0 (100%)
	float EvaporationRate = FMath::Max(0.0f, OutCell.Temperature * 0.0005f);
	OutCell.Humidity = FMath::Clamp(InCell.Humidity + EvaporationRate, 0.0f, 1.0f);

	// 3. CONVECTION ET CAPE (Instabilité)
	// L'air chaud ET humide crée de l'instabilité (orage).
	if (OutCell.Temperature > 20.f && OutCell.Humidity > 0.6f)
	{
		// Formule de gameplay : Température * Saturation en eau
		OutCell.CAPE = (OutCell.Temperature - 20.f) * (OutCell.Humidity * 1000.f);
	}
	else
	{
		OutCell.CAPE = 0.f; // Pas assez chaud ou humide pour faire monter l'air
	}

	// 4. PRÉCIPITATIONS ET GRÊLE (La vapeur devient liquide)
	if (OutCell.Humidity >= 0.99f) // Saturation maximale
	{
		OutCell.PrecipitationIntensity = 10.f; // Il pleut !
		OutCell.Humidity -= 0.05f; // La pluie vide le nuage de son humidité
	}
	else
	{
		OutCell.PrecipitationIntensity = 0.f;
	}

	// Calcul des Updrafts (Courants ascendants liés au CAPE)
	if (OutCell.CAPE > 1000.f)
	{
		OutCell.UpdraftVelocity = FMath::Sqrt(2.f * OutCell.CAPE) * 0.5f;
	}
	else
	{
		OutCell.UpdraftVelocity = 0.f;
	}
}

void FWeatherSimulationThread::ComputeAdvection(int x, int y, int Index)
{
	FWeatherCell& Cell = (*CurrentReadGrid)[Index];
	FWeatherCell& OutCell = (*CurrentWriteGrid)[Index];

	float DeltaTime = 1.0f; // Notre tick est de 1 seconde
	float TravelX_cm = Cell.WindVector.X * 100.f * DeltaTime;
	float TravelY_cm = Cell.WindVector.Y * 100.f * DeltaTime;

	float CurrentWorldX = x * CellSize;
	float CurrentWorldY = y * CellSize;

	float SourceWorldX = CurrentWorldX - TravelX_cm;
	float SourceWorldY = CurrentWorldY - TravelY_cm;

	float SourceU = SourceWorldX / CellSize;
	float SourceV = SourceWorldY / CellSize;

	int32 x0 = FMath::FloorToInt(SourceU);
	int32 y0 = FMath::FloorToInt(SourceV);
	int32 x1 = x0 + 1;
	int32 y1 = y0 + 1;

	x0 = FMath::Clamp(x0, 0, GridSizeX - 1);
	y0 = FMath::Clamp(y0, 0, GridSizeY - 1);
	x1 = FMath::Clamp(x1, 0, GridSizeX - 1);
	y1 = FMath::Clamp(y1, 0, GridSizeY - 1);

	float FractionX = SourceU - FMath::FloorToFloat(SourceU);
	float FractionY = SourceV - FMath::FloorToFloat(SourceV);

	const FWeatherCell& C00 = (*CurrentReadGrid)[y0 * GridSizeX + x0];
	const FWeatherCell& C10 = (*CurrentReadGrid)[y0 * GridSizeX + x1];
	const FWeatherCell& C01 = (*CurrentReadGrid)[y1 * GridSizeX + x0];
	const FWeatherCell& C11 = (*CurrentReadGrid)[y1 * GridSizeX + x1];

	// Interpolation Température
	float TempTop = FMath::Lerp(C00.Temperature, C10.Temperature, FractionX);
	float TempBot = FMath::Lerp(C01.Temperature, C11.Temperature, FractionX);
	OutCell.Temperature = FMath::Lerp(TempTop, TempBot, FractionY);

	// --- NOUVEAU : Interpolation Humidité ---
	float HumTop = FMath::Lerp(C00.Humidity, C10.Humidity, FractionX);
	float HumBot = FMath::Lerp(C01.Humidity, C11.Humidity, FractionX);
	OutCell.Humidity = FMath::Lerp(HumTop, HumBot, FractionY);
}
