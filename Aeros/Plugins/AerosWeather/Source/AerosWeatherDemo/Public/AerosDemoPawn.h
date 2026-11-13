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

	/** cm/s; Shift x10, Ctrl x100 (fly to a storm 50 km away in seconds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	float BaseSpeed = 2500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	float MouseSensitivity = 0.15f;

	/** Last action, shown by the HUD. */
	FString LastMessage;
	float MessageTime = 0.f;

private:
	void HandleMovement(APlayerController* PC, float DeltaSeconds);
	void HandleHotkeys(APlayerController* PC);
	void Notify(const FString& Message);
	void FlyToNearestStorm();

	int32 DebugLevel = 0;
};
