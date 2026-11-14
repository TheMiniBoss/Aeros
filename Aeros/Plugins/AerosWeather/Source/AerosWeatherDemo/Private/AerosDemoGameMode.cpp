// Copyright Aeros. All Rights Reserved.

#include "AerosDemoGameMode.h"
#include "AerosDemoPawn.h"
#include "AerosDemoHUD.h"
#include "AerosWeatherController.h"
#include "AerosWeatherRadar.h"
#include "AerosVolcano.h"
#include "AerosDemoTerrain.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Misc/PackageName.h"

namespace
{
	/** LoadObject without "failed to find" warnings when an optional asset was not generated. */
	template <typename T>
	T* LoadOptional(const TCHAR* Path)
	{
		const FSoftObjectPath SoftPath(Path);
		return FPackageName::DoesPackageExist(SoftPath.GetLongPackageName()) ? LoadObject<T>(nullptr, Path) : nullptr;
	}
}

AAerosDemoGameMode::AAerosDemoGameMode()
{
	DefaultPawnClass = AAerosDemoPawn::StaticClass();
	HUDClass = AAerosDemoHUD::StaticClass();

	// A lively default: a "loaded gun" spring afternoon over the plains, with an ocean to the east
	DemoSettings.GridSizeX = 160;
	DemoSettings.GridSizeY = 160;
	DemoSettings.CellSizeKm = 2.f;
	DemoSettings.Scenario = EAerosScenario::SupercellOutbreak;
	DemoSettings.StartDayOfYear = 140;
	DemoSettings.StartHour = 12.f;
	DemoSettings.LatitudeDeg = 35.f;
	DemoSettings.TimeScale = 60.f;
	// Coast 70 km east (tsunamis, storm surge), a desert 120 km west (sand storms),
	// a seismic zone around the start, and the ground 1.5 m above the sea
	FAerosWaterRegion Ocean;
	Ocean.Center = FVector(27000000.0, 0.0, 0.0);
	Ocean.RadiusKm = 200.f;
	Ocean.ShoreWidthKm = 4.f;
	DemoSettings.WaterRegions.Add(Ocean);
	FAerosDesertRegion Desert;
	Desert.Center = FVector(-12000000.0, 2000000.0, 0.0);
	Desert.RadiusKm = 55.f;
	DemoSettings.DesertRegions.Add(Desert);
	FAerosSeismicZone Fault;
	Fault.Center = FVector(3000000.0, 0.0, 0.0);
	Fault.RadiusKm = 60.f;
	Fault.EventsPerDay = 2.f;
	Fault.MaxMagnitude = 7.3f;
	DemoSettings.SeismicZones.Add(Fault);
	DemoSettings.SeaLevelZ = -150.f;
}

void AAerosDemoGameMode::StartPlay()
{
	if (bBuildSceneIfMissing) BuildScene();
	Super::StartPlay();
}

void AAerosDemoGameMode::BuildScene()
{
	UWorld* World = GetWorld();
	if (!World) return;
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	auto Has = [World](UClass* Cls)
	{
		for (TActorIterator<AActor> It(World, Cls); It; ++It) return true;
		return false;
	};
	auto HasComponent = [World](UClass* Cls)
	{
		for (TActorIterator<AActor> It(World); It; ++It) if (It->FindComponentByClass(Cls)) return true;
		return false;
	};

	if (!Has(ADirectionalLight::StaticClass()))
	{
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 100000), FRotator(-45, 0, 0), P);
		if (Sun)
		{
			UDirectionalLightComponent* L = Cast<UDirectionalLightComponent>(Sun->GetLightComponent());
			L->SetMobility(EComponentMobility::Movable);
			L->SetIntensity(10.f);
			L->SetAtmosphereSunLight(true);
			L->SetCastShadows(true);
		}
	}

	if (!HasComponent(USkyAtmosphereComponent::StaticClass()))
	{
		if (AActor* Sky = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, P))
		{
			USkyAtmosphereComponent* C = NewObject<USkyAtmosphereComponent>(Sky, TEXT("SkyAtmosphere"));
			Sky->SetRootComponent(C);
			C->RegisterComponent();
		}
	}

	if (!Has(ASkyLight::StaticClass()))
	{
		if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 50000), FRotator::ZeroRotator, P))
		{
			USkyLightComponent* C = Sky->GetLightComponent();
			C->SetMobility(EComponentMobility::Movable);
			C->bRealTimeCapture = true;
			C->RecaptureSky();
		}
	}

	if (!Has(AExponentialHeightFog::StaticClass()))
	{
		if (AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(FVector::ZeroVector, FRotator::ZeroRotator, P))
		{
			Fog->GetComponent()->SetFogDensity(0.01f);
			Fog->GetComponent()->SetVolumetricFog(true);
		}
	}

	UMaterialInterface* CloudMat = LoadOptional<UMaterialInterface>(TEXT("/AerosWeather/Materials/M_AerosVolumetricCloud.M_AerosVolumetricCloud"));
	if (!HasComponent(UVolumetricCloudComponent::StaticClass()))
	{
		if (AActor* CloudActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, P))
		{
			UVolumetricCloudComponent* C = NewObject<UVolumetricCloudComponent>(CloudActor, TEXT("VolumetricCloud"));
			CloudActor->SetRootComponent(C);
			if (!CloudMat)
			{
				// Engine default clouds (not driven by the simulation; run aeros_setup.py for the weather-driven material)
				C->SetMaterial(LoadOptional<UMaterialInterface>(TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst")));
			}
			C->RegisterComponent();
		}
	}

	// Ground. Default: the procedural landscape. A level with its own ground keeps it, except the
	// engine's flat Plane mesh (old demo map), which is replaced.
	FAerosSimulationSettings TerrainSettings = DemoSettings;
	for (TActorIterator<AAerosWeatherController> It(World); It; ++It) { TerrainSettings = It->Settings; break; }
	const FVector VolcanoXY(-1800000.0, -1800000.0, 0.0);
	FHitResult Hit;
	// Object-type query: only real static geometry counts (the pawn already exists here and blocks every channel)
	const bool bGround = World->LineTraceSingleByObjectType(Hit, FVector(0, 0, 100000), FVector(0, 0, -100000), FCollisionObjectQueryParams(ECC_WorldStatic));
	AStaticMeshActor* FlatPlane = nullptr;
	if (bGround)
	{
		AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Hit.GetActor());
		const UStaticMesh* Mesh = SMA ? SMA->GetStaticMeshComponent()->GetStaticMesh() : nullptr;
		if (Mesh && Mesh->GetPathName() == TEXT("/Engine/BasicShapes/Plane.Plane")) FlatPlane = SMA;
	}
	if (bProceduralTerrain && (!bGround || FlatPlane))
	{
		if (FlatPlane) FlatPlane->Destroy();
		Terrain = World->SpawnActor<AAerosDemoTerrain>(FVector::ZeroVector, FRotator::ZeroRotator, P);
		if (Terrain)
		{
			Terrain->Configure(TerrainSettings);
			if (bSpawnVolcano) Terrain->AddPad(VolcanoXY, 7000.f, 4000.f); // flat base for the cone
			FVector Start(0, 0, 0);
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				if (APawn* Pawn = PC->GetPawn()) Start = Pawn->GetActorLocation();
			}
			Terrain->BuildNow(Start);
		}
		SpawnProps();
		UE_LOG(LogTemp, Display, TEXT("Aeros demo: procedural landscape%s, ground at the origin Z=%.0f"),
			FlatPlane ? TEXT(" (replaced the flat plane)") : TEXT(""), GroundZ(0, 0));
	}
	else if (!bGround)
	{
		// Flat fallback: a 600 km plane (the whole domain, coast 70 km east)
		if (AStaticMeshActor* Ground = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, P))
		{
			Ground->SetMobility(EComponentMobility::Movable);
			UStaticMeshComponent* M = Ground->GetStaticMeshComponent();
			M->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
			M->SetWorldScale3D(FVector(600000.f, 600000.f, 1.f)); // 600 km: covers the whole simulated domain
			if (UMaterialInterface* GroundMat = LoadOptional<UMaterialInterface>(TEXT("/AerosWeather/Materials/M_AerosGround.M_AerosGround")))
			{
				M->SetMaterial(0, GroundMat);
				// The plane has no vertex colours (read as white = sand + rock + snow): plain grass
				if (UMaterialInstanceDynamic* MID = M->CreateDynamicMaterialInstance(0)) MID->SetScalarParameterValue(TEXT("TerrainMasks"), 0.f);
			}
			else if (UMaterialInstanceDynamic* MID = M->CreateDynamicMaterialInstance(0))
			{
				MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.12f, 0.2f, 0.07f));
			}
		}
		SpawnProps();
		UE_LOG(LogTemp, Display, TEXT("Aeros demo: no ground under the origin, spawned a 600 km ground plane and props"));
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("Aeros demo: ground found under the origin: %s at Z=%.0f"), *GetNameSafe(Hit.GetActor()), Hit.ImpactPoint.Z);
	}

	bool bHasController = false;
	for (TActorIterator<AAerosWeatherController> It(World); It; ++It) { bHasController = true; break; }
	if (!bHasController)
	{
		AAerosWeatherController* Controller = World->SpawnActorDeferred<AAerosWeatherController>(AAerosWeatherController::StaticClass(), FTransform::Identity);
		if (Controller)
		{
			Controller->Settings = DemoSettings;
			Controller->CloudMaterial = CloudMat;
			Controller->FinishSpawning(FTransform::Identity);
		}
	}

	bool bHasVolcano = false;
	for (TActorIterator<AAerosVolcano> It(World); It; ++It) { bHasVolcano = true; break; }
	if (!bHasVolcano && bSpawnVolcano)
	{
		// 25 km north-west: a 1.8 km high cone (fallback mountain) that can erupt, on its flat pad
		const FVector At(VolcanoXY.X, VolcanoXY.Y, GroundZ(VolcanoXY.X, VolcanoXY.Y));
		AAerosVolcano* V = World->SpawnActorDeferred<AAerosVolcano>(AAerosVolcano::StaticClass(), FTransform(At));
		if (V)
		{
			V->bFallbackCone = true;
			V->MaxVEI = 6;
			V->FinishSpawning(FTransform(At));
		}
	}

	bool bHasRadar = false;
	for (TActorIterator<AAerosWeatherRadar> It(World); It; ++It) { bHasRadar = true; break; }
	if (!bHasRadar) World->SpawnActor<AAerosWeatherRadar>(FVector::ZeroVector, FRotator::ZeroRotator, P);
}

void AAerosDemoGameMode::SpawnProps()
{
	UWorld* World = GetWorld();
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!World || !Cube) return;
	FRandomStream Rand(42);
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	for (int32 K = 0; K < NumPhysicsProps; ++K)
	{
		// Rings of crates and small stacks within 3 km
		const float A = Rand.FRandRange(0.f, 2.f * PI);
		const float R = Rand.FRandRange(20000.f, 300000.f);
		const int32 Stack = Rand.RandRange(1, 3);
		for (int32 S = 0; S < Stack; ++S)
		{
			const float Size = Rand.FRandRange(0.6f, 2.5f);
			const double X = FMath::Cos(A) * R, Y = FMath::Sin(A) * R;
			const FVector Loc(X, Y, GroundZ(X, Y) + 60.f * Size + S * 105.f * Size);
			AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Loc, FRotator(0, Rand.FRandRange(0.f, 90.f), 0), P);
			if (!Prop) continue;
			Prop->SetMobility(EComponentMobility::Movable);
			UStaticMeshComponent* M = Prop->GetStaticMeshComponent();
			M->SetStaticMesh(Cube);
			M->SetWorldScale3D(FVector(Size));
			M->SetSimulatePhysics(true);
			M->SetMassOverrideInKg(NAME_None, 40.f * Size * Size * Size, true);
			if (UMaterialInstanceDynamic* MID = M->CreateDynamicMaterialInstance(0))
			{
				MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.45f + 0.3f * Rand.FRand(), 0.3f, 0.15f));
			}
		}
	}
}

double AAerosDemoGameMode::GroundZ(double X, double Y) const
{
	return Terrain ? Terrain->GetGroundZ(X, Y) : 0.0;
}
