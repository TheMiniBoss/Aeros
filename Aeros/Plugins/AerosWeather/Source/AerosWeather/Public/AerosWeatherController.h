// Copyright Aeros. All Rights Reserved.
//
// AAerosWeatherController - drop one in a level. It starts the simulation with its settings
// and plugs the weather into the scene: sun position (seasons + time of day), fog, sky
// light recapture and the Volumetric Cloud material (weather map).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AerosTypes.h"
#include "AerosWeatherController.generated.h"

class ADirectionalLight;
class AExponentialHeightFog;
class ASkyLight;
class UMaterialInterface;
class UMaterialInstanceDynamic;

UCLASS(Blueprintable, meta = (DisplayName = "Aeros Weather Controller"))
class AEROSWEATHER_API AAerosWeatherController : public AActor
{
	GENERATED_BODY()

public:
	AAerosWeatherController();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Simulation")
	FAerosSimulationSettings Settings;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Simulation")
	bool bStartOnBeginPlay = true;

	/** Centre the domain on this actor when Settings.DomainCenter is zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Simulation")
	bool bCenterOnActor = true;

	// ---- Scene bindings (auto-found when empty) ---------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	TObjectPtr<ADirectionalLight> SunLight;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	TObjectPtr<AExponentialHeightFog> HeightFog;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	TObjectPtr<ASkyLight> SkyLight;

	/** Actor holding a Volumetric Cloud component. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	TObjectPtr<AActor> VolumetricCloud;

	/** Volumetric cloud material reading AerosWeatherMap / AerosDomain (see Content/Python/aeros_setup.py). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	TObjectPtr<UMaterialInterface> CloudMaterial;

	/** Trace the clouds at half resolution (r.VolumetricRenderTarget.Mode 1) instead of a quarter.
	 *  Off by default: mode 1 flickered badly even at x1 (frame-to-frame difference 4.5 vs 1.2 for the
	 *  default mode 0, measured with -AerosFlickerShots). Off = keep the project setting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	bool bSharpCloudTracing = false;

	/** r.VolumetricRenderTarget.UpsamplingMode set at start (-1 = keep the project setting). 0 = bilinear flickers
	 *  least (0.7 vs 1.2) but shows the quarter-res pixels on cloud edges; the default bilateral (4) looks cleaner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	int32 CloudUpsamplingMode = -1;

	/** Cloud view-ray sample count scale. 1.25: ~12 ms GPU from the ground, ~18-20 ms above a cumulus
	 *  field (1336x751, measured with half-res tracing). 2: smoother towers, ~26 ms from above. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene", meta = (ClampMin = "0.25", ClampMax = "4"))
	float CloudSampleScale = 1.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	bool bDriveSun = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	bool bDriveFog = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	float BaseFogDensity = 0.01f;

	/** Recapture a static sky light every N seconds (0 = off; real-time capture does not need it). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros|Scene")
	float SkyRecaptureInterval = 0.f;

	UFUNCTION(BlueprintCallable, Category = "Aeros")
	void FindSceneActors();

private:
	void BindCloudMaterial();
	/** Feeds the custom cloud renderer and hides the engine VolumetricCloud while it is on. */
	void UpdateCustomClouds(class UAerosWeatherSubsystem* Sub, float DeltaSeconds);
	FVector PrevNoiseOffsetKm = FVector::ZeroVector;
	/** Tornadoes drawn by the custom renderer (the 4 nearest), each with its fade in / out of the set. */
	void GatherCloudTornadoes(class UAerosWeatherSubsystem* Sub, float DeltaSeconds, struct FAerosCloudRenderParams& P);
	TMap<TWeakObjectPtr<class AAerosTornado>, float> TornadoFades;
	FVector3f NoiseDriftKmS = FVector3f::ZeroVector;
	bool bCustomCloudsActive = false;
	/** Cloud shadows of the custom renderer: R16F transmittance map read by the sun's light function. */
	void SetCustomCloudShadows(bool bOn);

	UPROPERTY(Transient)
	TObjectPtr<class UTextureRenderTarget2D> CloudShadowRT;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CloudShadowMID;

	float BaseSkyLightIntensity = -1.f;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CloudMID;

	float SkyTimer = 0.f;
	float SmoothedFog = 0.f;
	float BaseSunIntensity = -1.f;
	FLinearColor SmoothedTint = FLinearColor(0.45f, 0.55f, 0.7f);
	bool bTintActive = false;
};
