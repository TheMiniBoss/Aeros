// Copyright Aeros. All Rights Reserved.
//
// Free-flying storm-chaser camera for the demo. Keys are read directly from the player
// controller (no input assets needed), see AAerosDemoHUD for the on-screen help.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "AerosDemoPawn.generated.h"

class USphereComponent;
class UCameraComponent;
class UFloatingPawnMovement;
class UAerosPrecipitationComponent;
class UAerosWaterSurfaceComponent;
class APlayerController;

UCLASS()
class AEROSWEATHERDEMO_API AAerosDemoPawn : public APawn
{
	GENERATED_BODY()

public:
	AAerosDemoPawn();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Demo")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Demo")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Demo")
	TObjectPtr<UFloatingPawnMovement> Movement;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Demo")
	TObjectPtr<UAerosPrecipitationComponent> Precipitation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Demo")
	TObjectPtr<UAerosWaterSurfaceComponent> WaterSurface;

	/** cm/s; Shift x10, Ctrl x100 (fly to a storm 50 km away in seconds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	float BaseSpeed = 2500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	float MouseSensitivity = 0.15f;

	/** Last action, shown by the HUD. */
	FString LastMessage;
	float MessageTime = 0.f;

	/** Guided tour caption, shown by the HUD (empty = no tour running). */
	FString TourTitle;
	FString TourCaption;

	/** Guided tour: shows each phenomenon, explains it, and saves a screenshot per step
	 *  (Saved/Screenshots/AerosTour). Key F2, or "-AerosTour" on the command line
	 *  ("-AerosTourQuit" also exits at the end, for automated captures). */
	void StartTour();
	void StopTour();
	bool IsTourRunning() const { return TourStep >= 0; }

private:
	struct FTourStep
	{
		FString Title;
		FString Caption;
		FString Shot;
		float Duration = 8.f;
		TFunction<void()> Enter;
	};

	void HandleMovement(APlayerController* PC, float DeltaSeconds);
	void HandleHotkeys(APlayerController* PC);
	void Notify(const FString& Message);
	void FlyToNearestStorm();
	void TeleportLookAt(const FVector& Location, const FVector& Target);
	void TickTour(float DeltaSeconds);
	void BuildTour();

	int32 DebugLevel = 0;
	int32 VolcanoVEI = 2;
	int32 CityIndex = 0;

	TArray<FTourStep> Tour;
	int32 TourStep = -1;
	float TourTime = 0.f;
	bool bTourShotTaken = false;
	bool bQuitAfterTour = false;
	float AutoTourDelay = -1.f;
};
