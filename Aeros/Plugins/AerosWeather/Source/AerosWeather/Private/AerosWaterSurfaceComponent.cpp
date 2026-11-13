// Copyright Aeros. All Rights Reserved.

#include "AerosWaterSurfaceComponent.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosAssetUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

UAerosWaterSurfaceComponent::UAerosWaterSurfaceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneFinder(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneFinder.Succeeded()) PlaneMesh = PlaneFinder.Object;
}

void UAerosWaterSurfaceComponent::BeginPlay()
{
	Super::BeginPlay();
	if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
	{
		SetComponentTickEnabled(false);
		return;
	}
	Plane = NewObject<UStaticMeshComponent>(GetOwner(), TEXT("AerosWaterPlane"));
	Plane->SetStaticMesh(PlaneMesh);
	Plane->SetUsingAbsoluteLocation(true);
	Plane->SetUsingAbsoluteRotation(true);
	Plane->SetUsingAbsoluteScale(true);
	Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Plane->SetCastShadow(false);
	Plane->SetupAttachment(this);
	Plane->RegisterComponent();
	// Engine plane is 100 x 100 cm
	Plane->SetWorldScale3D(FVector(HalfExtentM * 2.f, HalfExtentM * 2.f, 1.f));
	Plane->SetVisibility(false);

	if (UMaterialInterface* Water = Aeros::LoadOptional(UAerosWeatherSettings::Get()->FloodWaterMaterial))
	{
		Plane->SetMaterial(0, Water);
	}
	else if (UMaterialInstanceDynamic* MID = Plane->CreateDynamicMaterialInstance(0))
	{
		MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.12f, 0.16f, 0.13f)); // muddy flood water
	}
}

void UAerosWaterSurfaceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub || !Sub->IsSimulationRunning() || !Plane) return;

	const FVector Here = GetComponentLocation();
	const FAerosWeatherSample S = Sub->GetWeatherAtLocation(Here);
	const bool bWant = S.WaterSurfaceZ > -1e6f && (S.bOcean ? bShowOcean : S.FloodDepth >= MinFloodDepth);

	if (bWant)
	{
		SmoothedZ = bVisible ? FMath::FInterpTo(SmoothedZ, S.WaterSurfaceZ, DeltaTime, 1.5f) : S.WaterSurfaceZ;
		// Snap XY to 50 m so the surface does not "swim" with the camera
		const double Snap = 5000.0;
		Plane->SetWorldLocation(FVector(FMath::GridSnap(Here.X, Snap), FMath::GridSnap(Here.Y, Snap), SmoothedZ));
	}
	if (bWant != bVisible)
	{
		bVisible = bWant;
		Plane->SetVisibility(bVisible);
	}
}
