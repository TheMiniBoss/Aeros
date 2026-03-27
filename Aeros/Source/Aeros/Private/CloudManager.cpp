// Fill out your copyright notice in the Description page of Project Settings.

#include "CloudManager.h"
#include "WeatherSubsystem.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

DEFINE_LOG_CATEGORY(LogCloudManager);

namespace CloudDebugKeys
{
	static constexpr int32 HEADER = 800;
	static constexpr int32 ACTIVE = 801;
	static constexpr int32 FREE = 802;
	static constexpr int32 EXHAUSTED = 803;
	static constexpr int32 LOD0 = 804;
	static constexpr int32 LOD1 = 805;
	static constexpr int32 LOD2 = 806;
	static constexpr int32 TORNADOS = 807;
	static constexpr int32 STORMS = 808;
	static constexpr int32 RADIUS = 809;
}

void UCloudManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	WeatherSubsystem = GetWorld()->GetSubsystem<UWeatherSubsystem>();

	if (!WeatherSubsystem.IsValid())
	{
		UE_LOG(LogCloudManager, Error, TEXT("[CloudManager] UWeatherSubsystem introuvable."));
		return;
	}

	if (!CloudActorClass)
	{
		CloudActorClass = ACloudActor::StaticClass();
		UE_LOG(LogCloudManager, Warning, TEXT("[CloudManager] CloudActorClass non assigné."));
	}

	CloudPool.Reserve(CloudPoolSize);
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	for (int32 i = 0; i < CloudPoolSize; ++i)
	{
		ACloudActor* Actor = GetWorld()->SpawnActor<ACloudActor>(
			CloudActorClass, FVector(0.f, 0.f, -100000.f), FRotator::ZeroRotator, SpawnParams);

		if (Actor)
		{
			Actor->DeactivateCloud();
			CloudPool.Add(Actor);
		}
	}

	// On lance un tick dédié pour les nuages (toutes les 0.5 secondes, c'est largement suffisant pour de la météo)
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UCloudManager::TickWrapper), 0.5f);

	UE_LOG(LogCloudManager, Log, TEXT("[CloudManager] Initialisé. Pool=%d"), CloudPool.Num());
}

void UCloudManager::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);

	for (ACloudActor* Actor : CloudPool)
	{
		if (IsValid(Actor)) Actor->Destroy();
	}
	CloudPool.Empty();
	ActiveClouds.Empty();
	Super::Deinitialize();
}

bool UCloudManager::TickWrapper(float DeltaTime)
{
	Tick(DeltaTime);
	return true;
}

void UCloudManager::Tick(float DeltaTime)
{
	if (!WeatherSubsystem.IsValid()) return;

	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* Cam = PC ? PC->PlayerCameraManager : nullptr;
	if (!Cam) return;

	const FVector PlayerLoc = Cam->GetCameraLocation();
	const FVector2D PlayerLoc2D = FVector2D(PlayerLoc.X, PlayerLoc.Y);

	// On récupère la taille d'une cellule depuis la config active
	const float CellSize = WeatherSubsystem->GetConfiguration().DX_km * 100000.f;

	// Coordonnée "grille virtuelle" du joueur
	const int32 PlayerGridX = FMath::RoundToInt(PlayerLoc.X / CellSize);
	const int32 PlayerGridY = FMath::RoundToInt(PlayerLoc.Y / CellSize);

	Metrics = FCloudMetrics();
	TSet<FIntPoint> CellsInRadius;
	CellsInRadius.Reserve(FMath::Square(CloudRenderRadius * 2));

	const float RDistSq = FMath::Square(static_cast<float>(CloudRenderRadius) * CellSize);

	for (int32 dy = -CloudRenderRadius; dy <= CloudRenderRadius; ++dy)
	{
		for (int32 dx = -CloudRenderRadius; dx <= CloudRenderRadius; ++dx)
		{
			FIntPoint GridCell(PlayerGridX + dx, PlayerGridY + dy);

			// Position exacte dans le monde
			FVector CloudWorldPos(GridCell.X * CellSize, GridCell.Y * CellSize, CloudThresholds::BaseCloudAltitude);

			const float DistSq = FVector2D::DistSquared(PlayerLoc2D, FVector2D(CloudWorldPos.X, CloudWorldPos.Y));
			if (DistSq > RDistSq) continue;

			// --- LE CŒUR DU NOUVEAU SYSTÈME ---
			FAtmosphericCell AtmosCell;
			// Si la coordonnée est hors domaine ou pas chargée, on skip
			if (!WeatherSubsystem->GetAtmosphereAtLocation(CloudWorldPos, AtmosCell)) continue;

			// On récupère aussi la colonne pour avoir le CAPE et la pluie
			FAtmosphericColumn Col;
			WeatherSubsystem->GetColumnDiagnostics(CloudWorldPos, Col);

			// Vérification de l'humidité relative
			if (AtmosCell.RelativeHumidity < CloudThresholds::HumidityCumulus) continue;

			CellsInRadius.Add(GridCell);

			const ECloudLOD LOD = ComputeLOD(DistSq);
			const FCloudVisualParams Params = BuildVisualParams(AtmosCell, Col, CloudWorldPos, LOD);

			if (ACloudActor** ExistingActor = ActiveClouds.Find(GridCell))
			{
				(*ExistingActor)->UpdateFromParams(Params);
			}
			else
			{
				ACloudActor* FreeActor = GetFreeActorFromPool();
				if (FreeActor)
				{
					FreeActor->bDebugDraw = bShowCloudSpheres;
					FreeActor->ActivateCloud(Params);
					ActiveClouds.Add(GridCell, FreeActor);
				}
				else { ++Metrics.PoolExhausted; }
			}

			switch (LOD)
			{
				case ECloudLOD::LOD0_Full:     ++Metrics.LOD0Count; break;
				case ECloudLOD::LOD1_Medium:   ++Metrics.LOD1Count; break;
				case ECloudLOD::LOD2_Impostor: ++Metrics.LOD2Count; break;
				default: break;
			}

			if (Params.State == ECloudState::Tornado)     ++Metrics.TornadoCount;
			if (Params.State == ECloudState::StormActive) ++Metrics.StormCount;
		}
	}

	// Nettoyage des nuages hors rayon
	TArray<FIntPoint> ToRemove;
	for (auto& Pair : ActiveClouds)
	{
		if (!CellsInRadius.Contains(Pair.Key)) ToRemove.Add(Pair.Key);
	}
	for (const FIntPoint& Cell : ToRemove)
	{
		if (ACloudActor** Actor = ActiveClouds.Find(Cell)) ReturnActorToPool(*Actor);
		ActiveClouds.Remove(Cell);
	}

	Metrics.TotalActive = ActiveClouds.Num();
	Metrics.PoolFree = 0;
	for (const ACloudActor* Actor : CloudPool)
	{
		if (IsValid(Actor) && !Actor->IsActive()) ++Metrics.PoolFree;
	}

	if (bShowCloudDebugHUD || bShowPoolDebug) DrawCloudDebugHUD();
}

FCloudVisualParams UCloudManager::BuildVisualParams(
	const FAtmosphericCell& Cell,
	const FAtmosphericColumn& Col,
	FVector WorldCenter,
	ECloudLOD LOD) const
{
	FCloudVisualParams P;
	P.WorldCenter = WorldCenter;
	P.LOD = LOD;

	// Traduction des nouvelles variables (W = Updraft, PrecipRate = mm/hr)
	float Updraft = Cell.W;
	float Humidity = Cell.RelativeHumidity;
	float Precip = Col.PrecipRate;

	if (Updraft > CloudThresholds::UpdraftTornado && Col.CAPE > CloudThresholds::CAPETornado)
	{
		P.State = ECloudState::Tornado;
	}
	else if (Humidity >= CloudThresholds::HumidityStorm && Col.CAPE > CloudThresholds::CAPEStorm && Precip > 0.f)
	{
		P.State = ECloudState::StormActive;
	}
	else if (Humidity >= CloudThresholds::HumidityCumulonimbus && Col.CAPE > CloudThresholds::CAPECumulonimbus)
	{
		P.State = ECloudState::Cumulonimbus;
	}
	else
	{
		P.State = ECloudState::Cumulus;
	}

	switch (P.State)
	{
		case ECloudState::Cumulus:
			P.Opacity = FMath::Lerp(CloudThresholds::MinOpacity, 0.75f, Humidity);
			P.Darkness = 0.0f;
			P.VerticalScale = FMath::Lerp(0.6f, 1.2f, Humidity);
			P.HorizontalPuff = FMath::Lerp(0.8f, 1.3f, Humidity);
			break;

		case ECloudState::Cumulonimbus:
			P.Opacity = FMath::Lerp(0.70f, 0.88f, Humidity);
			P.Darkness = FMath::Clamp(Col.CAPE / CloudThresholds::CAPEStorm, 0.f, 0.5f);
			P.VerticalScale = FMath::Lerp(1.2f, CloudThresholds::MaxVerticalScale * 0.6f,
										  (Col.CAPE - CloudThresholds::CAPECumulonimbus) / (CloudThresholds::CAPEStorm - CloudThresholds::CAPECumulonimbus));
			P.HorizontalPuff = 1.4f;
			break;

		case ECloudState::StormActive:
			P.Opacity = FMath::Lerp(0.82f, CloudThresholds::MaxOpacity, Humidity);
			P.Darkness = FMath::Clamp(Col.CAPE / (CloudThresholds::CAPEStorm * 1.5f), 0.3f, 0.75f);
			P.VerticalScale = FMath::Lerp(CloudThresholds::MaxVerticalScale * 0.6f, CloudThresholds::MaxVerticalScale, Updraft / 80.f);
			P.HorizontalPuff = 1.8f;
			P.RainIntensity = FMath::Clamp(Precip / 50.f, 0.f, 1.f);
			P.HailDiameter = (Cell.Hail.MixingRatio > 0.001f) ? 0.05f : 0.f; // Approximation simple
			break;

		case ECloudState::Tornado:
			P.Opacity = CloudThresholds::MaxOpacity;
			P.Darkness = 0.85f;
			P.VerticalScale = CloudThresholds::MaxVerticalScale;
			P.HorizontalPuff = 0.6f;
			P.RainIntensity = 1.0f;
			P.HailDiameter = (Cell.Hail.MixingRatio > 0.001f) ? 0.05f : 0.f;
			P.TornadoStrength = FMath::Clamp((Updraft - CloudThresholds::UpdraftTornado) / 50.f, 0.f, 1.f);
			break;

		default: break;
	}

	// Simplification : altitude de base fixe pour l'instant (0°C dépend du profil)
	P.WorldCenter.Z = CloudThresholds::BaseCloudAltitude;

	return P;
}

ECloudLOD UCloudManager::ComputeLOD(float DistanceFromPlayerSq) const
{
	const float D0Sq = FMath::Square(LODDist0_Full);
	const float D1Sq = FMath::Square(LODDist1_Medium);
	const float D2Sq = FMath::Square(LODDist2_Impostor);

	if (DistanceFromPlayerSq < D0Sq) return ECloudLOD::LOD0_Full;
	if (DistanceFromPlayerSq < D1Sq) return ECloudLOD::LOD1_Medium;
	if (DistanceFromPlayerSq < D2Sq) return ECloudLOD::LOD2_Impostor;
	return ECloudLOD::Hidden;
}

ACloudActor* UCloudManager::GetFreeActorFromPool()
{
	for (ACloudActor* Actor : CloudPool)
	{
		if (IsValid(Actor) && !Actor->IsActive()) return Actor;
	}
	return nullptr;
}

void UCloudManager::ReturnActorToPool(ACloudActor* Actor)
{
	if (IsValid(Actor)) Actor->DeactivateCloud();
}

void UCloudManager::DrawCloudDebugHUD()
{
	if (!GEngine) return;

	if (bShowCloudDebugHUD)
	{
		const FColor HeaderColor = Metrics.PoolExhausted > 0 ? FColor::Red : FColor::White;
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::HEADER, 1.2f, FColor::White, TEXT("═══ CloudManager Debug ═══"));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::ACTIVE, 1.2f, FColor::Cyan, FString::Printf(TEXT("  Nuages actifs   : %d"), Metrics.TotalActive));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::FREE, 1.2f, FColor::Green, FString::Printf(TEXT("  Pool libre      : %d / %d"), Metrics.PoolFree, CloudPoolSize));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::EXHAUSTED, 1.2f, HeaderColor, FString::Printf(TEXT("  Pool épuisé     : %d fois"), Metrics.PoolExhausted));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::LOD0, 1.2f, FColor::Silver, FString::Printf(TEXT("  LOD0 (5 layers) : %d"), Metrics.LOD0Count));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::LOD1, 1.2f, FColor::Silver, FString::Printf(TEXT("  LOD1 (3 layers) : %d"), Metrics.LOD1Count));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::LOD2, 1.2f, FColor::Silver, FString::Printf(TEXT("  LOD2 (impostor) : %d"), Metrics.LOD2Count));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::STORMS, 1.2f, Metrics.StormCount > 0 ? FColor::Cyan : FColor::Silver, FString::Printf(TEXT("  Orages actifs   : %d"), Metrics.StormCount));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::TORNADOS, 1.2f, Metrics.TornadoCount > 0 ? FColor::Red : FColor::Silver, FString::Printf(TEXT("  Tornades actives: %d"), Metrics.TornadoCount));
		GEngine->AddOnScreenDebugMessage(CloudDebugKeys::RADIUS, 1.2f, FColor::Silver, FString::Printf(TEXT("  Rayon rendu     : %d cellules"), CloudRenderRadius));
	}
}