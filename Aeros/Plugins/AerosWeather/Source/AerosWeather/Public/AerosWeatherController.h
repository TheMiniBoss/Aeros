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

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CloudMID;

	float SkyTimer = 0.f;
	float SmoothedFog = 0.f;
	float BaseSunIntensity = -1.f;
	FLinearColor SmoothedTint = FLinearColor(0.45f, 0.55f, 0.7f);
	bool bTintActive = false;
};
