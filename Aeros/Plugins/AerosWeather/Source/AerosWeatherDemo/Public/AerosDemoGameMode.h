// Copyright Aeros. All Rights Reserved.
//
// Demo game mode. Works in ANY level (even an empty one): if the level has no sky / sun /
// ground / weather controller, it builds them at runtime. Set it as GameMode Override in
// World Settings, or open /AerosWeather/Maps/L_AerosDemo (created by Content/Python/aeros_setup.py).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "AerosTypes.h"
#include "AerosDemoGameMode.generated.h"

UCLASS()
class AEROSWEATHERDEMO_API AAerosDemoGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AAerosDemoGameMode();

	virtual void StartPlay() override;

	/** Simulation used when the level has no AAerosWeatherController. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	FAerosSimulationSettings DemoSettings;

	/** Build sky, sun, fog, clouds and ground when missing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	bool bBuildSceneIfMissing = true;

	/** Scatter physics props (crates, stacks) to show tornado / hail interactions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	int32 NumPhysicsProps = 80;

	/** Add an erupting-capable volcano (with its own cone) 25 km north-west. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	bool bSpawnVolcano = true;

	/** Procedural landscape (hills, mountains, lakes, beaches, sea) instead of a flat plane. Replaces the
	 *  engine's flat Plane mesh if that is the only ground under the origin (old demo map). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Aeros Demo")
	bool bProceduralTerrain = true;

	/** The landscape built by this game mode (null with a level that has its own ground). */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Aeros Demo")
	TObjectPtr<class AAerosDemoTerrain> Terrain;

private:
	void BuildScene();
	void SpawnProps();
	double GroundZ(double X, double Y) const;
};
