// Copyright Aeros. All Rights Reserved.

#include "AerosWeatherRadar.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherModule.h"
#include "AerosTornado.h"
#include "Core/AerosSnapshot.h"
#include "Engine/Texture2D.h"
#include "RenderUtils.h"

DECLARE_CYCLE_STAT(TEXT("Aeros Radar"), STAT_AerosRadar, STATGROUP_Aeros);

AAerosWeatherRadar::AAerosWeatherRadar()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	bReplicates = false; // every machine scans its own deterministic simulation
}

void AAerosWeatherRadar::BeginPlay()
{
	Super::BeginPlay();
	EnsureTexture();
}

FVector AAerosWeatherRadar::GetRadarLocation() const
{
	return ScanCenter;
}

void AAerosWeatherRadar::SetProduct(EAerosRadarProduct InProduct)
{
	Product = InProduct;
	ScanSector(0.f, 360.f); // full repaint so the product switch is instant
}

void AAerosWeatherRadar::EnsureTexture()
{
	if (RadarTexture && RadarTexture->GetSizeX() == Resolution) return;
	RadarTexture = UTexture2D::CreateTransient(Resolution, Resolution, PF_B8G8R8A8, TEXT("AerosRadar"));
	RadarTexture->SRGB = true;
	RadarTexture->Filter = TF_Bilinear;
	RadarTexture->CompressionSettings = TC_VectorDisplacementmap;
	RadarTexture->UpdateResource();

	Pixels.Init(FColor(0, 0, 0, 0), Resolution * Resolution);
	PixelAzimuth.SetNum(Resolution * Resolution);
	const float Half = Resolution * 0.5f;
	for (int32 Y = 0; Y < Resolution; ++Y)
	{
		for (int32 X = 0; X < Resolution; ++X)
		{
			const float DX = (X + 0.5f - Half) / Half;   // east
			const float DY = (Half - Y - 0.5f) / Half;   // north (texture up)
			const float R = FMath::Sqrt(DX * DX + DY * DY);
			PixelAzimuth[X + Y * Resolution] = R <= 1.f ? FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(DX, DY)) + 360.f, 360.f) : -1.f;
		}
	}
}

FLinearColor AAerosWeatherRadar::ReflectivityColor(float DBZ)
{
	struct FStop { float DBZ; FLinearColor C; };
	static const FStop Stops[] = {
		{ 5.f,  FLinearColor(0.02f, 0.80f, 0.93f) }, { 10.f, FLinearColor(0.00f, 0.60f, 0.95f) },
		{ 15.f, FLinearColor(0.00f, 0.25f, 0.95f) }, { 20.f, FLinearColor(0.00f, 1.00f, 0.00f) },
		{ 25.f, FLinearColor(0.00f, 0.78f, 0.00f) }, { 30.f, FLinearColor(0.00f, 0.56f, 0.00f) },
		{ 35.f, FLinearColor(1.00f, 1.00f, 0.00f) }, { 40.f, FLinearColor(0.91f, 0.75f, 0.00f) },
		{ 45.f, FLinearColor(1.00f, 0.56f, 0.00f) }, { 50.f, FLinearColor(1.00f, 0.00f, 0.00f) },
		{ 55.f, FLinearColor(0.84f, 0.00f, 0.00f) }, { 60.f, FLinearColor(0.75f, 0.00f, 0.00f) },
		{ 65.f, FLinearColor(1.00f, 0.00f, 1.00f) }, { 70.f, FLinearColor(0.60f, 0.33f, 0.79f) },
		{ 75.f, FLinearColor(1.00f, 1.00f, 1.00f) } };
	if (DBZ < Stops[0].DBZ) return FLinearColor::Transparent;
	FLinearColor C = Stops[0].C;
	for (const FStop& S : Stops) { if (DBZ >= S.DBZ) C = S.C; else break; }
	return C;
}

bool AAerosWeatherRadar::WorldToRadarUV(const FVector& World, FVector2D& OutUV) const
{
	const double RangeCm = RangeKm * 100000.0;
	const FVector D = World - ScanCenter;
	OutUV = FVector2D(0.5 + D.X / (2.0 * RangeCm), 0.5 + D.Y / (2.0 * RangeCm)); // world -Y = north = texture up
	return D.SizeSquared2D() <= RangeCm * RangeCm;
}

FColor AAerosWeatherRadar::PixelColor(const FAerosSnapshot& Snap, const FVector& World) const
{
	const FAerosSnapCell* C = Snap.CellAtWorld(World);
	if (!C) return FColor(0, 0, 0, 0);

	switch (Product)
	{
	case EAerosRadarProduct::Velocity:
	{
		if (C->DBZ < 5.f && C->CloudWater < 0.05f) return FColor(0, 0, 0, 0);
		// Radial velocity of the low-level flow: + away (red), - toward (green)
		const FVector Wind = FAerosSnapshot::SimToWorldDir(C->U850, C->V850);
		const FVector Dir = (World - ScanCenter).GetSafeNormal2D();
		const float V = float(FVector::DotProduct(Wind, Dir));
		const float T = FMath::Clamp(FMath::Abs(V) / 30.f, 0.f, 1.f);
		return (V > 0.f ? FLinearColor(0.35f + 0.65f * T, 0.f, 0.f) : FLinearColor(0.f, 0.35f + 0.65f * T, 0.f)).ToFColor(true);
	}
	case EAerosRadarProduct::PrecipType:
	{
		if (C->PrecipRate < 0.05f) return FColor(0, 0, 0, 0);
		const float T = FMath::Clamp(FMath::Sqrt(C->PrecipRate / 50.f), 0.25f, 1.f);
		FLinearColor Col;
		switch (static_cast<EAerosPrecipType>(C->PType))
		{
		case EAerosPrecipType::Snow:         Col = FLinearColor(0.3f, 0.6f, 1.f); break;
		case EAerosPrecipType::Sleet:        Col = FLinearColor(1.f, 0.5f, 0.9f); break;
		case EAerosPrecipType::FreezingRain: Col = FLinearColor(0.9f, 0.2f, 0.6f); break;
		case EAerosPrecipType::Hail:         Col = FLinearColor(1.f, 0.f, 1.f); break;
		default:                             Col = FLinearColor(0.f, 0.8f, 0.1f); break;
		}
		return (Col * T).ToFColor(true);
	}
	case EAerosRadarProduct::Hazards:
	{
		if (C->bWater)
		{
			// Sea level anomaly: red = crest, blue = trough (tsunami, surge)
			const float T = FMath::Clamp(FMath::Abs(C->Eta) / 3.f, 0.f, 1.f);
			if (T < 0.03f) return FColor(0, 0, 0, 0);
			return (C->Eta > 0.f ? FLinearColor(1.f, 0.2f * (1.f - T), 0.f) : FLinearColor(0.f, 0.3f, 1.f)).ToFColor(true);
		}
		if (C->Flood > 0.02f)
		{
			const float T = FMath::Clamp(FMath::Sqrt(C->Flood / 2.f), 0.25f, 1.f);
			return (FLinearColor(0.1f, 0.4f, 1.f) * T).ToFColor(true);
		}
		if (C->AshFall > 0.01f || C->AshDepth > 1.f)
		{
			const float T = FMath::Clamp(0.3f + C->AshFall + C->AshDepth / 20.f, 0.3f, 1.f);
			return (FLinearColor(0.55f, 0.55f, 0.55f) * T).ToFColor(true);
		}
		if (C->Dust > 0.5f)
		{
			const float T = FMath::Clamp(C->Dust / 20.f, 0.25f, 1.f);
			return (FLinearColor(0.8f, 0.5f, 0.2f) * T).ToFColor(true);
		}
		if (C->Blow > 0.2f) return (FLinearColor(0.85f, 0.9f, 1.f) * C->Blow).ToFColor(true);
		return FColor(0, 0, 0, 0);
	}
	default:
	{
		// Roadmap: magenta = hail / tornado
		if (C->Hail >= 20.f) return FColor(255, 0, 255);
		const FLinearColor Col = ReflectivityColor(C->DBZ);
		return Col.A > 0.f ? FLinearColor(Col.R, Col.G, Col.B, 1.f).ToFColor(true) : FColor(0, 0, 0, 0);
	}
	}
}

void AAerosWeatherRadar::ScanSector(float FromDeg, float ToDeg)
{
	const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	FAerosSnapshotPtr Snap = Sub ? Sub->GetSnapshot() : nullptr;
	if (Pixels.Num() == 0 || !Snap || !Snap->IsValid()) return;
	const float Half = Resolution * 0.5f;
	const double RangeCm = RangeKm * 100000.0;
	const bool bWrap = ToDeg >= 360.f;
	for (int32 K = 0; K < Pixels.Num(); ++K)
	{
		const float Az = PixelAzimuth[K];
		if (Az < 0.f) continue;
		const bool bIn = bWrap ? (Az >= FromDeg || Az < ToDeg - 360.f) : (Az >= FromDeg && Az < ToDeg);
		if (!bIn && !(FromDeg == 0.f && ToDeg >= 360.f)) continue;
		const int32 X = K % Resolution, Y = K / Resolution;
		const FVector World = ScanCenter + FVector((X + 0.5 - Half) / Half * RangeCm, (Y + 0.5 - Half) / Half * RangeCm, 0.0);
		Pixels[K] = PixelColor(*Snap, World);
	}
}

void AAerosWeatherRadar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	SCOPE_CYCLE_COUNTER(STAT_AerosRadar);
	const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub || !Sub->IsSimulationRunning()) return;
	EnsureTexture();

	ScanCenter = bFollowViewer ? Sub->GetViewerLocation() : GetActorLocation();
	ScanCenter.Z = 0.0;

	const float From = BeamAzimuth;
	const float Step = FMath::Clamp(SweepDegreesPerSecond * DeltaSeconds, 0.f, 360.f);
	BeamAzimuth = FMath::Fmod(BeamAzimuth + Step, 360.f);
	ScanSector(From, From + Step);

	// Tornado vortex signatures: magenta dots
	for (const AAerosTornado* T : Sub->GetActiveTornadoes())
	{
		FVector2D UV;
		if (!T || !WorldToRadarUV(T->GetActorLocation(), UV)) continue;
		const int32 PX = int32(UV.X * Resolution), PY = int32(UV.Y * Resolution);
		for (int32 Y = PY - 2; Y <= PY + 2; ++Y)
			for (int32 X = PX - 2; X <= PX + 2; ++X)
				if (X >= 0 && Y >= 0 && X < Resolution && Y < Resolution) Pixels[X + Y * Resolution] = FColor(255, 0, 255);
	}

	UploadTimer += DeltaSeconds;
	if (UploadTimer >= 1.f / 30.f)
	{
		UploadTimer = 0.f;
		const int32 Bytes = Pixels.Num() * 4;
		uint8* Data = static_cast<uint8*>(FMemory::Malloc(Bytes));
		FMemory::Memcpy(Data, Pixels.GetData(), Bytes);
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Resolution, Resolution);
		RadarTexture->UpdateTextureRegions(0, 1, Region, Resolution * 4, 4, Data,
			[](uint8* Src, const FUpdateTextureRegion2D* R) { FMemory::Free(Src); delete R; });
	}
}
