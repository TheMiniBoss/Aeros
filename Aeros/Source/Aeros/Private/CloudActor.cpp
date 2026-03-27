// Fill out your copyright notice in the Description page of Project Settings.

#include "CloudActor.h"
#include "Components/StaticMeshComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogCloudActor, Log, All);

// ============================================================
// Noms des paramètres matériaux (centralisés pour éviter les typos)
// ============================================================
namespace CloudMatParams
{
	static const FName Opacity = TEXT("CloudOpacity");
	static const FName Darkness = TEXT("CloudDarkness");
	static const FName Scale = TEXT("CloudScale");
	static const FName Turbulence = TEXT("CloudTurbulence");
	static const FName Tint = TEXT("CloudTint");
}

namespace CloudNiagaraParams
{
	static const FName RainIntensity = TEXT("User.RainIntensity");
	static const FName HailDiameter = TEXT("User.HailDiameter");
	static const FName TornadoStrength = TEXT("User.TornadoStrength");
}

// ============================================================
// Construction
// ============================================================
ACloudActor::ACloudActor()
{
	// Pas de tick par défaut : activé uniquement pour LOD2 (impostor camera-facing)
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// Pas de réplication : les nuages sont entièrement locaux/esthétiques
	bReplicates = false;

	// ── Root ──────────────────────────────────────────────────
	CloudRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CloudRoot"));
	SetRootComponent(CloudRoot);

	// ── Sprite layers ─────────────────────────────────────────
	// On crée MAX_SPRITE_LAYERS plans, disposés en étoile autour de l'axe Z.
	// Angles : 0°, 36°, 72°, 108°, 144° (répartition sur 180° car un plan à 0° et 180° sont identiques)
	SpriteLayers.SetNum(MAX_SPRITE_LAYERS);

	const float AngleStep = 180.f / static_cast<float>(MAX_SPRITE_LAYERS);

	for (int32 i = 0; i < MAX_SPRITE_LAYERS; ++i)
	{
		FString CompName = FString::Printf(TEXT("SpriteLayer_%d"), i);
		UStaticMeshComponent* Layer = CreateDefaultSubobject<UStaticMeshComponent>(*CompName);
		Layer->SetupAttachment(CloudRoot);

		// Rotation autour de Z : étoile de plans
		Layer->SetRelativeRotation(FRotator(0.f, AngleStep * i, 0.f));

		// Décalage vertical léger pour l'effet bouffi (couches légèrement décalées)
		Layer->SetRelativeLocation(FVector(0.f, 0.f, LayerVerticalSpread * i));

		// Paramètres de rendu
		Layer->SetCastShadow(true);
		Layer->SetReceivesDecals(false);
		Layer->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		// Caché par défaut (l'acteur démarre dans le pool, inactif)
		Layer->SetVisibility(false);
		Layer->SetHiddenInGame(true);

		SpriteLayers[i] = Layer;
	}

	// ── VFX Pluie ─────────────────────────────────────────────
	RainVFX = CreateDefaultSubobject<UNiagaraComponent>(TEXT("RainVFX"));
	RainVFX->SetupAttachment(CloudRoot);
	RainVFX->SetAutoActivate(false);

	// ── VFX Tornade ───────────────────────────────────────────
	TornadoVFX = CreateDefaultSubobject<UNiagaraComponent>(TEXT("TornadoVFX"));
	TornadoVFX->SetupAttachment(CloudRoot);
	TornadoVFX->SetAutoActivate(false);

	// ── Mesh par défaut (plane basique d'Unreal) ───────────────
	// On tente de charger le plane basique. Si l'asset personnalisé est
	// assigné dans le Blueprint, il override cette valeur.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> DefaultPlaneMesh(
		TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (DefaultPlaneMesh.Succeeded())
	{
		CloudMeshAsset = DefaultPlaneMesh.Object;
	}
}

// ============================================================
// BeginPlay
// ============================================================
void ACloudActor::BeginPlay()
{
	Super::BeginPlay();

	// Assigner le mesh à tous les layers
	if (CloudMeshAsset)
	{
		for (UStaticMeshComponent* Layer : SpriteLayers)
		{
			if (Layer) Layer->SetStaticMesh(CloudMeshAsset);
		}
	}
	else
	{
		UE_LOG(LogCloudActor, Warning, TEXT("[CloudActor] CloudMeshAsset non assigné. Assigne-le dans le Blueprint de la classe."));
	}

	// Créer le Material Instance Dynamic depuis le matériau de base
	if (CloudMaterialBase)
	{
		CloudMID = UMaterialInstanceDynamic::Create(CloudMaterialBase, this);
		for (UStaticMeshComponent* Layer : SpriteLayers)
		{
			if (Layer) Layer->SetMaterial(0, CloudMID);
		}
	}
	else
	{
		UE_LOG(LogCloudActor, Warning, TEXT("[CloudActor] CloudMaterialBase non assigné. Assigne M_Cloud_Translucent dans le Blueprint."));
	}

	// Assigner les systèmes Niagara
	if (RainSystemAsset && RainVFX)
	{
		RainVFX->SetAsset(RainSystemAsset);
	}
	if (TornadoSystemAsset && TornadoVFX)
	{
		TornadoVFX->SetAsset(TornadoSystemAsset);
	}
}

// ============================================================
// Tick — Uniquement actif en LOD2 (impostor camera-facing)
// ============================================================
void ACloudActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	FaceCamera();
}

// ============================================================
// API Pool — ActivateCloud
// ============================================================
void ACloudActor::ActivateCloud(const FCloudVisualParams& Params)
{
	bIsActive = true;
	CurrentParams = Params;

	// Positionner l'acteur
	SetActorLocation(Params.WorldCenter);

	// Appliquer le LOD (visibility des layers)
	ApplyLOD(Params.LOD);

	// Paramètres matériaux
	RefreshMaterial();

	// VFX
	switch (Params.State)
	{
		case ECloudState::StormActive:
			UpdateRainVFX(Params.RainIntensity, Params.HailDiameter);
			break;
		case ECloudState::Tornado:
			UpdateRainVFX(Params.RainIntensity, Params.HailDiameter);
			UpdateTornadoVFX(Params.TornadoStrength);
			break;
		default:
			DisableAllVFX();
			break;
	}

	// Activer le tick seulement pour LOD2 (camera-facing impostor)
	SetActorTickEnabled(Params.LOD == ECloudLOD::LOD2_Impostor);
}

// ============================================================
// API Pool — DeactivateCloud
// ============================================================
void ACloudActor::DeactivateCloud()
{
	bIsActive = false;

	// Cacher tous les layers
	for (UStaticMeshComponent* Layer : SpriteLayers)
	{
		if (Layer)
		{
			Layer->SetVisibility(false);
			Layer->SetHiddenInGame(true);
		}
	}

	DisableAllVFX();
	SetActorTickEnabled(false);

	// Déplacer hors de la scène visible (évite tout rendu fantôme)
	SetActorLocation(FVector(0.f, 0.f, -100000.f));
}

// ============================================================
// UpdateFromParams — Mise à jour d'un nuage déjà actif
// ============================================================
void ACloudActor::UpdateFromParams(const FCloudVisualParams& Params)
{
	const bool bLODChanged = Params.LOD != CurrentParams.LOD;
	const bool bStateChanged = Params.State != CurrentParams.State;

	CurrentParams = Params;
	SetActorLocation(Params.WorldCenter);

	if (bLODChanged)
	{
		ApplyLOD(Params.LOD);
		// LOD2 → tick pour camera-facing; autre LOD → pas de tick
		SetActorTickEnabled(Params.LOD == ECloudLOD::LOD2_Impostor);
	}

	RefreshMaterial();

	if (bStateChanged)
	{
		switch (Params.State)
		{
			case ECloudState::StormActive:
				UpdateRainVFX(Params.RainIntensity, Params.HailDiameter);
				if (TornadoVFX) TornadoVFX->Deactivate();
				break;
			case ECloudState::Tornado:
				UpdateRainVFX(Params.RainIntensity, Params.HailDiameter);
				UpdateTornadoVFX(Params.TornadoStrength);
				break;
			default:
				DisableAllVFX();
				break;
		}
	}
	else
	{
		// Même état : mettre à jour les intensités VFX sans restarting le système
		if (Params.State == ECloudState::StormActive || Params.State == ECloudState::Tornado)
		{
			if (RainVFX && RainVFX->IsActive())
			{
				RainVFX->SetFloatParameter(CloudNiagaraParams::RainIntensity, Params.RainIntensity);
				RainVFX->SetFloatParameter(CloudNiagaraParams::HailDiameter, Params.HailDiameter);
			}
			if (Params.State == ECloudState::Tornado && TornadoVFX && TornadoVFX->IsActive())
			{
				TornadoVFX->SetFloatParameter(CloudNiagaraParams::TornadoStrength, Params.TornadoStrength);
			}
		}
	}
}

// ============================================================
// ApplyLOD — Active le bon nombre de couches
// ============================================================
void ACloudActor::ApplyLOD(ECloudLOD LOD)
{
	// Nombre de couches actives par LOD
	int32 ActiveLayers = 0;
	switch (LOD)
	{
		case ECloudLOD::LOD0_Full:     ActiveLayers = 5; break;
		case ECloudLOD::LOD1_Medium:   ActiveLayers = 3; break;
		case ECloudLOD::LOD2_Impostor: ActiveLayers = 1; break;
		case ECloudLOD::Hidden:        ActiveLayers = 0; break;
	}

	for (int32 i = 0; i < MAX_SPRITE_LAYERS; ++i)
	{
		if (!SpriteLayers[i]) continue;
		const bool bVisible = (i < ActiveLayers);
		SpriteLayers[i]->SetVisibility(bVisible);
		SpriteLayers[i]->SetHiddenInGame(!bVisible);
	}

	// En LOD2, le plan unique est un impostor horizontal (RY=-90 pour que la face soit vers le haut)
	// On le remet vertical ensuite via FaceCamera() chaque frame.
	if (LOD == ECloudLOD::LOD2_Impostor && SpriteLayers[0])
	{
		SpriteLayers[0]->SetRelativeRotation(FRotator(90.f, 0.f, 0.f)); // face vers le ciel par défaut
	}
}

// ============================================================
// FaceCamera — LOD2 : orient le plan vers la caméra (billboarding)
// ============================================================
void ACloudActor::FaceCamera()
{
	if (!SpriteLayers.IsValidIndex(0) || !SpriteLayers[0]) return;

	const UWorld* World = GetWorld();
	if (!World) return;

	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager) return;

	const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
	const FVector CloudLoc = GetActorLocation();
	const FVector Dir = (CamLoc - CloudLoc).GetSafeNormal2D(); // direction horizontale seule

	if (!Dir.IsNearlyZero())
	{
		// On fait pivoter le plan autour de Z pour faire face à la caméra
		const FRotator FaceRot = FRotationMatrix::MakeFromX(Dir).Rotator();
		SpriteLayers[0]->SetWorldRotation(FRotator(0.f, FaceRot.Yaw, 0.f));
	}
}

// ============================================================
// RefreshMaterial — Met à jour les paramètres du MID et le scale 3D
// ============================================================
void ACloudActor::RefreshMaterial()
{
	if (!CloudMID) return;

	// ── Scale 3D des plans ─────────────────────────────────────
	// UnrealEngine plane = 100x100cm à scale 1.0.
	// On veut BaseSpriteSize en cm de côté, donc on scale en conséquence.
	const float HorizScale = (BaseSpriteSize * CurrentParams.HorizontalPuff) / 100.f;
	const float VertScale = (BaseSpriteSize * CurrentParams.VerticalScale) / 100.f;

	for (int32 i = 0; i < MAX_SPRITE_LAYERS; ++i)
	{
		if (!SpriteLayers[i] || !SpriteLayers[i]->IsVisible()) continue;

		// En LOD2, le plan unique reste carré (impostor pleine face)
		if (CurrentParams.LOD == ECloudLOD::LOD2_Impostor)
		{
			SpriteLayers[i]->SetRelativeScale3D(FVector(HorizScale, HorizScale, HorizScale));
		}
		else
		{
			// Plans verticaux : largeur = HorizScale, hauteur = VertScale
			// La plane d'Unreal est dans XY, donc "hauteur" = Y quand le plan est dressé à 90° en pitch
			SpriteLayers[i]->SetRelativeScale3D(FVector(HorizScale, HorizScale, VertScale));
		}
	}

	// ── Tint selon l'état ──────────────────────────────────────
	FLinearColor Tint = FLinearColor::White;
	switch (CurrentParams.State)
	{
		case ECloudState::Cumulus:
			Tint = FLinearColor(0.95f, 0.95f, 0.95f, 1.f);
			break;
		case ECloudState::Cumulonimbus:
			Tint = FLinearColor(0.65f, 0.65f, 0.70f, 1.f);
			break;
		case ECloudState::StormActive:
			Tint = FLinearColor(0.35f, 0.35f, 0.40f, 1.f);
			break;
		case ECloudState::Tornado:
			Tint = FLinearColor(0.25f, 0.25f, 0.30f, 1.f);
			break;
		default:
			break;
	}

	// ── Paramètres scalaires ───────────────────────────────────
	CloudMID->SetScalarParameterValue(CloudMatParams::Opacity, CurrentParams.Opacity);
	CloudMID->SetScalarParameterValue(CloudMatParams::Darkness, CurrentParams.Darkness);
	CloudMID->SetScalarParameterValue(CloudMatParams::Scale, HorizScale);
	CloudMID->SetScalarParameterValue(CloudMatParams::Turbulence,
									  CurrentParams.State >= ECloudState::Cumulonimbus ? 0.6f : 0.1f);
	CloudMID->SetVectorParameterValue(CloudMatParams::Tint, Tint);

	// ── Debug ──────────────────────────────────────────────────
	if (bDebugDraw)
	{
		const FColor DebugColor = [this]() -> FColor
		{
			switch (CurrentParams.State)
			{
				case ECloudState::Cumulus:      return FColor::White;
				case ECloudState::Cumulonimbus: return FColor::Silver;
				case ECloudState::StormActive:  return FColor::Cyan;
				case ECloudState::Tornado:      return FColor::Red;
				default:                         return FColor::Green;
			}
		}();

		DrawDebugSphere(GetWorld(), GetActorLocation(),
						BaseSpriteSize * 0.5f * CurrentParams.HorizontalPuff,
						8, DebugColor, false, 1.1f, 0, 2.f);
	}
}

// ============================================================
// UpdateRainVFX
// ============================================================
void ACloudActor::UpdateRainVFX(float Intensity, float HailDiameter)
{
	if (!RainVFX || !RainSystemAsset) return;

	if (Intensity > 0.01f)
	{
		if (!RainVFX->IsActive()) RainVFX->Activate(true);
		RainVFX->SetFloatParameter(CloudNiagaraParams::RainIntensity, Intensity);
		RainVFX->SetFloatParameter(CloudNiagaraParams::HailDiameter, HailDiameter);
	}
	else
	{
		if (RainVFX->IsActive()) RainVFX->Deactivate();
	}
}

// ============================================================
// UpdateTornadoVFX
// ============================================================
void ACloudActor::UpdateTornadoVFX(float Strength)
{
	if (!TornadoVFX || !TornadoSystemAsset) return;

	if (Strength > 0.01f)
	{
		if (!TornadoVFX->IsActive()) TornadoVFX->Activate(true);
		TornadoVFX->SetFloatParameter(CloudNiagaraParams::TornadoStrength, Strength);
	}
	else
	{
		if (TornadoVFX->IsActive()) TornadoVFX->Deactivate();
	}
}

// ============================================================
// DisableAllVFX
// ============================================================
void ACloudActor::DisableAllVFX()
{
	if (RainVFX && RainVFX->IsActive())   RainVFX->Deactivate();
	if (TornadoVFX && TornadoVFX->IsActive()) TornadoVFX->Deactivate();
}