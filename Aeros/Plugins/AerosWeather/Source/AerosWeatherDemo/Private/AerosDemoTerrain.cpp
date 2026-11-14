// Copyright Aeros. All Rights Reserved.

#include "AerosDemoTerrain.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWaterSurfaceComponent.h"
#include "AerosAssetUtils.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"

namespace AerosDemoTerrainInternal
{
	constexpr int32 Quads = 128;          // quads per side of a level
	constexpr int32 Verts = Quads + 1;    // vertices per side
	constexpr int32 Apron = Verts + 2;    // with one extra ring for the normals
	constexpr int32 HoleQuads = Quads / 2;
	constexpr int32 CollisionOverlap = 2; // collision hole shrunk by this many cells
	constexpr double AnchorCm = 20000000.0; // components sit on a 200 km grid (float precision of the vertices)

	inline double SnapTo(double V, double Step) { return FMath::RoundToDouble(V / Step) * Step; }
}

AAerosDemoTerrain::AAerosDemoTerrain()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Movable);
	Model = MakeShared<AerosDemoTerrainModel::FModel>();
}

void AAerosDemoTerrain::Configure(const FAerosSimulationSettings& Settings)
{
	using namespace AerosDemoTerrainModel;
	Model->Seed = uint32(Settings.Seed);
	Model->SeaLevelM = Settings.SeaLevelZ * 0.01;
	SeaLevelZ = Settings.SeaLevelZ;
	Model->Oceans.clear();
	Model->Deserts.clear();
	for (const FAerosWaterRegion& R : Settings.WaterRegions)
	{
		FCircle C;
		C.X = R.Center.X * 0.01; C.Y = R.Center.Y * 0.01;
		C.RadiusM = R.RadiusKm * 1000.0;
		C.EdgeM = FMath::Max(R.ShoreWidthKm, 0.1f) * 1000.0;
		Model->Oceans.push_back(C);
	}
	for (const FAerosDesertRegion& R : Settings.DesertRegions)
	{
		FCircle C;
		C.X = R.Center.X * 0.01; C.Y = R.Center.Y * 0.01;
		C.RadiusM = R.RadiusKm * 1000.0;
		C.EdgeM = FMath::Max(R.EdgeWidthKm, 0.1f) * 1000.0;
		Model->Deserts.push_back(C);
	}
	Model->Init();
}

void AAerosDemoTerrain::AddPad(const FVector& Center, float RadiusM, float EdgeM)
{
	AerosDemoTerrainModel::FCircle C;
	C.X = Center.X * 0.01; C.Y = Center.Y * 0.01;
	C.RadiusM = RadiusM;
	C.EdgeM = FMath::Max(EdgeM, 1.f);
	Model->Pads.push_back(C);
	Model->Init();
}

double AAerosDemoTerrain::GetGroundZ(double X, double Y) const
{
	return double(Model->Sample(X * 0.01, Y * 0.01).HeightM) * 100.0;
}

void AAerosDemoTerrain::BeginPlay()
{
	Super::BeginPlay();
	EnsureComponents();
	if (BuiltCenters.Num() == 0)
	{
		FVector View;
		BuildNow(GetViewLocation(View) ? View : GetActorLocation());
	}
}

void AAerosDemoTerrain::EnsureComponents()
{
	if (LevelMeshes.Num() == NumLevels) return;
	UMaterialInterface* GroundMat = Aeros::LoadOptional(TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/AerosWeather/Materials/M_AerosGround.M_AerosGround"))));
	for (int32 K = 0; K < NumLevels; ++K)
	{
		UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(this, *FString::Printf(TEXT("TerrainLevel%d"), K));
		C->SetMobility(EComponentMobility::Movable);
		C->SetupAttachment(RootComponent);
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetUsingAbsoluteScale(true);
		C->bUseAsyncCooking = false; // first build: collision ready at once
		C->SetCollisionProfileName(TEXT("BlockAll")); // object type WorldStatic
		C->SetCastShadow(K < FirstLevelWithoutShadow);
		C->RegisterComponent();
		LevelMeshes.Add(C);
		if (GroundMat) C->SetMaterial(0, GroundMat);
	}

	Sea = NewObject<UStaticMeshComponent>(this, TEXT("Sea"));
	Sea->SetMobility(EComponentMobility::Movable);
	Sea->SetupAttachment(RootComponent);
	Sea->SetUsingAbsoluteLocation(true);
	Sea->SetUsingAbsoluteRotation(true);
	Sea->SetUsingAbsoluteScale(true);
	Sea->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
	Sea->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sea->SetCastShadow(false);
	Sea->RegisterComponent();
	Sea->SetWorldScale3D(FVector(1000000.0, 1000000.0, 1.0)); // 1000 km (engine plane = 1 m)
	Sea->SetWorldLocation(FVector(0, 0, SeaLevelZ));
	if (UMaterialInterface* SeaMat = Aeros::LoadOptional(TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/AerosWeather/Materials/M_AerosSea.M_AerosSea")))))
	{
		Sea->SetMaterial(0, SeaMat);
	}
	else if (UMaterialInstanceDynamic* MID = Sea->CreateDynamicMaterialInstance(0))
	{
		MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.02f, 0.06f, 0.08f));
	}
}

TArray<FVector2D> AAerosDemoTerrain::DesiredCenters(const FVector& View) const
{
	using namespace AerosDemoTerrainInternal;
	TArray<FVector2D> Out;
	Out.SetNum(NumLevels);
	for (int32 K = 0; K < NumLevels; ++K)
	{
		// Snapped to 8 cells: the finer level's footprint falls on this level's grid lines, and a level
		// is rebuilt only every 8 cells of camera travel (200 m for the finest)
		const double Step = CellCm(K) * 8.0;
		Out[K] = FVector2D(SnapTo(View.X, Step), SnapTo(View.Y, Step));
	}
	return Out;
}

TSharedPtr<AAerosDemoTerrain::FBatch> AAerosDemoTerrain::BuildBatch(TSharedPtr<const AerosDemoTerrainModel::FModel> InModel,
	TArray<FVector2D> Centers, TArray<int32> Dirty, double FinestCellCm)
{
	using namespace AerosDemoTerrainInternal;
	using AerosDemoTerrainModel::Smooth;
	TSharedPtr<FBatch> Batch = MakeShared<FBatch>();
	Batch->Centers = Centers;
	Batch->Levels.SetNum(Dirty.Num());
	const double SeaM = InModel->SeaLevelM;
	const int32 NumLevelsTotal = Centers.Num();

	ParallelFor(Dirty.Num(), [&](int32 Index)
	{
		const int32 K = Dirty[Index];
		FLevelMesh& L = Batch->Levels[Index];
		L.Level = K;
		const double Cell = FinestCellCm * double(1 << K);
		const double CX = Centers[K].X, CY = Centers[K].Y;
		L.Anchor = FVector(SnapTo(CX, AnchorCm), SnapTo(CY, AnchorCm), 0.0);

		// Heights (cm) with a one-vertex apron for the normals
		TArray<double> H;
		H.SetNumUninitialized(Apron * Apron);
		TArray<AerosDemoTerrainModel::FSurface> Surf;
		Surf.SetNumUninitialized(Verts * Verts);
		for (int32 J = 0; J < Apron; ++J)
		{
			for (int32 I = 0; I < Apron; ++I)
			{
				const double X = CX + double(I - 1 - Quads / 2) * Cell;
				const double Y = CY + double(J - 1 - Quads / 2) * Cell;
				const AerosDemoTerrainModel::FSurface S = InModel->Sample(X * 0.01, Y * 0.01);
				H[I + J * Apron] = double(S.HeightM) * 100.0;
				if (I >= 1 && I <= Verts && J >= 1 && J <= Verts) Surf[(I - 1) + (J - 1) * Verts] = S;
			}
		}
		// Stitch the outer border to the coarser level: odd border vertices sit mid-edge of a coarse
		// cell, so they take the average of their neighbours (the coarse edge is a straight line)
		if (K < NumLevelsTotal - 1)
		{
			auto At = [&H](int32 I, int32 J) -> double& { return H[(I + 1) + (J + 1) * Apron]; };
			for (int32 T = 1; T < Quads; T += 2)
			{
				At(T, 0) = 0.5 * (At(T - 1, 0) + At(T + 1, 0));
				At(T, Quads) = 0.5 * (At(T - 1, Quads) + At(T + 1, Quads));
				At(0, T) = 0.5 * (At(0, T - 1) + At(0, T + 1));
				At(Quads, T) = 0.5 * (At(Quads, T - 1) + At(Quads, T + 1));
			}
		}

		L.Positions.SetNumUninitialized(Verts * Verts);
		L.Normals.SetNumUninitialized(Verts * Verts);
		L.Colors.SetNumUninitialized(Verts * Verts);
		for (int32 J = 0; J < Verts; ++J)
		{
			for (int32 I = 0; I < Verts; ++I)
			{
				const int32 A = (I + 1) + (J + 1) * Apron;
				const double Z = H[A];
				const double X = CX + double(I - Quads / 2) * Cell;
				const double Y = CY + double(J - Quads / 2) * Cell;
				const int32 V = I + J * Verts;
				L.Positions[V] = FVector(X - L.Anchor.X, Y - L.Anchor.Y, Z);
				const double DZX = (H[A + 1] - H[A - 1]) / (2.0 * Cell);
				const double DZY = (H[A + Apron] - H[A - Apron]) / (2.0 * Cell);
				L.Normals[V] = FVector(-DZX, -DZY, 1.0).GetSafeNormal();

				// Surface masks (vertex colour): R sand, G rock, B snow, A dry grass
				const AerosDemoTerrainModel::FSurface& S = Surf[V];
				const double Hm = Z * 0.01;
				const double Slope = FMath::Sqrt(DZX * DZX + DZY * DZY); // rise / run
				const double Rock = FMath::Max(Smooth(0.45, 0.8, Slope), Smooth(1900.0, 2800.0, Hm) * 0.55) * (1.0 - 0.7 * S.Desert);
				const double Snow = Smooth(2500.0, 2950.0, Hm + 150.0 * (S.Dryness - 0.5)) * (1.0 - Smooth(0.7, 1.1, Slope));
				// Beach: the first metre above the water (the start plains are 1.6-2.8 m above the sea: grass)
				const double Sand = FMath::Max(1.0 - Smooth(0.5, 1.5, Hm - SeaM), double(S.Desert)) * (1.0 - Rock);
				L.Colors[V] = FColor(uint8(Sand * 255.0 + 0.5), uint8(Rock * 255.0 + 0.5), uint8(Snow * 255.0 + 0.5), uint8(S.Dryness * 255.0f + 0.5f));
			}
		}

		// Holes: the finer level's footprint (visible) and the same shrunk by 2 cells (collision)
		int32 HX = -1, HY = -1;
		if (K > 0)
		{
			HX = int32(FMath::RoundToDouble((Centers[K - 1].X - CX) / Cell)) + (Quads - HoleQuads) / 2;
			HY = int32(FMath::RoundToDouble((Centers[K - 1].Y - CY) / Cell)) + (Quads - HoleQuads) / 2;
		}
		auto InHole = [HX, HY](int32 I, int32 J, int32 Shrink)
		{
			return HX >= 0 && I >= HX + Shrink && I < HX + HoleQuads - Shrink && J >= HY + Shrink && J < HY + HoleQuads - Shrink;
		};
		L.VisibleTriangles.Reserve(Quads * Quads * 6);
		L.CollisionTriangles.Reserve(Quads * Quads * 6);
		for (int32 J = 0; J < Quads; ++J)
		{
			for (int32 I = 0; I < Quads; ++I)
			{
				const int32 V0 = I + J * Verts;
				// Upward-facing winding; alternate the diagonal (no directional stripes)
				const bool bFlip = ((I + J) & 1) != 0;
				int32 Tri[6];
				if (bFlip)
				{
					Tri[0] = V0; Tri[1] = V0 + Verts; Tri[2] = V0 + Verts + 1;
					Tri[3] = V0; Tri[4] = V0 + Verts + 1; Tri[5] = V0 + 1;
				}
				else
				{
					Tri[0] = V0; Tri[1] = V0 + Verts; Tri[2] = V0 + 1;
					Tri[3] = V0 + 1; Tri[4] = V0 + Verts; Tri[5] = V0 + Verts + 1;
				}
				if (!InHole(I, J, 0)) L.VisibleTriangles.Append(Tri, 6);
				if (!InHole(I, J, CollisionOverlap)) L.CollisionTriangles.Append(Tri, 6);
			}
		}
	});
	return Batch;
}

void AAerosDemoTerrain::ApplyBatch(const FBatch& Batch)
{
	for (const FLevelMesh& L : Batch.Levels)
	{
		UProceduralMeshComponent* C = LevelMeshes.IsValidIndex(L.Level) ? LevelMeshes[L.Level].Get() : nullptr;
		if (!C) continue;
		C->SetWorldLocation(L.Anchor);
		C->CreateMeshSection(0, L.Positions, L.VisibleTriangles, L.Normals, TArray<FVector2D>(), L.Colors, TArray<FProcMeshTangent>(), false);
		C->CreateMeshSection(1, L.Positions, L.CollisionTriangles, TArray<FVector>(), TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>(), true);
		C->SetMeshSectionVisible(1, false);
	}
	BuiltCenters = Batch.Centers;
}

void AAerosDemoTerrain::BuildNow(const FVector& Around)
{
	EnsureComponents();
	if (bPending)
	{
		Pending.Wait();
		bPending = false;
	}
	const TArray<FVector2D> Centers = DesiredCenters(Around);
	TArray<int32> All;
	for (int32 K = 0; K < NumLevels; ++K) All.Add(K);
	const double Start = FPlatformTime::Seconds();
	TSharedPtr<FBatch> Batch = BuildBatch(Model, Centers, All, double(FinestCellM) * 100.0);
	for (UProceduralMeshComponent* C : LevelMeshes) C->bUseAsyncCooking = false;
	ApplyBatch(*Batch);
	for (UProceduralMeshComponent* C : LevelMeshes) C->bUseAsyncCooking = true; // later rebuilds: no hitch
	UE_LOG(LogTemp, Display, TEXT("Aeros terrain: %d levels built around (%.0f, %.0f) in %.0f ms, ground there Z=%.0f"),
		NumLevels, Around.X, Around.Y, (FPlatformTime::Seconds() - Start) * 1000.0, GetGroundZ(Around.X, Around.Y));
}

bool AAerosDemoTerrain::GetViewLocation(FVector& Out) const
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC) return false;
	FRotator Rot;
	PC->GetPlayerViewPoint(Out, Rot);
	return true;
}

void AAerosDemoTerrain::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	FVector View;
	if (!GetViewLocation(View)) return;

	if (bPending && Pending.IsReady())
	{
		TSharedPtr<FBatch> Batch = Pending.Get();
		bPending = false;
		if (Batch) ApplyBatch(*Batch);
	}
	if (!bPending)
	{
		const TArray<FVector2D> Centers = DesiredCenters(View);
		TArray<int32> Dirty;
		for (int32 K = 0; K < NumLevels; ++K)
		{
			const bool bMoved = !BuiltCenters.IsValidIndex(K) || !BuiltCenters[K].Equals(Centers[K], 1.0);
			const bool bInnerMoved = K > 0 && (!BuiltCenters.IsValidIndex(K - 1) || !BuiltCenters[K - 1].Equals(Centers[K - 1], 1.0));
			if (bMoved || bInnerMoved) Dirty.Add(K);
		}
		if (Dirty.Num() > 0)
		{
			TSharedPtr<const AerosDemoTerrainModel::FModel> ModelRef = Model;
			const double Finest = double(FinestCellM) * 100.0;
			Pending = Async(EAsyncExecution::ThreadPool, [ModelRef, Centers, Dirty, Finest]()
			{
				return BuildBatch(ModelRef, Centers, Dirty, Finest);
			});
			bPending = true;
		}
	}

	UpdateSea(DeltaSeconds, View);
}

void AAerosDemoTerrain::UpdateSea(float DeltaSeconds, const FVector& View)
{
	if (!Sea) return;
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);

	// The pawn's translucent ocean plane would fight with this one: keep it for floods only
	if (UWorld* World = GetWorld())
	{
		APlayerController* PC = World->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (Pawn && OceanPlaneFixedFor.Get() != Pawn)
		{
			TArray<UAerosWaterSurfaceComponent*> Waters;
			Pawn->GetComponents(Waters);
			for (UAerosWaterSurfaceComponent* W : Waters) W->bShowOcean = false;
			OceanPlaneFixedFor = Pawn;
		}
	}

	// Sea level anomaly a few km offshore (tsunami, storm surge), twice a second
	SeaQueryTimer -= DeltaSeconds;
	if (SeaQueryTimer <= 0.f && Sub && Sub->IsSimulationRunning())
	{
		SeaQueryTimer = 0.5f;
		FVector Coast, Offshore;
		SeaTargetAnomaly = Sub->FindNearestOcean(View, Coast, Offshore, 3.f) ? Sub->GetWeatherAtLocation(Offshore).SeaLevelAnomaly : 0.f;
	}
	SeaAnomaly = FMath::FInterpTo(SeaAnomaly, SeaTargetAnomaly, DeltaSeconds, 1.f);
	const double Snap = 1000000.0; // 10 km: the plane does not swim with the camera
	Sea->SetWorldLocation(FVector(FMath::GridSnap(View.X, Snap), FMath::GridSnap(View.Y, Snap), SeaLevelZ + SeaAnomaly * 100.0));
}
