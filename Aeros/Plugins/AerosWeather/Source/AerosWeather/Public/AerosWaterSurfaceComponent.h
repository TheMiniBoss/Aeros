// Copyright Aeros. All Rights Reserved.
//
// UAerosWaterSurfaceComponent - shows the simulated water around the viewer: rising flood water,
// storm surge and tsunami run-up on land, and the sea level anomaly over the ocean.
// Add it to the player pawn / camera. The plane follows the viewer at the simulated water height.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "AerosWaterSurfaceComponent.generated.h"

class UStaticMeshComponent;
class UStaticMesh;

UCLASS(ClassGroup = (Aeros), meta = (BlueprintSpawnableComponent))
class AEROSWEATHER_API UAerosWaterSurfaceComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UAerosWaterSurfaceComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Half size of the water plane (m). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros")
	float HalfExtentM = 4000.f;

	/** Draw the sea too (turn off if the level has its own ocean, e.g. the Water plugin). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros")
	bool bShowOcean = true;

	/** Minimum simulated flood depth (m) before land water is drawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros")
	float MinFloodDepth = 0.03f;

	UFUNCTION(BlueprintPure, Category = "Aeros")
	float GetWaterSurfaceZ() const { return SmoothedZ; }

	UFUNCTION(BlueprintPure, Category = "Aeros")
	bool IsWaterVisible() const { return bVisible; }

	UPROPERTY(EditAnywhere, Category = "Aeros")
	TObjectPtr<UStaticMesh> PlaneMesh;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Aeros")
	TObjectPtr<UStaticMeshComponent> Plane;

private:
	float SmoothedZ = -1e7f;
	bool bVisible = false;
};
