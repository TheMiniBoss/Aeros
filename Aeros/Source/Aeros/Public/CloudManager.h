// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CloudTypes.h"
#include "WeatherTypes.h"
#include "CloudActor.h"
#include "CloudManager.generated.h"

class UWeatherSubsystem;

DECLARE_LOG_CATEGORY_EXTERN(LogCloudManager, Log, All);

// ============================================================
// UCloudManager — Système de gestion des nuages
//
// Abonné à UWeatherSubsystem::OnWeatherTickComplete.
// À chaque tick météo, il :
//   1. Calcule quelles cellules locales sont dans le rayon de rendu
//   2. Détermine l'état visuel (ECloudState) de chaque cellule active
//   3. Attribue/récupère des ACloudActor du pool
//   4. Appelle UpdateFromParams() ou ActivateCloud() selon l'état du pool
//
// Pool :
//   Taille fixe (CloudPoolSize) pré-allouée au Initialize().
//   Les acteurs sont déplacés et réactivés, jamais détruits en jeu.
//
// LOD :
//   Distances configurables en cm depuis le joueur :
//   < LODDist0 → LOD0_Full
//   < LODDist1 → LOD1_Medium
//   < LODDist2 → LOD2_Impostor
//   au-delà    → Hidden (acteur retourné au pool)
//
// Debug :
//   bShowCloudDebugHUD  : panneau de stats à l'écran
//   bShowPoolDebug      : état du pool (actifs/libres/skippés)
// ============================================================
UCLASS()
class AEROS_API UCloudManager : public UWorldSubsystem
{
	GENERATED_BODY()

	TWeakObjectPtr<UWeatherSubsystem> WeatherSubsystem;

	UPROPERTY()
	TArray<TObjectPtr<ACloudActor>> CloudPool;

	TMap<FIntPoint, ACloudActor*> ActiveClouds;

	// Remplacement du Delegate météo par un Ticker interne
	FTSTicker::FDelegateHandle TickHandle;

	struct FCloudMetrics
	{
		int32 TotalActive = 0;
		int32 PoolFree = 0;
		int32 PoolExhausted = 0;
		int32 LOD0Count = 0;
		int32 LOD1Count = 0;
		int32 LOD2Count = 0;
		int32 TornadoCount = 0;
		int32 StormCount = 0;
	} Metrics;

public:
	UPROPERTY(EditAnywhere, Category = "Cloud|Config")
	int32 CloudRenderRadius = 20;

	UPROPERTY(EditAnywhere, Category = "Cloud|Config")
	int32 CloudPoolSize = 400;

	UPROPERTY(EditAnywhere, Category = "Cloud|LOD")
	float LODDist0_Full = 80000.f;
	UPROPERTY(EditAnywhere, Category = "Cloud|LOD")
	float LODDist1_Medium = 200000.f;
	UPROPERTY(EditAnywhere, Category = "Cloud|LOD")
	float LODDist2_Impostor = 400000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud|Debug")
	bool bShowCloudDebugHUD = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud|Debug")
	bool bShowPoolDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud|Debug")
	bool bShowCloudSpheres = false;

	UPROPERTY(EditAnywhere, Category = "Cloud|Config")
	TSubclassOf<ACloudActor> CloudActorClass;

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	const FCloudMetrics& GetMetrics() const { return Metrics; }

private:
	// Nouveau système de Tick (appelé via le Ticker d'Unreal)
	bool TickWrapper(float DeltaTime);
	void Tick(float DeltaTime);

	// On utilise maintenant FAtmosphericCell et FAtmosphericColumn
	FCloudVisualParams BuildVisualParams(
		const FAtmosphericCell& Cell,
		const FAtmosphericColumn& Col,
		FVector WorldCenter,
		ECloudLOD LOD) const;

	ECloudLOD ComputeLOD(float DistanceFromPlayerSq) const;
	ACloudActor* GetFreeActorFromPool();
	void ReturnActorToPool(ACloudActor* Actor);
	void DrawCloudDebugHUD();
};