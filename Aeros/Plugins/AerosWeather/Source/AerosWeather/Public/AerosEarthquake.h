// Copyright Aeros. All Rights Reserved.
//
// AAerosEarthquake - one seismic event. Replicated (magnitude, depth, server start time) so every
// machine computes the same shaking: P waves (6 km/s) then S waves (3.5 km/s), peak ground
// acceleration from an attenuation law, duration growing with magnitude.
// Server effects near players: physics bodies shaken, Chaos strain on Geometry Collections,
// tsunami if submarine (M >= 7, shallow), aftershocks (Omori).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Camera/CameraModifier.h"
#include "AerosTypes.h"
#include "AerosEarthquake.generated.h"

class UFieldSystemComponent;
class UAudioComponent;

UCLASS(Blueprintable)
class AEROSWEATHER_API AAerosEarthquake : public AActor
{
	GENERATED_BODY()

public:
	AAerosEarthquake();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	void ServerInitialize(float InMagnitude, float InDepthKm, bool bInAftershock);

	/** Peak ground acceleration (g) at a location at the current time. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Earthquake")
	float GetShakingAt(const FVector& Location) const;

	/** Peak value (ignoring wave arrival): for maps / warnings. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Earthquake")
	float GetPeakAccelerationAt(const FVector& Location) const;

	/** Modified Mercalli intensity (I-XII) from PGA. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Earthquake")
	static int32 MercalliFromPGA(float PGA);

	UFUNCTION(BlueprintPure, Category = "Aeros|Earthquake")
	FAerosEarthquakeData GetData() const;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Earthquake") float Magnitude = 6.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Earthquake") float DepthKm = 10.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Earthquake") double StartServerTime = 0.0;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Earthquake") float Duration = 20.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Earthquake") bool bTsunamigenic = false;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Aeros|Earthquake") bool bAftershock = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Earthquake")
	TObjectPtr<UFieldSystemComponent> FieldSystem;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros|Earthquake")
	TObjectPtr<UAudioComponent> RumbleAudio;

private:
	double Now() const;
	float Envelope(float DistanceKm) const;
	void ShakePhysics(float DeltaSeconds);
	void ScheduleAftershocks();

	float PhysicsTimer = 0.f;
	float StrainTimer = 0.f;
	TArray<float> AftershockTimes;
	TArray<float> AftershockMagnitudes;
	bool bAnnounced = false;
};

/** Camera shake driven by ground acceleration (earthquakes) and extreme wind. Added automatically. */
UCLASS()
class AEROSWEATHER_API UAerosShakeCameraModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;

private:
	float Time = 0.f;
	float Smoothed = 0.f;
};
