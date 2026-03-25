// Fill out your copyright notice in the Description page of Project Settings.


#include "WeatherSubsystem.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"

DEFINE_LOG_CATEGORY(LogWeather);

// ============================================================
// Helpers internes
// ============================================================

namespace WeatherDebug
{
	// Clés de debug uniques (pour éviter que les messages s'empilent chaque frame)
	static constexpr int32 KEY_HEADER = 900;
	static constexpr int32 KEY_OFFSET = 901;
	static constexpr int32 KEY_CELL = 902;
	static constexpr int32 KEY_TEMP = 903;
	static constexpr int32 KEY_HUMID = 904;
	static constexpr int32 KEY_CAPE = 905;
	static constexpr int32 KEY_RAIN = 906;
	static constexpr int32 KEY_HAIL = 907;
	static constexpr int32 KEY_THREAD = 908;
	static constexpr int32 KEY_SKIP = 909;
	static constexpr int32 KEY_SHIFT = 910;
}

// ============================================================
// Initialize / Deinitialize
// ============================================================

void UWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const int32 TotalCells = GridSizeX * GridSizeY;

	// Pré-allocation des buffers (pas d'allocation dynamique pendant le jeu)
	GridBufferA.Init(FWeatherCell(), TotalCells);
	GridBufferB.Init(FWeatherCell(), TotalCells);
	RadarPixelCache.SetNumUninitialized(TotalCells);

	// Initialisation procédurale (offset = 0,0 → sera recentré au premier tick)
	InitializeGridWithProcedural();

	// Texture radar : format B8G8R8A8, filtre Nearest pour des pixels nets
	RadarTexture = UTexture2D::CreateTransient(GridSizeX, GridSizeY, PF_B8G8R8A8);
	if (RadarTexture)
	{
		RadarTexture->Filter = TF_Nearest;
		RadarTexture->SRGB = false;          // Données pures, pas de correction gamma
		RadarTexture->AddressX = TA_Clamp;
		RadarTexture->AddressY = TA_Clamp;
		RadarTexture->UpdateResource();
	}

	UIRadarPixelCache.SetNumUninitialized(GridSizeX * GridSizeY);

	// On crée la texture UI (PF_B8G8R8A8).
	UIRadarTexture = UTexture2D::CreateTransient(GridSizeX, GridSizeY, PF_B8G8R8A8);
	if (UIRadarTexture)
	{
		// Ici on met TF_Bilinear au lieu de TF_Nearest pour que les pixels soient lissés quand on zoome fort !
		UIRadarTexture->Filter = TF_Bilinear;
		UIRadarTexture->SRGB = false;
		UIRadarTexture->UpdateResource();
	}

	// Thread de simulation
	SimulationThread = new FWeatherSimulationThread(GridSizeX, GridSizeY, CellSize);

	// Le tick météo ne tourne que sur le serveur (ou en mode solo)
	if (GetWorld() && GetWorld()->GetNetMode() != NM_Client)
	{
		GetWorld()->GetTimerManager().SetTimer(
			WeatherTickHandle,
			this,
			&UWeatherSubsystem::OnWeatherTick,
			1.0f,    // 1 seconde par tick météo
			true
		);
	}

	UE_LOG(LogWeather, Log,
		   TEXT("[WeatherSubsystem] Initialisé. Grille=%dx%d, Couverture=%.0fx%.0fm"),
		   GridSizeX, GridSizeY,
		   GridSizeX * CellSize / 100.f,
		   GridSizeY * CellSize / 100.f);
}

void UWeatherSubsystem::Deinitialize()
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(WeatherTickHandle);
	}

	if (SimulationThread)
	{
		delete SimulationThread;
		SimulationThread = nullptr;
	}

	Super::Deinitialize();
}

// ============================================================
// Tick météo principal
// ============================================================

void UWeatherSubsystem::OnWeatherTick()
{
	if (!SimulationThread || SimulationThread->IsCalculating())
	{
		// Le thread est encore occupé : on saute ce tick.
		// Le compteur SkippedTicks dans les métriques est incrémenté côté thread.
		return;
	}

	// =============================================================
	// ÉTAPE 1 : FLIP DU BUFFER
	// Le thread vient de finir d'écrire dans le buffer "write".
	// On le désigne maintenant comme buffer "display".
	// IMPORTANT : le flip se fait AVANT l'affichage, pas après.
	// C'est le bug du code original (la texture affichait un tick de retard).
	// =============================================================
	bDisplayBufferIsA = !bDisplayBufferIsA;

	// =============================================================
	// ÉTAPE 2 : AFFICHAGE (lecture du buffer fraîchement calculé)
	// =============================================================
	const TArray<FWeatherCell>& DisplayGrid = bDisplayBufferIsA ? GridBufferA : GridBufferB;
	UpdateRadarTexture(DisplayGrid);

	// =============================================================
	// ÉTAPE 3 : SCROLL INFINI (suivi du joueur)
	// On détermine combien de cellules le joueur a parcouru depuis
	// le dernier tick et on enregistre le décalage pour le thread.
	// =============================================================
	int32 DeltaX = 0, DeltaY = 0;

	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* Cam = PC ? PC->PlayerCameraManager : nullptr;

	if (Cam)
	{
		const FVector CamLoc = Cam->GetCameraLocation();

		// Cellule monde actuelle du joueur
		const FIntPoint CurrentWorldCell(
			FMath::FloorToInt(CamLoc.X / CellSize),
			FMath::FloorToInt(CamLoc.Y / CellSize)
		);

		if (bIsFirstTick)
		{
			// Premier tick : on centre le joueur au milieu de la grille.
			// GlobalGridOffset = CurrentWorldCell - GridCenter
			const FIntPoint GridCenter(GridSizeX / 2, GridSizeY / 2);
			GlobalGridOffset = CurrentWorldCell - GridCenter;

			// Le décalage est la totalité de l'offset initial
			DeltaX = GlobalGridOffset.X;
			DeltaY = GlobalGridOffset.Y;

			LastPlayerGridPos = CurrentWorldCell;
			bIsFirstTick = false;

			UE_LOG(LogWeather, Log,
				   TEXT("[WeatherSubsystem] Centrage initial. Joueur=(%d,%d), Offset=(%d,%d)"),
				   CurrentWorldCell.X, CurrentWorldCell.Y,
				   GlobalGridOffset.X, GlobalGridOffset.Y);
		}
		else
		{
			const FIntPoint Delta = CurrentWorldCell - LastPlayerGridPos;

			if (Delta.X != 0 || Delta.Y != 0)
			{
				DeltaX = Delta.X;
				DeltaY = Delta.Y;
				GlobalGridOffset += Delta;
				LastPlayerGridPos = CurrentWorldCell;
			}
		}
	}

	// Mise à jour de l'ancre monde de la texture radar
	// Coin (0,0) de la grille en coordonnées monde (cm)
	RadarWorldOrigin = FVector2D(
		GlobalGridOffset.X * CellSize,
		GlobalGridOffset.Y * CellSize
	);

	// =============================================================
	// ÉTAPE 4 : DEBUG
	// =============================================================
	if (bShowDebugHUD || bShowDebugCell || DebugGridRadius > 0)
	{
		FWeatherCell PlayerCell;
		const FVector PlayerLoc = Cam ? Cam->GetCameraLocation() : FVector::ZeroVector;
		GetWeatherAtLocation(PlayerLoc, PlayerCell);
		DrawDebugInfo(PlayerCell);
	}

	// =============================================================
	// ÉTAPE 5 : LANCEMENT DU PROCHAIN TICK DE SIMULATION
	// Le thread reçoit le DisplayGrid comme ReadGrid (source pour l'advection)
	// et l'OtherBuffer comme WriteGrid (destination de l'écriture).
	// =============================================================
	TArray<FWeatherCell>& ReadGrid = bDisplayBufferIsA ? GridBufferA : GridBufferB;
	TArray<FWeatherCell>& WriteGrid = bDisplayBufferIsA ? GridBufferB : GridBufferA;

	SimulationThread->AddGridShift(DeltaX, DeltaY, GlobalGridOffset.X, GlobalGridOffset.Y);
	SimulationThread->TriggerCalculation(&ReadGrid, &WriteGrid);

	// On met à jour l'image du joueur
	UpdateUIRadarTexture();
}

// ============================================================
// Mise à jour de la texture radar
// Encodage : R=Temp | G=Humidité | B=Pluie | A=CAPE
// ============================================================
void UWeatherSubsystem::UpdateRadarTexture(const TArray<FWeatherCell>& DisplayGrid)
{
	if (!RadarTexture || RadarPixelCache.Num() != GridSizeX * GridSizeY) return;

	for (int32 i = 0; i < DisplayGrid.Num(); ++i)
	{
		const FWeatherCell& Cell = DisplayGrid[i];

		// Normalisation de chaque canal dans [0,1]
		const float NTemp = FMath::Clamp((Cell.Temperature + 10.f) / 50.f, 0.f, 1.f);  // [-10°C, 40°C]
		const float NHumid = FMath::Clamp(Cell.Humidity, 0.f, 1.f);
		const float NRain = FMath::Clamp(Cell.PrecipitationIntensity / 50.f, 0.f, 1.f);  // [0, 50 mm/h]
		const float NCape = FMath::Clamp(Cell.CAPE / 3000.f, 0.f, 1.f);  // [0, 3000 J/kg]

		// FColor::ToFColor(false) = pas de correction gamma (données brutes)
		RadarPixelCache[i] = FLinearColor(NTemp, NHumid, NRain, NCape).ToFColor(false);
	}

	// Upload CPU → GPU (appelé depuis le GameThread : safe)
	if (FTexturePlatformData* PlatformData = RadarTexture->GetPlatformData())
	{
		void* TexData = PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(TexData, RadarPixelCache.GetData(), RadarPixelCache.Num() * sizeof(FColor));
		PlatformData->Mips[0].BulkData.Unlock();
		RadarTexture->UpdateResource();
	}
}

// ============================================================
// Initialisation procédurale des deux buffers
// ============================================================
void UWeatherSubsystem::InitializeGridWithProcedural()
{
	const float CoarseScale = 0.040f;
	const float FineScale = 0.120f;
	const float WindSpeed = CellSize / 100.f;

	for (int32 y = 0; y < GridSizeY; ++y)
	{
		for (int32 x = 0; x < GridSizeX; ++x)
		{
			const int32 Index = y * GridSizeX + x;

			// Bruit procédural (offset = 0 lors de l'init, sera recalé au premier tick)
			const float NoiseCoarse = FMath::PerlinNoise2D(FVector2D(x * CoarseScale, y * CoarseScale));
			const float NoiseFine = FMath::PerlinNoise2D(FVector2D(x * FineScale, y * FineScale));
			const float NoiseTemp = FMath::PerlinNoise2D(FVector2D(x * 0.02f, y * 0.02f));

			const float RawHumid = ((NoiseCoarse * 0.7f) + (NoiseFine * 0.3f) + 1.f) * 0.5f;

			FWeatherCell Cell;
			Cell.Temperature = 20.f + NoiseTemp * 6.f;
			Cell.Humidity = RawHumid > 0.55f ? RawHumid : 0.f;
			Cell.WindVector = FVector2D(WindSpeed, WindSpeed * 0.4f);

			GridBufferA[Index] = Cell;
			GridBufferB[Index] = Cell;  // Les deux buffers démarrent identiques
		}
	}
}

// ============================================================
// API publique — GetWeatherAtLocation
// BUG CORRIGÉ : on soustrait GlobalGridOffset pour la conversion monde → local
// ============================================================
bool UWeatherSubsystem::GetWeatherAtLocation(FVector WorldLocation, FWeatherCell& OutCell) const
{
	const FIntPoint LocalCell = WorldToLocalCell(WorldLocation);

	if (!IsValidIndex(LocalCell.X, LocalCell.Y))
	{
		return false;
	}

	const int32 Index = LocalCell.Y * GridSizeX + LocalCell.X;
	const TArray<FWeatherCell>& DisplayGrid = bDisplayBufferIsA ? GridBufferA : GridBufferB;

	if (DisplayGrid.IsValidIndex(Index))
	{
		OutCell = DisplayGrid[Index];
		return true;
	}
	return false;
}

FIntPoint UWeatherSubsystem::WorldToLocalCell(FVector WorldLocation) const
{
	// Cellule monde de cette position
	const int32 WorldCellX = FMath::FloorToInt(WorldLocation.X / CellSize);
	const int32 WorldCellY = FMath::FloorToInt(WorldLocation.Y / CellSize);

	// Cellule locale = cellule monde - offset global
	return FIntPoint(WorldCellX - GlobalGridOffset.X, WorldCellY - GlobalGridOffset.Y);
}

FVector UWeatherSubsystem::LocalCellToWorld(FIntPoint LocalCell) const
{
	// Coin bas-gauche de la cellule en coordonnées monde
	return FVector(
		(LocalCell.X + GlobalGridOffset.X) * CellSize,
		(LocalCell.Y + GlobalGridOffset.Y) * CellSize,
		0.f
	);
}

// ============================================================
// Debug HUD
// ============================================================
void UWeatherSubsystem::DrawDebugInfo(const FWeatherCell& PlayerCell)
{
	if (!GEngine) return;

	const FWeatherThreadMetrics Metrics = SimulationThread ? SimulationThread->GetMetrics() : FWeatherThreadMetrics();

	if (bShowDebugHUD)
	{
		// Couleur dynamique : rouge si chaud et humide (orage), bleu si froid
		const FColor TempColor = PlayerCell.Temperature > 25.f ? FColor::Orange : FColor::Cyan;

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_HEADER, 1.2f, FColor::White,
										 TEXT("═══ AEROS Weather Debug ═══"));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_OFFSET, 1.2f, FColor::Silver,
										 FString::Printf(TEXT("  Grid Offset     : (%d, %d)"), GlobalGridOffset.X, GlobalGridOffset.Y));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_CELL, 1.2f, FColor::Silver,
										 FString::Printf(TEXT("  Cellule locale  : (%d, %d)"),
										 LastPlayerGridPos.X - GlobalGridOffset.X,
										 LastPlayerGridPos.Y - GlobalGridOffset.Y));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_TEMP, 1.2f, TempColor,
										 FString::Printf(TEXT("  Température     : %.1f°C"), PlayerCell.Temperature));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_HUMID, 1.2f,
										 PlayerCell.Humidity > 0.9f ? FColor::Blue : FColor::Cyan,
										 FString::Printf(TEXT("  Humidité        : %.0f%%"), PlayerCell.Humidity * 100.f));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_RAIN, 1.2f,
										 PlayerCell.PrecipitationIntensity > 0.f ? FColor::Blue : FColor::Green,
										 FString::Printf(TEXT("  Précipitations  : %.1f mm/h"), PlayerCell.PrecipitationIntensity));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_CAPE, 1.2f,
										 PlayerCell.CAPE > 2000.f ? FColor::Red : (PlayerCell.CAPE > 500.f ? FColor::Yellow : FColor::Green),
										 FString::Printf(TEXT("  CAPE            : %.0f J/kg  [%s]"),
										 PlayerCell.CAPE,
										 PlayerCell.CAPE > 2500.f ? TEXT("DANGER") : (PlayerCell.CAPE > 1000.f ? TEXT("FORT") : TEXT("Faible"))));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_HAIL, 1.2f,
										 PlayerCell.HailstoneDiameter > 30.f ? FColor::Red : FColor::White,
										 FString::Printf(TEXT("  Grêle           : %.1f mm  (%s)"),
										 PlayerCell.HailstoneDiameter,
										 PlayerCell.HailstoneDiameter > 30.f ? TEXT("DESTRUCTRICE") :
										 (PlayerCell.HailstoneDiameter > 0.f ? TEXT("présente") : TEXT("aucune"))));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_THREAD, 1.2f, FColor::Silver,
										 FString::Printf(TEXT("  Sim Thread      : %d ticks (%.2f ms/tick)"),
										 Metrics.TotalTicksComputed, Metrics.LastTickDurationMs));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_SKIP, 1.2f,
										 Metrics.SkippedTicks > 5 ? FColor::Red : FColor::Silver,
										 FString::Printf(TEXT("  Ticks sautés    : %d"), Metrics.SkippedTicks));

		GEngine->AddOnScreenDebugMessage(WeatherDebug::KEY_SHIFT, 1.2f, FColor::Silver,
										 FString::Printf(TEXT("  Dernier shift   : (%d,%d) → %d nouvelles cellules"),
										 Metrics.LastShiftX, Metrics.LastShiftY, Metrics.LastNewCellsCount));
	}

	// Debug cellule sous le joueur
	if (bShowDebugCell)
	{
		const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (!PC || !PC->PlayerCameraManager) return;

		const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
		const FIntPoint LocalPos = WorldToLocalCell(CamLoc);
		const FVector CellOrigin = LocalCellToWorld(LocalPos);

		const FVector CellCenter = CellOrigin + FVector(CellSize * 0.5f, CellSize * 0.5f, 0.f);
		const FVector BoxExtent = FVector(CellSize * 0.48f, CellSize * 0.48f, 50.f);

		// Couleur de la boîte en fonction de l'humidité
		FColor BoxColor = FColor::Green;
		if (PlayerCell.PrecipitationIntensity > 0.f) BoxColor = FColor::Blue;
		if (PlayerCell.CAPE > 1000.f)               BoxColor = FColor::Yellow;
		if (PlayerCell.CAPE > 2500.f)               BoxColor = FColor::Red;

		DrawDebugBox(GetWorld(), CellCenter, BoxExtent, BoxColor, false, 1.1f, 0, 3.f);

		DrawDebugString(GetWorld(), CellCenter + FVector(0, 0, 200.f),
						FString::Printf(TEXT("T:%.1f H:%.0f%% P:%.0fmm/h"),
						PlayerCell.Temperature,
						PlayerCell.Humidity * 100.f,
						PlayerCell.PrecipitationIntensity),
						nullptr, BoxColor, 1.1f);
	}

	// Visualisation du rayon de grille autour du joueur
	if (DebugGridRadius > 0)
	{
		const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (!PC || !PC->PlayerCameraManager) return;

		const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
		const FIntPoint LocalPos = WorldToLocalCell(CamLoc);
		const TArray<FWeatherCell>& Grid = bDisplayBufferIsA ? GridBufferA : GridBufferB;

		for (int32 dy = -DebugGridRadius; dy <= DebugGridRadius; ++dy)
		{
			for (int32 dx = -DebugGridRadius; dx <= DebugGridRadius; ++dx)
			{
				const int32 LX = LocalPos.X + dx;
				const int32 LY = LocalPos.Y + dy;

				if (!IsValidIndex(LX, LY)) continue;

				const FWeatherCell& C = Grid[LY * GridSizeX + LX];

				// On ne dessine que les cellules actives (humidité > 0)
				if (C.Humidity < 0.01f) continue;

				const FVector Origin = LocalCellToWorld(FIntPoint(LX, LY));
				const FVector Center = Origin + FVector(CellSize * 0.5f, CellSize * 0.5f, 0.f);

				// Intensité de la couleur proportionnelle à l'humidité
				const uint8 Intensity = static_cast<uint8>(C.Humidity * 255.f);
				const FColor CellColor = C.CAPE > 1000.f
					? FColor(Intensity, Intensity, 0)     // Jaune = instabilité
					: FColor(0, 0, Intensity);            // Bleu = nuage/pluie

				DrawDebugPoint(GetWorld(), Center, 8.f, CellColor, false, 1.1f);
			}
		}
	}
}

void UWeatherSubsystem::UpdateUIRadarTexture()
{
	if (!UIRadarTexture || UIRadarPixelCache.Num() != GridSizeX * GridSizeY) return;

	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager) return;

	FVector PlayerLoc = PC->PlayerCameraManager->GetCameraLocation();
	int32 HalfRes = UIRadarResolution / 2;

	for (int32 py = 0; py < UIRadarResolution; ++py)
	{
		for (int32 px = 0; px < UIRadarResolution; ++px)
		{
			// 1. Décalage spatial
			float WorldOffsetX = (px - HalfRes) * CellSize * RadarZoom;
			float WorldOffsetY = (py - HalfRes) * CellSize * RadarZoom;
			FVector PixelWorldLoc = PlayerLoc + FVector(WorldOffsetX, WorldOffsetY, 0.f);

			FWeatherCell Cell;

			// 2. LECTURE STRICTE DE LA SIMULATION
			bool bIsSimulated = GetWeatherAtLocation(PixelWorldLoc, Cell);

			int32 Index = py * UIRadarResolution + px;

			// 3. Dessin du Joueur (au centre)
			if (FMath::Abs(px - HalfRes) <= 1 && FMath::Abs(py - HalfRes) <= 1)
			{
				if (px == HalfRes && py == HalfRes)
					UIRadarPixelCache[Index] = FColor::White;
				else
					UIRadarPixelCache[Index] = FColor(0, 0, 0, 255);
			}
			else
			{
				if (bIsSimulated)
				{
					// 4. Coloration de la VRAIE météo
					if (bIsSimulated)
					{
						// --- CHOIX DE LA COULEUR SELON LE MODE ---
						switch (CurrentRadarMode)
						{
							case ERadarMode::Reflectivity:
								UIRadarPixelCache[Index] = GetReflectivityColor(Cell.PrecipitationIntensity, Cell.HailstoneDiameter);
								break;

							case ERadarMode::Velocity:
							{
								// Pour le Doppler, il nous faut le vecteur entre le joueur (radar) et le nuage
								FVector2D DirFromRadar = FVector2D(PixelWorldLoc.X - PlayerLoc.X, PixelWorldLoc.Y - PlayerLoc.Y);
								DirFromRadar.Normalize(); // On le réduit à 1

								// Et on ne dessine le vent QUE là où il y a un nuage pour renvoyer l'écho !
								if (Cell.Humidity > 0.4f)
									UIRadarPixelCache[Index] = GetVelocityColor(Cell.WindVector, DirFromRadar);
								else
									UIRadarPixelCache[Index] = FColor(0, 0, 0, 0); // Pas de nuage = pas d'écho radar
								break;
							}

							case ERadarMode::Instability:
								UIRadarPixelCache[Index] = GetInstabilityColor(Cell.CAPE);
								break;
						}
					}
				}
				else
				{
					// 5. FINIE LA TRICHE : Si on regarde au-delà des 256x256 simulés (à cause d'un dézoom), on met du vide.
					UIRadarPixelCache[Index] = FColor(0, 0, 0, 0);
				}
			}
		}
	}

	// 6. Envoi à la carte graphique
	if (FTexturePlatformData* PlatformData = UIRadarTexture->GetPlatformData())
	{
		void* TexData = PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(TexData, UIRadarPixelCache.GetData(), UIRadarPixelCache.Num() * sizeof(FColor));
		PlatformData->Mips[0].BulkData.Unlock();
		UIRadarTexture->UpdateResource();
	}
}

FColor UWeatherSubsystem::GetReflectivityColor(float RainIntensity, float Hail)
{
	if (RainIntensity < 0.1f && Hail == 0.f) return FColor(0, 0, 0, 0);

	// 1. DÉFINITION DES COULEURS CLÉS (RGBA) pour les vrais radars météo
	FLinearColor C_Bruine(0.f, 1.f, 1.f, 0.4f);  // Cyan Translucide
	FLinearColor C_Verte(0.f, 1.f, 0.f, 0.6f);   // Verte
	FLinearColor C_Jaune(1.f, 1.f, 0.f, 0.8f);   // Jaune
	FLinearColor C_Orange(1.f, 0.5f, 0.f, 0.9f); // Orange
	FLinearColor C_Rouge(1.f, 0.f, 0.f, 1.f);    // Rouge intense (Pluie torrentielle)
	FLinearColor C_Magenta(1.f, 0.f, 1.f, 1.f);  // Magenta (Grêle massive/Tornade)

	FLinearColor BaseColor;

	// 2. INTERPOLATION DOUCE (Lerp) de la pluie (Réflectivité de 0 à 35 mm/h)
	if (RainIntensity < 0.5f)
	{
		BaseColor = FLinearColor::LerpUsingHSV(FLinearColor::Transparent, C_Bruine, FMath::GetRangePct(0.1f, 0.5f, RainIntensity));
	}
	else if (RainIntensity < 1.0f)
	{
		BaseColor = FLinearColor::LerpUsingHSV(C_Bruine, C_Verte, FMath::GetRangePct(0.5f, 1.0f, RainIntensity));
	}
	else if (RainIntensity < 5.0f)
	{
		BaseColor = FLinearColor::LerpUsingHSV(C_Verte, C_Jaune, FMath::GetRangePct(1.0f, 5.0f, RainIntensity));
	}
	else if (RainIntensity < 15.0f)
	{
		BaseColor = FLinearColor::LerpUsingHSV(C_Jaune, C_Orange, FMath::GetRangePct(5.0f, 15.0f, RainIntensity));
	}
	else
	{
		// Au-delà de 30mm/h, on fonce vers le Rouge vif
		BaseColor = FLinearColor::LerpUsingHSV(C_Orange, C_Rouge, FMath::Clamp(FMath::GetRangePct(15.0f, 35.0f, RainIntensity), 0.f, 1.f));
	}

	// 3. SUPERPOSITION DE LA GRÊLE (Le cœur de l'écho radar)
	// S'il y a de la grêle, on fait un fondu de la couleur de pluie vers le Magenta !
	FLinearColor FinalColor = BaseColor;
	if (Hail > 0.f)
	{
		// La grêle écrase la couleur de pluie de 0 à 15mm de diamètre
		float HailAlpha = FMath::Clamp(Hail / 15.f, 0.f, 1.f);
		FinalColor = FLinearColor::LerpUsingHSV(BaseColor, C_Magenta, HailAlpha);
	}

	return FinalColor.ToFColor(true); // false pour sRGB pur
}

FColor UWeatherSubsystem::GetVelocityColor(FVector2D WindVector, FVector2D DirFromRadar)
{
	float V = FVector2D::DotProduct(WindVector, DirFromRadar);

	FLinearColor C_Cyan(0.f, 1.f, 1.f, 1.f);
	FLinearColor C_Green(0.f, 1.f, 0.f, 0.8f);
	FLinearColor C_Neutral(0.5f, 0.5f, 0.5f, 0.1f); // Gris presque invisible
	FLinearColor C_Red(1.f, 0.f, 0.f, 0.8f);
	FLinearColor C_Orange(1.f, 0.5f, 0.f, 1.f);

	if (V < 0.f) // S'approche (Gris -> Vert -> Cyan)
	{
		if (V > -5.f)  return FLinearColor::LerpUsingHSV(C_Neutral, C_Green, FMath::GetRangePct(0.f, -5.f, V)).ToFColor(true);
		return FLinearColor::LerpUsingHSV(C_Green, C_Cyan, FMath::Clamp(FMath::GetRangePct(-5.f, -15.f, V), 0.f, 1.f)).ToFColor(true);
	}
	else // S'éloigne (Gris -> Rouge -> Orange)
	{
		if (V < 5.f)   return FLinearColor::LerpUsingHSV(C_Neutral, C_Red, FMath::GetRangePct(0.f, 5.f, V)).ToFColor(true);
		return FLinearColor::LerpUsingHSV(C_Red, C_Orange, FMath::Clamp(FMath::GetRangePct(5.f, 15.f, V), 0.f, 1.f)).ToFColor(true);
	}
}

FColor UWeatherSubsystem::GetInstabilityColor(float CAPE)
{
	if (CAPE < 500.f) return FColor(0, 0, 0, 0);

	FLinearColor C_Clear(1.f, 1.f, 0.f, 0.f);     // Jaune totalement transparent
	FLinearColor C_Yellow(1.f, 1.f, 0.f, 0.6f);   // Jaune
	FLinearColor C_Red(1.f, 0.f, 0.f, 0.8f);      // Rouge
	FLinearColor C_White(1.f, 1.f, 1.f, 1.f);     // Blanc (Danger extrême)

	if (CAPE < 1000.f) return FLinearColor::LerpUsingHSV(C_Clear, C_Yellow, FMath::GetRangePct(500.f, 1000.f, CAPE)).ToFColor(true);
	if (CAPE < 2000.f) return FLinearColor::LerpUsingHSV(C_Yellow, C_Red, FMath::GetRangePct(1000.f, 2000.f, CAPE)).ToFColor(true);

	return FLinearColor::LerpUsingHSV(C_Red, C_White, FMath::Clamp(FMath::GetRangePct(2000.f, 3000.f, CAPE), 0.f, 1.f)).ToFColor(true);
}

// ============================================================
// Validation d'index de grille
// ============================================================
bool UWeatherSubsystem::IsValidIndex(int32 LocalX, int32 LocalY) const
{
	return LocalX >= 0 && LocalX < GridSizeX && LocalY >= 0 && LocalY < GridSizeY;
}