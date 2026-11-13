// Copyright Aeros. All Rights Reserved.

#include "AerosWeatherController.h"
#include "AerosWeatherSubsystem.h"
#include "AerosCloudManager.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

AAerosWeatherController::AAerosWeatherController()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	bReplicates = false; // the simulation is synchronised by AAerosWeatherNetState
}

void AAerosWeatherController::FindSceneActors()
{
	UWorld* World = GetWorld();
	if (!World) return;
	if (!SunLight) { for (TActorIterator<ADirectionalLight> It(World); It; ++It) { SunLight = *It; break; } }
	if (!HeightFog) { for (TActorIterator<AExponentialHeightFog> It(World); It; ++It) { HeightFog = *It; break; } }
	if (!SkyLight) { for (TActorIterator<ASkyLight> It(World); It; ++It) { SkyLight = *It; break; } }
	if (!VolumetricCloud)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->FindComponentByClass<UVolumetricCloudComponent>()) { VolumetricCloud = *It; break; }
		}
	}
}

void AAerosWeatherController::BeginPlay()
{
	Super::BeginPlay();
	FindSceneActors();

	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (Sub && bStartOnBeginPlay && Sub->HasAuthority())
	{
		FAerosSimulationSettings S = Settings;
		if (bCenterOnActor && S.DomainCenter.IsNearlyZero()) S.DomainCenter = FVector(GetActorLocation().X, GetActorLocation().Y, 0.0);
		Sub->StartSimulation(S);
	}
	if (SunLight)
	{
		SunLight->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	}
	if (HeightFog && HeightFog->GetComponent()) BaseFogDensity = FMath::Max(BaseFogDensity, HeightFog->GetComponent()->FogDensity);
	BindCloudMaterial();
}

void AAerosWeatherController::BindCloudMaterial()
{
	if (!CloudMaterial || !VolumetricCloud) return;
	UVolumetricCloudComponent* Cloud = VolumetricCloud->FindComponentByClass<UVolumetricCloudComponent>();
	if (!Cloud) return;
	CloudMID = UMaterialInstanceDynamic::Create(CloudMaterial, this);
	Cloud->SetMaterial(CloudMID);
	Cloud->SetLayerBottomAltitude(0.3f);
	Cloud->SetLayerHeight(15.f);
	// Storm towers must be visible from far away (default 50 km hid everything beyond),
	// with more samples so the longer rays keep their detail
	Cloud->SetTracingMaxDistance(180.f);
	Cloud->SetViewSampleCountScale(CloudSampleScale);
	// Self-shadowing: 15 km covers a whole tower (dark storm bases); 2x the 10 default samples keeps
	// the step near 750 m so the lumps of an anvil top shade each other (25 km / 10 samples left it flat white)
	Cloud->SetShadowTracingDistance(15.f);
	Cloud->SetShadowViewSampleCountScale(2.f);
	Cloud->SetSkyLightCloudBottomOcclusion(0.75f);
	// Default (quarter-res tracing + temporal) turns into grain when the time scale makes clouds
	// move fast: the history no longer matches. Half-res tracing keeps the edges sharp.
	if (bSharpCloudTracing)
	{
		if (IConsoleVariable* Mode = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricRenderTarget.Mode")))
		{
			Mode->Set(1, ECVF_SetByGameSetting);
		}
	}
}

void AAerosWeatherController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub || !Sub->IsSimulationRunning()) return;

	// Sun: astronomical position from latitude, day of year, hour
	if (bDriveSun && SunLight)
	{
		const FAerosDateTime Clock = Sub->GetDateTime();
		SunLight->GetLightComponent()->SetWorldRotation((-Clock.SunDirection).Rotation());
		// Ash plumes and dust storms dim the sun (clouds are handled by the cloud renderer)
		if (BaseSunIntensity < 0.f) BaseSunIntensity = SunLight->GetLightComponent()->Intensity;
		const FAerosWeatherSample Sky = Sub->GetWeatherAtLocation(Sub->GetViewerLocation());
		const float Dim = FMath::Exp(-Sky.Dust / 40.f - Sky.AshFall * 1.5f);
		SunLight->GetLightComponent()->SetIntensity(BaseSunIntensity * FMath::Max(Dim, 0.05f));
	}

	const FAerosWeatherSample Local = Sub->GetWeatherAtLocation(Sub->GetViewerLocation());

	// Fog: visibility (fog, precipitation) + humidity haze
	if (bDriveFog && HeightFog && HeightFog->GetComponent())
	{
		const float Thick = FMath::Clamp(1.f - Local.Visibility / 20000.f, 0.f, 1.f);
		const float Haze = FMath::Clamp((Local.RelativeHumidity - 0.6f) / 0.4f, 0.f, 1.f) * 0.5f;
		const float Target = BaseFogDensity * (1.f + Haze + 25.f * Thick * Thick);
		SmoothedFog = FMath::FInterpTo(SmoothedFog > 0.f ? SmoothedFog : Target, Target, DeltaSeconds, 0.5f);
		HeightFog->GetComponent()->SetFogDensity(SmoothedFog);

		// Tint: ochre sand, grey ash, white blowing snow
		const float Sand = FMath::Clamp(Local.Dust / 10.f, 0.f, 1.f);
		const float Ash = FMath::Clamp(Local.AshFall + Local.AshDepth / 50.f, 0.f, 1.f);
		FLinearColor Tint = FLinearColor(0.45f, 0.55f, 0.7f);
		Tint = FMath::Lerp(Tint, FLinearColor(0.75f, 0.5f, 0.25f), Sand);
		Tint = FMath::Lerp(Tint, FLinearColor(0.25f, 0.24f, 0.23f), Ash);
		Tint = FMath::Lerp(Tint, FLinearColor(0.9f, 0.92f, 0.95f), Local.BlowingSnow);
		SmoothedTint = FMath::Lerp(SmoothedTint, Tint, FMath::Clamp(DeltaSeconds, 0.f, 1.f));
		if (Sand + Ash + Local.BlowingSnow > 0.01f || bTintActive)
		{
			HeightFog->GetComponent()->SetFogInscatteringColor(SmoothedTint);
			bTintActive = Sand + Ash + Local.BlowingSnow > 0.01f;
		}
	}

	// Cloud material: weather map + domain mapping
	if (CloudMID && Sub->GetWeatherMapTexture())
	{
		CloudMID->SetTextureParameterValue(TEXT("AerosWeatherMap"), Sub->GetWeatherMapTexture());
		if (UAerosCloudManager* Clouds = Sub->GetCloudManager())
		{
			if (Clouds->GetCloudShapeMap()) CloudMID->SetTextureParameterValue(TEXT("AerosCloudShapeMap"), Clouds->GetCloudShapeMap());
			const FVector Off = Clouds->GetCloudNoiseOffsetKm();
			CloudMID->SetVectorParameterValue(TEXT("AerosNoiseOffset"), FLinearColor(float(Off.X), float(Off.Y), 0.f, 0.f));
			// Voxel cloud grid (two buffers, each with its own place)
			if (Clouds->GetCloudVolume(0) && Clouds->GetCloudVolume(1))
			{
				CloudMID->SetTextureParameterValue(TEXT("AerosCloudVolumePrev"), Clouds->GetCloudVolume(0));
				CloudMID->SetTextureParameterValue(TEXT("AerosCloudVolumeCur"), Clouds->GetCloudVolume(1));
			}
			for (int32 K = 0; K < 2; ++K)
			{
				const FVector4 R = Clouds->GetCloudVolumeRect(K);
				CloudMID->SetVectorParameterValue(K == 0 ? TEXT("AerosCloudVolumeRectPrev") : TEXT("AerosCloudVolumeRectCur"), FLinearColor(float(R.X), float(R.Y), float(R.Z), float(R.W)));
			}
			CloudMID->SetScalarParameterValue(TEXT("AerosCloudVolumeBlend"), Clouds->GetCloudVolumeBlend());
			CloudMID->SetScalarParameterValue(TEXT("AerosCloudVolumeWeight"), Clouds->GetCloudVolumeWeight());
		}
		const FVector4 D = Sub->GetDomainRect();
		CloudMID->SetVectorParameterValue(TEXT("AerosDomain"), FLinearColor(float(D.X), float(D.Y), float(D.Z), float(D.W)));
		CloudMID->SetScalarParameterValue(TEXT("AerosLightningFlash"), Sub->GetCloudManager() ? Sub->GetCloudManager()->GetLightningFlash() : 0.f);
		if (Sub->GetCloudManager())
		{
			const FVector L = Sub->GetCloudManager()->GetLightningLocation();
			CloudMID->SetVectorParameterValue(TEXT("AerosLightningPos"), FLinearColor(float(L.X), float(L.Y), float(L.Z), 1.f));
		}
	}

	if (SkyRecaptureInterval > 0.f && SkyLight && SkyLight->GetLightComponent())
	{
		SkyTimer += DeltaSeconds;
		if (SkyTimer >= SkyRecaptureInterval)
		{
			SkyTimer = 0.f;
			SkyLight->GetLightComponent()->RecaptureSky();
		}
	}
}
