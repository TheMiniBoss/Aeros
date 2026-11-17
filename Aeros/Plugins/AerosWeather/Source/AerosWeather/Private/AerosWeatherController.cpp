// Copyright Aeros. All Rights Reserved.

#include "AerosWeatherController.h"
#include "AerosWeatherSubsystem.h"
#include "AerosCloudManager.h"
#include "AerosTornado.h"
#include "AerosCloudRender.h"
#include "AerosAssetUtils.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
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

void AAerosWeatherController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The renderer keeps the last parameters on the render thread: hand it empty ones before this
	// world's textures are released (queued ahead of their release), so no other view (editor
	// viewport after PIE, next map) samples freed resources.
	AerosCloudRender::SetParams(FAerosCloudRenderParams());
	if (bCustomCloudsActive) SetCustomCloudShadows(false);
	Super::EndPlay(EndPlayReason);
}

void AAerosWeatherController::UpdateCustomClouds(UAerosWeatherSubsystem* Sub, float DeltaSeconds)
{
	const bool bOn = AerosCloudRender::IsEnabled();
	if (bOn != bCustomCloudsActive)
	{
		bCustomCloudsActive = bOn;
		if (UVolumetricCloudComponent* Cloud = VolumetricCloud ? VolumetricCloud->FindComponentByClass<UVolumetricCloudComponent>() : nullptr)
		{
			Cloud->SetVisibility(!bOn);
		}
		SetCustomCloudShadows(bOn);
	}
	if (!bOn || !Sub)
	{
		return;
	}

	FAerosCloudRenderParams P;
	auto Res = [](UTexture* T) { return T ? T->GetResource() : nullptr; };
	P.WeatherMap = Res(Sub->GetWeatherMapTexture());
	const FVector4 D = Sub->GetDomainRect();
	P.DomainCm = FVector4f(float(D.X), float(D.Y), float(D.Z), float(D.W));
	if (UAerosCloudManager* Clouds = Sub->GetCloudManager())
	{
		P.ShapeMap = Res(Clouds->GetCloudShapeMap());
		P.LayerMapA = Res(Clouds->GetLayerMapA());
		P.LayerMapB = Res(Clouds->GetLayerMapB());
		if (FAerosSnapshotPtr WindSnap = Sub->GetSnapshot())
		{
			const FVector W = FAerosSnapshot::SimToWorldDir(WindSnap->Synoptic.Wind500.X, WindSnap->Synoptic.Wind500.Y).GetSafeNormal2D();
			if (!W.IsNearlyZero()) P.HighWindDir = FVector2f(float(W.X), float(W.Y));
		}
		P.VolumePrev = Res(Clouds->GetCloudVolume(0));
		P.VolumeCur = Res(Clouds->GetCloudVolume(1));
		const FVector4 RP = Clouds->GetCloudVolumeRect(0);
		const FVector4 RC = Clouds->GetCloudVolumeRect(1);
		P.RectPrevCm = FVector4f(float(RP.X), float(RP.Y), float(RP.Z), float(RP.W));
		P.RectCurCm = FVector4f(float(RC.X), float(RC.Y), float(RC.Z), float(RC.W));
		P.VolumeBlend = Clouds->GetCloudVolumeBlend();
		P.VolumeWeight = Clouds->GetCloudVolumeWeight();
		P.NoiseOffsetKm = FVector3f(Clouds->GetCloudNoiseOffsetKm());
		// Drift of the noise / cumulus field per real second, from its change this frame (the offset
		// wraps every 10000 km: ignore a jump)
		const FVector Off = Clouds->GetCloudNoiseOffsetKm();
		const FVector Step = Off - PrevNoiseOffsetKm;
		if (DeltaSeconds > 1e-4f && Step.Size2D() < 50.0) NoiseDriftKmS = FVector3f(Step / DeltaSeconds);
		PrevNoiseOffsetKm = Off;
		P.DriftKmS = NoiseDriftKmS;
		// Per real second (the renderer reprojects with the frame time): storm motion x time scale
		P.MotionKmS = FVector3f(Clouds->GetCloudMotionCmS() * (Sub->IsPaused() ? 0.0 : double(Sub->GetTimeScale())) / 100000.0);
		for (const Aeros::FFlashEntry& E : Clouds->GetFlashes())
		{
			FAerosCloudFlash F;
			F.PosKm = FVector3f(E.Location / 100000.0);
			F.Intensity = E.Intensity;
			P.Flashes.Add(F);
		}
	}
	GatherCloudTornadoes(Sub, DeltaSeconds, P);
	GatherCloudStorms(Sub, DeltaSeconds, P);
	// Shadow map: 100 km centred on the viewer, snapped to 1 km (the light function reads the same rect).
	if (CloudShadowRT)
	{
		const FVector V = Sub->GetViewerLocation();
		const double Snap = 100000.0;
		P.ShadowRectCm = FVector4f(float(FMath::RoundToDouble(V.X / Snap) * Snap), float(FMath::RoundToDouble(V.Y / Snap) * Snap), 10000000.f, 10000000.f);
		P.ShadowTarget = CloudShadowRT->GameThread_GetRenderTargetResource();
		if (CloudShadowMID)
		{
			CloudShadowMID->SetVectorParameterValue(TEXT("AerosCloudShadowRect"), FLinearColor(P.ShadowRectCm.X, P.ShadowRectCm.Y, P.ShadowRectCm.Z, P.ShadowRectCm.W));
		}
	}
	// Sky light under a cloud deck (replaces the VolumetricCloud's SkyLightCloudBottomOcclusion 0.75)
	if (SkyLight && SkyLight->GetLightComponent())
	{
		USkyLightComponent* SkyComp = SkyLight->GetLightComponent();
		if (BaseSkyLightIntensity < 0.f) BaseSkyLightIntensity = SkyComp->Intensity;
		const float CoverAbove = FMath::Clamp(Sub->GetWeatherAtLocation(Sub->GetViewerLocation()).CloudCover, 0.f, 1.f);
		SkyComp->SetIntensity(BaseSkyLightIntensity * FMath::Lerp(1.f, 0.75f, CoverAbove));
	}
	AerosCloudRender::SetParams(P);
}

void AAerosWeatherController::GatherCloudTornadoes(UAerosWeatherSubsystem* Sub, float DeltaSeconds, FAerosCloudRenderParams& P)
{
	UWorld* World = GetWorld();
	if (!World || !Sub) return;
	const FVector Viewer = Sub->GetViewerLocation();
	static IConsoleVariable* KVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AerosClouds.TornadoK"));
	const float K = KVar ? KVar->GetFloat() : 2.f;
	// Gallery overrides (metres, m/s): cloud base, peak wind and core radius of every tornado
	float ForcedBaseM = -1.f, ForcedVmax = -1.f, ForcedCoreM = -1.f;
	int32 ForcedSubs = -1;
	FParse::Value(FCommandLine::Get(), TEXT("AerosTornadoSubs="), ForcedSubs);
	FParse::Value(FCommandLine::Get(), TEXT("AerosTornadoLCL="), ForcedBaseM);
	FParse::Value(FCommandLine::Get(), TEXT("AerosTornadoCore="), ForcedCoreM);
	FParse::Value(FCommandLine::Get(), TEXT("AerosTornadoVmax="), ForcedVmax);

	// The 4 nearest within 60 km
	TArray<TPair<double, AAerosTornado*>> Near;
	for (TActorIterator<AAerosTornado> It(World); It; ++It)
	{
		const double D = FVector::Dist2D(It->GetActorLocation(), Viewer);
		if (D < 6000000.0) Near.Emplace(D, *It);
	}
	Near.Sort([](const TPair<double, AAerosTornado*>& A, const TPair<double, AAerosTornado*>& B) { return A.Key < B.Key; });
	TSet<AAerosTornado*> Selected;
	for (int32 I = 0; I < Near.Num() && I < 4; ++I) Selected.Add(Near[I].Value);

	// Fades: toward 1 when selected, toward 0 when dropped (1/s): the set changes without a pop
	for (AAerosTornado* T : Selected)
	{
		if (!TornadoFades.Contains(T))
		{
			TornadoFades.Add(T, 0.f);
			UE_LOG(LogTemp, Verbose, TEXT("Aeros clouds: tornado %s joins the rendered set"), *T->GetName());
		}
	}
	for (auto It = TornadoFades.CreateIterator(); It; ++It)
	{
		AAerosTornado* T = It.Key().Get();
		const bool bIn = T && Selected.Contains(T);
		const float OldFade = It.Value();
		It.Value() = FMath::Clamp(OldFade + (bIn ? DeltaSeconds : -DeltaSeconds), 0.f, 1.f);
		if (T && bIn && OldFade < 1.f && It.Value() >= 1.f) UE_LOG(LogTemp, Verbose, TEXT("Aeros clouds: tornado %s fully faded in"), *T->GetName());
		if (T && !bIn && OldFade >= 1.f) UE_LOG(LogTemp, Verbose, TEXT("Aeros clouds: tornado %s starts fading out"), *T->GetName());
		if (!T || (!bIn && It.Value() <= 0.f))
		{
			UE_LOG(LogTemp, Verbose, TEXT("Aeros clouds: tornado %s left the rendered set"), T ? *T->GetName() : TEXT("(destroyed)"));
			It.RemoveCurrent();
			continue;
		}
		if (P.Tornadoes.Num() >= 4) continue;   // a 5th fading one waits: at most 4 drawn

		// Cloud base: the storm's LCL, as the voxel grid draws it (FAerosCloudVolume: clamped to 0.3-3 km) -
		// the parent storm, else the nearest one within 30 km, else the local LCL
		float BaseM = Sub->GetWeatherAtLocation(T->GetActorLocation()).LCL;
		if (FAerosSnapshotPtr Snap = Sub->GetSnapshot())
		{
			double BestD = 3000000.0;
			for (const FAerosStormData& S : Snap->Storms)
			{
				const double DS = FVector::Dist2D(S.UpdraftLocation, T->GetActorLocation());
				if (S.Id == T->GetParentStormId()) { BaseM = S.LCL; break; }
				if (DS < BestD) { BestD = DS; BaseM = S.LCL; }
			}
		}
		if (ForcedBaseM > 0.f) BaseM = ForcedBaseM;
		FAerosCloudTornado D = T->MakeRenderDescriptor(FMath::Clamp(BaseM, 300.f, 3000.f), K);
		if (ForcedVmax > 0.f && T->MaxWind > 0.f) D.VmaxMs *= ForcedVmax / T->MaxWind;
		if (ForcedCoreM > 0.f && T->CoreRadius > 0.f) D.CoreRadiusM *= ForcedCoreM / T->CoreRadius;
		if (ForcedSubs >= 0) D.NumSubVortices = FMath::Min(ForcedSubs, 6);
		if (!TornadoBaseLogged.Contains(T))
		{
			TornadoBaseLogged.Add(T);
			UE_LOG(LogTemp, Verbose, TEXT("Aeros clouds: tornado %s base %.0f m, Vmax %.0f m/s, core %.0f m, sub-vortices %d"),
				*T->GetName(), D.CloudBaseKm * 1000.f, D.VmaxMs, D.CoreRadiusM, D.NumSubVortices);
		}
		D.Fade = It.Value();
		P.Tornadoes.Add(D);
	}
}

void AAerosWeatherController::GatherCloudStorms(UAerosWeatherSubsystem* Sub, float DeltaSeconds, FAerosCloudRenderParams& P)
{
	FAerosSnapshotPtr Snap = Sub ? Sub->GetSnapshot() : nullptr;
	if (!Snap) return;
	const FVector Viewer = Sub->GetViewerLocation();
	const float Hemi = Sub->GetSimulationSettings().LatitudeDeg < 0.f ? -1.f : 1.f;
	// Tower lean as the voxel grid draws it (FAerosCloudVolume: 0.25 km per km along the 500 hPa wind)
	FVector Dir500 = FAerosSnapshot::SimToWorldDir(Snap->Synoptic.Wind500.X, Snap->Synoptic.Wind500.Y).GetSafeNormal2D();
	if (Dir500.Size2D() < 0.5) Dir500 = FVector(1.0, 0.0, 0.0);
	auto Smooth = [](float E0, float E1, float X) { const float T = FMath::Clamp((X - E0) / (E1 - E0), 0.f, 1.f); return T * T * (3.f - 2.f * T); };

	// The 4 nearest rotating storms within 60 km, as displayed (interpolated between states)
	const TArray<FAerosStormData> Storms = Sub->GetStorms();
	TArray<TPair<double, int32>> Near;
	for (int32 K = 0; K < Storms.Num(); ++K)
	{
		const FAerosStormData& S = Storms[K];
		if (!S.IsActive() || S.Mesocyclone < 0.3f) continue;
		const double D = FVector::Dist2D(S.UpdraftLocation, Viewer);
		if (D < 6000000.0) Near.Emplace(D, K);
	}
	Near.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });
	TSet<int32> Selected;
	for (int32 I = 0; I < Near.Num() && I < 4; ++I) Selected.Add(Storms[Near[I].Value].Id);

	// Visible rotation: a mesocyclone turns ~0.01 rad/s; sped up with the clock, capped (it would strobe)
	const float Spin = Hemi * FMath::Min(0.01f * FMath::Max(Sub->GetTimeScale(), 1.f), 0.25f) * (Sub->IsPaused() ? 0.f : 1.f);
	for (int32 Id : Selected) if (!StormFadeSpin.Contains(Id)) StormFadeSpin.Add(Id, FVector2f(0.f, 0.f));
	for (auto It = StormFadeSpin.CreateIterator(); It; ++It)
	{
		const bool bIn = Selected.Contains(It.Key());
		FVector2f& FS = It.Value();
		FS.X = FMath::Clamp(FS.X + (bIn ? 0.5f : -0.5f) * DeltaSeconds, 0.f, 1.f);
		const FAerosStormData* S = Storms.FindByPredicate([&It](const FAerosStormData& X) { return X.Id == It.Key(); });
		if (!S || (!bIn && FS.X <= 0.f)) { It.RemoveCurrent(); continue; }
		const float Rotation = Smooth(0.3f, 0.8f, S->Mesocyclone);
		FS.Y = FMath::Fmod(FS.Y + Spin * Rotation * DeltaSeconds, 2.f * PI);
		if (P.Storms.Num() >= 4) continue;

		FAerosCloudStorm D;
		const FAerosWeatherSample Here = Sub->GetWeatherAtLocation(S->UpdraftLocation);
		const float Ground = FMath::Max(Here.Elevation, 0.f) * 0.001f;   // as the voxel grid (cell elevation)
		D.UpdraftKm = FVector3f(float(S->UpdraftLocation.X * 1e-5), float(S->UpdraftLocation.Y * 1e-5), Ground);
		D.BaseKm = FMath::Clamp(S->LCL * 0.001f, 0.3f, 3.f);
		D.TopKm = FMath::Max(S->CloudTop * 0.001f, D.BaseKm + 1.f);
		D.TowerRadiusKm = FMath::Clamp(S->RadiusKm * 0.35f, 2.f, 6.f) * (S->SupercellType == EAerosSupercellType::LowTopped ? 0.75f : 1.f);
		D.LeanKm = FVector2f(float(Dir500.X), float(Dir500.Y)) * 0.25f;
		const FVector Wall = AAerosTornado::AnchorFor(*S, Dir500, Hemi);
		D.WallKm = FVector2f(float(Wall.X * 1e-5), float(Wall.Y * 1e-5));
		D.WallRadiusKm = FMath::Clamp(D.TowerRadiusKm * 0.6f, 1.5f, 3.f);
		D.Rotation = Rotation;
		D.SpinPhase = FS.Y;
		// An LP shows its corkscrew updraft bare; an HP hides it in rain
		D.Striation = Rotation * FMath::Lerp(1.f, 0.25f, Smooth(0.35f, 0.85f, S->PrecipLoad));
		D.Fade = FS.X;
		P.Storms.Add(D);
	}
}

void AAerosWeatherController::SetCustomCloudShadows(bool bOn)
{
	UDirectionalLightComponent* Sun = SunLight ? Cast<UDirectionalLightComponent>(SunLight->GetLightComponent()) : nullptr;
	if (!bOn)
	{
		if (Sun && CloudShadowMID && Sun->LightFunctionMaterial == CloudShadowMID) Sun->SetLightFunctionMaterial(nullptr);
		if (SkyLight && SkyLight->GetLightComponent() && BaseSkyLightIntensity >= 0.f) SkyLight->GetLightComponent()->SetIntensity(BaseSkyLightIntensity);
		return;
	}
	if (!CloudShadowRT)
	{
		CloudShadowRT = NewObject<UTextureRenderTarget2D>(this, TEXT("AerosCloudShadowMap"));
		CloudShadowRT->RenderTargetFormat = RTF_R16f;
		CloudShadowRT->ClearColor = FLinearColor::White;
		CloudShadowRT->bCanCreateUAV = true;
		CloudShadowRT->AddressX = TA_Clamp;
		CloudShadowRT->AddressY = TA_Clamp;
		CloudShadowRT->InitAutoFormat(512, 512);
		CloudShadowRT->UpdateResourceImmediate(true);
	}
	if (!CloudShadowMID)
	{
		UMaterialInterface* Mat = Aeros::LoadOptional(TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/AerosWeather/Materials/M_AerosCloudShadow.M_AerosCloudShadow"))));
		if (Mat)
		{
			CloudShadowMID = UMaterialInstanceDynamic::Create(Mat, this);
			CloudShadowMID->SetTextureParameterValue(TEXT("AerosCloudShadowMap"), CloudShadowRT);
		}
	}
	if (Sun && CloudShadowMID)
	{
		Sun->SetLightFunctionMaterial(CloudShadowMID);
		// The light function covers the whole view: no fade to the "disabled" brightness with distance.
		Sun->SetLightFunctionFadeDistance(WORLD_MAX);
		Sun->SetLightFunctionDisabledBrightness(1.f);
	}
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
	if (CloudUpsamplingMode >= 0)
	{
		if (IConsoleVariable* Up = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricRenderTarget.UpsamplingMode")))
		{
			Up->Set(CloudUpsamplingMode, ECVF_SetByGameSetting);
		}
	}
	int32 ForcedMode = -1; // debug: -AerosCloudMode=N tries another r.VolumetricRenderTarget.Mode
	FParse::Value(FCommandLine::Get(), TEXT("AerosCloudMode="), ForcedMode);
	if (bSharpCloudTracing || ForcedMode >= 0)
	{
		if (IConsoleVariable* Mode = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricRenderTarget.Mode")))
		{
			Mode->Set(ForcedMode >= 0 ? ForcedMode : 1, ECVF_SetByGameSetting);
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

	UpdateCustomClouds(Sub, DeltaSeconds);

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
