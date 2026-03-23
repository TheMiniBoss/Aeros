// Fill out your copyright notice in the Description page of Project Settings.


#include "WeatherSubsystem.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"

void UWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    int TotalCells = GridSizeX * GridSizeY;
    GridBufferA.Init(FWeatherCell(), TotalCells);
    GridBufferB.Init(FWeatherCell(), TotalCells);

    // On crée une texture vierge de la taille exacte de notre grille
    RadarTexture = UTexture2D::CreateTransient(GridSizeX, GridSizeY, PF_B8G8R8A8);
    if (RadarTexture)
    {
        // On empêche le flou (on veut des pixels nets comme un vrai radar)
        RadarTexture->Filter = TF_Nearest;
        RadarTexture->SRGB = false;
        RadarTexture->AddressX = TA_Clamp;
        RadarTexture->AddressY = TA_Clamp;
        RadarTexture->UpdateResource();
    }

    // --- AJOUT POUR LE TEST VISUEL ---
    // On va générer des températures aléatoires pour voir notre "Heatmap" !
    // On calcule la vitesse parfaite pour traverser exactement 1 cellule par seconde
    // Vitesse (m/s) = Taille (cm) / 100
    float PerfectWindSpeed = CellSize / 100.f;

    for (int32 y = 0; y < GridSizeY; ++y)
    {
        for (int32 x = 0; x < GridSizeX; ++x)
        {
            int32 Index = y * GridSizeX + x;

            // On s'assure que le ciel de base est bleu et SANS nuages
            GridBufferA[Index].Temperature = -5.f;
            GridBufferA[Index].Humidity = 0.0f; // <-- AJOUTE CETTE LIGNE
            GridBufferA[Index].PrecipitationIntensity = 0.0f;

            GridBufferA[Index].WindVector = FVector2D(PerfectWindSpeed, PerfectWindSpeed);

            GridBufferB[Index] = GridBufferA[Index];
        }
    }
    // ---------------------------------


    SimulationThread = new FWeatherSimulationThread(GridSizeX, GridSizeY, CellSize);

    if (GetWorld() && GetWorld()->GetNetMode() != NM_Client)
    {
        GetWorld()->GetTimerManager().SetTimer(WeatherTickHandle, this, &UWeatherSubsystem::OnWeatherTick, 1.0f, true);
    }
}

void UWeatherSubsystem::Deinitialize()
{
    if (SimulationThread)
    {
        delete SimulationThread;
        SimulationThread = nullptr;
    }
    Super::Deinitialize();
}

void UWeatherSubsystem::OnWeatherTick()
{
    if (!SimulationThread) return;
    // Si le thread calcule encore, on attend le prochain Tick du GameThread (sécurité)
    if (SimulationThread->IsCalculating()) return;

    // Le calcul est terminé ! On swap les buffers.
    bIsUsingBufferA = !bIsUsingBufferA;

    TArray<FWeatherCell>& ReadGrid = bIsUsingBufferA ? GridBufferA : GridBufferB;
    TArray<FWeatherCell>& WriteGrid = bIsUsingBufferA ? GridBufferB : GridBufferA;

    // On réveille le thread avec les nouveaux buffers
    SimulationThread->TriggerCalculation(&ReadGrid, &WriteGrid);

    UpdateRadarTexture();
}

void UWeatherSubsystem::UpdateRadarTexture()
{
    if (!RadarTexture) return;

    const TArray<FWeatherCell>& ReadGrid = bIsUsingBufferA ? GridBufferA : GridBufferB;

    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(GridSizeX * GridSizeY);

    for (int32 y = 0; y < GridSizeY; ++y)
    {
        for (int32 x = 0; x < GridSizeX; ++x)
        {
            int32 Index = y * GridSizeX + x;
            const FWeatherCell& Cell = ReadGrid[Index];

            // R: Température (Normalisée entre -10 et 40°C)
            float NormTemp = FMath::Clamp((Cell.Temperature + 10.f) / 50.f, 0.f, 1.f);

            // G: Humidité pure (0 à 1)
            float NormHumidity = FMath::Clamp(Cell.Humidity, 0.f, 1.f);

            // B: Pluie (Normalisée, ex: 0 à 20 mm/h max)
            float NormRain = FMath::Clamp(Cell.PrecipitationIntensity / 20.f, 0.f, 1.f);

            // A: CAPE / Orages (Normalisé, 0 à 3000 Joules)
            float NormCAPE = FMath::Clamp(Cell.CAPE / 3000.f, 0.f, 1.f);

            // Création de la couleur Data-Packée
            FLinearColor DataColor(NormTemp, NormHumidity, NormRain, 1.0f);

            // Stockage en 8-bits pour la texture (Unreal gère la conversion sRGB / Linéaire ici)
            Pixels[Index] = DataColor.ToFColor(false); // false = on veut les datas pures, pas de correction gamma
        }
    }

    if (FTexturePlatformData* PlatformData = RadarTexture->GetPlatformData())
    {
        void* TextureData = PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memcpy(TextureData, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
        PlatformData->Mips[0].BulkData.Unlock();
        RadarTexture->UpdateResource();
    }
}

bool UWeatherSubsystem::GetTemperatureAtLocation(FVector Location, FWeatherCell& OutWeatherData) const
{
    int x = FMath::Clamp(FMath::FloorToInt(Location.X / CellSize), 0, GridSizeX - 1);
    int y = FMath::Clamp(FMath::FloorToInt(Location.Y / CellSize), 0, GridSizeY - 1);
    int Index = y * GridSizeX + x;

    const TArray<FWeatherCell>& ReadGrid = bIsUsingBufferA ? GridBufferA : GridBufferB;

    if (ReadGrid.IsValidIndex(Index))
    {
        OutWeatherData = ReadGrid[Index];
        return true;
    }
    return false;
}
