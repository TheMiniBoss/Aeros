// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CloudTypes.h"
#include "CloudActor.generated.h"

class UStaticMeshComponent;
class UNiagaraComponent;
class UMaterialInstanceDynamic;

// ============================================================
// ACloudActor — Représentation visuelle d'un nuage
//
// Technique : Sprites croisés (Cross-Billboard)
//   Plusieurs plans UStaticMeshComponent sont disposés en étoile
//   autour de l'axe vertical (Z). Vus sous n'importe quel angle
//   horizontal, ils donnent l'illusion d'un volume 3D.
//
//   LOD0 : 5 plans (couverture angulaire maximale, 36° entre chaque)
//   LOD1 : 3 plans (60° entre chaque)
//   LOD2 : 1 plan  (impostor pur, orienté dynamiquement vers la caméra)
//
// Rendu :
//   Chaque plan utilise un UMaterialInstanceDynamic partagé.
//   Paramètres exposés au matériau :
//     "CloudOpacity"     float  [0,1]
//     "CloudDarkness"    float  [0,1]   → noirceur de la base
//     "CloudScale"       float          → scale horizontal unifié
//     "CloudTurbulence"  float  [0,1]   → bruit de bord (Cumulonimbus)
//
// Pool :
//   Géré par UCloudManager. Un acteur peut être réactivé sur une
//   nouvelle cellule via UpdateFromParams(), sans jamais être détruit.
//   Quand il est inactif, tous ses composants sont désactivés (pas de Tick).
//
// Setup Blueprint requis :
//   - Assigner CloudMeshAsset (SM_Plane ou plane personnalisée)
//   - Assigner CloudMaterialBase (M_Cloud_Translucent)
//   - Assigner RainSystemAsset  (NS_CloudRain)
//   - Assigner TornadoSystemAsset (NS_CloudTornado)
// ============================================================
UCLASS()
class AEROS_API ACloudActor : public AActor
{
	GENERATED_BODY()

	// ── Composants Sprites ─────────────────────────────────────
	// Pré-alloués au maximum (LOD0 = 5). On en montre 1, 3 ou 5 selon le LOD.
	static constexpr int32 MAX_SPRITE_LAYERS = 5;

	UPROPERTY()
	TObjectPtr<USceneComponent> CloudRoot;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> SpriteLayers;

	// ── Composants VFX ─────────────────────────────────────────
	UPROPERTY()
	TObjectPtr<UNiagaraComponent> RainVFX;

	UPROPERTY()
	TObjectPtr<UNiagaraComponent> TornadoVFX;

	// ── Material Instance Dynamic ───────────────────────────────
	// Un seul MID partagé entre tous les layers du même acteur.
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> CloudMID;

	// ── État courant ────────────────────────────────────────────
	FCloudVisualParams CurrentParams;
	bool bIsActive = false;

public:
	// ── Assets à assigner (Blueprint ou code) ──────────────────

	// Le mesh plan utilisé pour chaque sprite layer.
	// Par défaut : /Engine/BasicShapes/Plane.Plane
	UPROPERTY(EditDefaultsOnly, Category = "Cloud|Assets")
	TObjectPtr<UStaticMesh> CloudMeshAsset;

	// Matériau translucide de base (sera converti en MID au BeginPlay).
	// Doit exposer les paramètres : CloudOpacity, CloudDarkness, CloudScale, CloudTurbulence.
	UPROPERTY(EditDefaultsOnly, Category = "Cloud|Assets")
	TObjectPtr<UMaterialInterface> CloudMaterialBase;

	// Système Niagara pluie (paramètre : User.RainIntensity, User.HailDiameter).
	UPROPERTY(EditDefaultsOnly, Category = "Cloud|Assets")
	TObjectPtr<class UNiagaraSystem> RainSystemAsset;

	// Système Niagara tornade (paramètre : User.TornadoStrength).
	UPROPERTY(EditDefaultsOnly, Category = "Cloud|Assets")
	TObjectPtr<class UNiagaraSystem> TornadoSystemAsset;

	// ── Paramètres de rendu ─────────────────────────────────────

	// Taille de base d'un sprite (cm). Sera scaled par HorizontalPuff et VerticalScale.
	UPROPERTY(EditDefaultsOnly, Category = "Cloud|Render")
	float BaseSpriteSize = 2500.f;

	// Décalage vertical entre deux couches successives (pour l'effet bouffi).
	UPROPERTY(EditDefaultsOnly, Category = "Cloud|Render")
	float LayerVerticalSpread = 300.f;

	// Debug : dessine une sphère de debug pendant la durée de vie du nuage.
	UPROPERTY(EditAnywhere, Category = "Cloud|Debug")
	bool bDebugDraw = false;

public:
	ACloudActor();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

public:
	// ── API Pool ────────────────────────────────────────────────

	// Réactive l'acteur sur une nouvelle cellule et applique les paramètres visuels.
	// Appelé par UCloudManager quand une cellule inactive devient un nuage.
	void ActivateCloud(const FCloudVisualParams& Params);

	// Désactive tous les composants. L'acteur reste en mémoire pour le pool.
	void DeactivateCloud();

	// Met à jour les paramètres visuels sur un nuage déjà actif
	// (ex : le nuage change d'état mais reste sur la même cellule).
	void UpdateFromParams(const FCloudVisualParams& Params);

	bool IsActive() const { return bIsActive; }
	const FCloudVisualParams& GetCurrentParams() const { return CurrentParams; }

private:
	// ── Helpers internes ────────────────────────────────────────

	// Applique le LOD : active/désactive les couches selon le niveau.
	void ApplyLOD(ECloudLOD LOD);

	// Réoriente le layer unique (LOD2 impostor) vers la caméra.
	void FaceCamera();

	// Met à jour les paramètres du MID et le scale 3D des layers.
	void RefreshMaterial();

	// Active/configure le VFX pluie.
	void UpdateRainVFX(float Intensity, float HailDiameter);

	// Active/configure le VFX tornade.
	void UpdateTornadoVFX(float Strength);

	// Désactive les deux VFX.
	void DisableAllVFX();
};