// Copyright Aeros. All Rights Reserved.
//
// Procedural demo landscape: nested LOD rings (geometry clipmap) of procedural meshes centred on the
// camera, built from AerosDemoTerrainModel (deterministic height function), plus a sea plane.
//
//  * Level k is a 128 x 128 quad grid with cells of FinestCellM * 2^k (25 m ... 6.4 km: 3.2 km ... 819 km
//    wide). Each level has a hole where the finer level sits; the borders are stitched (no crack).
//  * Levels are rebuilt on a worker thread when the camera moves (finest every 200 m) and swapped in
//    together, so holes always match.
//  * Collision: a hidden section per level (hole shrunk by 2 cells so the levels overlap while the
//    async cooking swaps them). Object type WorldStatic: ground traces, the pawn, crates, tornadoes and
//    the simulation's terrain sampling (UAerosWeatherSubsystem line traces) all see the relief.
//  * Vertex colour = surface masks for M_AerosGround: R sand, G rock, B snow, A dry grass.
//  * Sea: one opaque plane (M_AerosSea) at the simulation's sea level; it fills the ocean and every
//    hollow below sea level (lakes). It follows the sea level anomaly offshore (tsunami, storm surge).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Async/Future.h"
#include "AerosTypes.h"
#include "AerosDemoTerrainModel.h"
#include "AerosDemoTerrain.generated.h"

class UProceduralMeshComponent;
class UStaticMeshComponent;

UCLASS(NotPlaceable)
class AEROSWEATHERDEMO_API AAerosDemoTerrain : public AActor
{
	GENERATED_BODY()

public:
	AAerosDemoTerrain();

	/** Seed, sea level, oceans and deserts from the simulation settings. Call before BuildNow. */
	void Configure(const FAerosSimulationSettings& Settings);

	/** Flatten a disc at its natural height (volcano base). Call before BuildNow. */
	void AddPad(const FVector& Center, float RadiusM, float EdgeM);

	/** Synchronous first build around a location: the collision is ready on return. */
	void BuildNow(const FVector& Around);

	/** Ground height (world Z, cm) from the height function: exact, no trace, works far from the camera. */
	double GetGroundZ(double X, double Y) const;

	/** Number of LOD levels (each twice as wide as the previous one). */
	UPROPERTY(EditAnywhere, Category = "Aeros Terrain", meta = (ClampMin = "2", ClampMax = "12"))
	int32 NumLevels = 9;

	/** Cell size of the finest level (m). */
	UPROPERTY(EditAnywhere, Category = "Aeros Terrain", meta = (ClampMin = "5"))
	float FinestCellM = 25.f;

	/** Levels from this one on do not cast shadows (far rings). */
	UPROPERTY(EditAnywhere, Category = "Aeros Terrain")
	int32 FirstLevelWithoutShadow = 7;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	struct FLevelMesh
	{
		int32 Level = 0;
		FVector Anchor = FVector::ZeroVector; // component location
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FColor> Colors;
		TArray<int32> VisibleTriangles;
		TArray<int32> CollisionTriangles;
	};
	struct FBatch
	{
		TArray<FLevelMesh> Levels;
		TArray<FVector2D> Centers; // requested centre of every level (cm)
	};

	void EnsureComponents();
	TArray<FVector2D> DesiredCenters(const FVector& View) const;
	double CellCm(int32 Level) const { return double(FinestCellM) * 100.0 * double(1 << Level); }
	static TSharedPtr<FBatch> BuildBatch(TSharedPtr<const AerosDemoTerrainModel::FModel> InModel, TArray<FVector2D> Centers,
		TArray<int32> Dirty, double FinestCellCm);
	void ApplyBatch(const FBatch& Batch);
	bool GetViewLocation(FVector& Out) const;
	void UpdateSea(float DeltaSeconds, const FVector& View);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> LevelMeshes;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Sea;

	TSharedPtr<AerosDemoTerrainModel::FModel> Model;
	TArray<FVector2D> BuiltCenters;
	TFuture<TSharedPtr<FBatch>> Pending;
	bool bPending = false;
	double SeaLevelZ = 0.0;
	float SeaAnomaly = 0.f;
	float SeaTargetAnomaly = 0.f;
	float SeaQueryTimer = 0.f;
	TWeakObjectPtr<APawn> OceanPlaneFixedFor;
};
