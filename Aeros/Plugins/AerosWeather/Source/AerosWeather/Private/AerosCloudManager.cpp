// Copyright Aeros. All Rights Reserved.

#include "AerosCloudManager.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosWeatherModule.h"
#include "Core/AerosSnapshot.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "RenderUtils.h"
#include "Misc/PackageName.h"

DECLARE_CYCLE_STAT(TEXT("Aeros Cloud Textures"), STAT_AerosCloudTextures, STATGROUP_Aeros);

void UAerosCloudManager::Initialize(UAerosWeatherSubsystem* InOwner)
{
	Owner = InOwner;
	const TSoftObjectPtr<UMaterialParameterCollection>& Soft = UAerosWeatherSettings::Get()->WeatherParameterCollection;
	const FSoftObjectPath Path = Soft.ToSoftObjectPath();
	if (Path.IsValid() && FPackageName::DoesPackageExist(Path.GetLongPackageName()))
	{
		Collection = Soft.LoadSynchronous();
	}
}

void UAerosCloudManager::Shutdown()
{
	Owner = nullptr;
	WeatherMap = nullptr;
	SurfaceMap = nullptr;
}

void UAerosCloudManager::EnsureTextures(int32 NX, int32 NY)
{
	if (WeatherMap && TexW == NX && TexH == NY) return;
	TexW = NX;
	TexH = NY;
	auto Make = [NX, NY](const TCHAR* Name)
	{
		UTexture2D* T = UTexture2D::CreateTransient(NX, NY, PF_B8G8R8A8, FName(Name));
		T->SRGB = false;
		T->Filter = TF_Bilinear;
		T->AddressX = TA_Clamp;
		T->AddressY = TA_Clamp;
		T->CompressionSettings = TC_VectorDisplacementmap;
		T->UpdateResource();
		return T;
	};
	WeatherMap = Make(TEXT("AerosWeatherMap"));
	SurfaceMap = Make(TEXT("AerosSurfaceMap"));
	HazardMap = Make(TEXT("AerosHazardMap"));
	WeatherPixels.SetNumZeroed(NX * NY);
	SurfacePixels.SetNumZeroed(NX * NY);
	HazardPixels.SetNumZeroed(NX * NY);
}

void UAerosCloudManager::Upload(UTexture2D* Texture, const TArray<FColor>& Pixels, int32 W, int32 H)
{
	if (!Texture) return;
	const int32 Bytes = W * H * 4;
	uint8* Data = static_cast<uint8*>(FMemory::Malloc(Bytes));
	FMemory::Memcpy(Data, Pixels.GetData(), Bytes);
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, W, H);
	Texture->UpdateTextureRegions(0, 1, Region, W * 4, 4, Data,
		[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
		{
			FMemory::Free(SrcData);
			delete Regions;
		});
}

void UAerosCloudManager::Tick(float DeltaTime)
{
	if (!Owner) return;
	Timer += DeltaTime;
	if (Timer >= 1.f / FMath::Max(TextureUpdateRate, 1.f))
	{
		Timer = 0.f;
		UpdateTextures();
	}
	UpdateParameterCollection(DeltaTime);
}

void UAerosCloudManager::UpdateTextures()
{
	SCOPE_CYCLE_COUNTER(STAT_AerosCloudTextures);
	FAerosSnapshotPtr Cur = Owner->GetSnapshot();
	FAerosSnapshotPtr Prev = Owner->GetPreviousSnapshot();
	if (!Cur || !Cur->IsValid()) return;
	if (!Prev || !Prev->IsValid() || Prev->NX != Cur->NX || Prev->NY != Cur->NY || Prev->DomainCenter != Cur->DomainCenter) Prev = Cur;

	EnsureTextures(Cur->NX, Cur->NY);
	const float A = Owner->GetSnapshotAlpha();
	const int32 NX = Cur->NX, NY = Cur->NY;
	auto Byte = [](float V) { return uint8(FMath::Clamp(V, 0.f, 1.f) * 255.f + 0.5f); };

	for (int32 J = 0; J < NY; ++J)
	{
		const int32 Row = (NY - 1 - J) * NX; // texture V grows with world +Y (= south)
		for (int32 I = 0; I < NX; ++I)
		{
			const FAerosSnapCell& P = Prev->Cells[I + J * NX];
			const FAerosSnapCell& C = Cur->Cells[I + J * NX];
			auto L = [A](float X, float Y) { return X + (Y - X) * A; };
			const float Cover = L(P.CloudCover, C.CloudCover);
			const float Base = L(P.CloudBase, C.CloudBase);
			const float Top = L(P.CloudTop, C.CloudTop);
			const float Rain = L(P.PrecipRate, C.PrecipRate);
			WeatherPixels[Row + I] = FColor(Byte(Cover), Byte(Base / 5000.f), Byte(Top / 16000.f), Byte(FMath::Sqrt(Rain / 100.f)));
			SurfacePixels[Row + I] = FColor(Byte(L(P.Wet, C.Wet)), Byte(L(P.Snow, C.Snow) / 50.f), Byte(L(P.HailCover, C.HailCover)), Byte((L(P.T, C.T) + 40.f) / 90.f));
			HazardPixels[Row + I] = FColor(Byte(FMath::Sqrt(L(P.Flood, C.Flood) / 5.f)), Byte(L(P.Dust, C.Dust) / 50.f),
				Byte(FMath::Sqrt(L(P.AshDepth, C.AshDepth) / 100.f)), Byte(0.5f + L(P.Eta, C.Eta) / 10.f));
		}
	}
	Upload(WeatherMap, WeatherPixels, NX, NY);
	Upload(SurfaceMap, SurfacePixels, NX, NY);
	Upload(HazardMap, HazardPixels, NX, NY);
}

void UAerosCloudManager::UpdateParameterCollection(float DeltaTime)
{
	// Lightning flash: driven by the lightning actors, stroke by stroke (they know the timing)
	if (PendingFlash > 0.f && PendingFlash >= Flash - DeltaTime * 12.f) FlashLocation = PendingLocation;
	Flash = FMath::Max(PendingFlash, Flash - DeltaTime * 12.f);
	PendingFlash = 0.f;

	UWorld* World = Owner->GetWorld();
	if (!Collection || !World) return;
	UMaterialParameterCollectionInstance* MPC = World->GetParameterCollectionInstance(Collection);
	if (!MPC) return;

	const FAerosWeatherSample S = Owner->GetWeatherAtLocation(Owner->GetViewerLocation());
	const bool bSnow = S.PrecipitationType == EAerosPrecipType::Snow || S.PrecipitationType == EAerosPrecipType::Sleet;
	const bool bHail = S.PrecipitationType == EAerosPrecipType::Hail;
	const float Intensity = FMath::Clamp(FMath::Sqrt(S.PrecipitationRate / 50.f), 0.f, 1.f);

	MPC->SetScalarParameterValue(TEXT("AerosWetness"), S.Wetness);
	MPC->SetScalarParameterValue(TEXT("AerosSnowDepth"), S.SnowDepth);
	MPC->SetScalarParameterValue(TEXT("AerosHailCover"), S.HailCover);
	MPC->SetScalarParameterValue(TEXT("AerosRainIntensity"), bSnow ? 0.f : Intensity);
	MPC->SetScalarParameterValue(TEXT("AerosSnowIntensity"), bSnow ? Intensity : 0.f);
	MPC->SetScalarParameterValue(TEXT("AerosHailIntensity"), bHail ? Intensity : 0.f);
	MPC->SetScalarParameterValue(TEXT("AerosCloudCover"), S.CloudCover);
	MPC->SetScalarParameterValue(TEXT("AerosTemperature"), S.Temperature);
	MPC->SetScalarParameterValue(TEXT("AerosWindSpeed"), S.WindSpeed);
	MPC->SetScalarParameterValue(TEXT("AerosLightningFlash"), Flash);
	MPC->SetScalarParameterValue(TEXT("AerosFogDensity"), FMath::Clamp(1.f - S.Visibility / 20000.f, 0.f, 1.f));
	MPC->SetScalarParameterValue(TEXT("AerosFloodDepth"), S.FloodDepth);
	MPC->SetScalarParameterValue(TEXT("AerosDust"), S.Dust);
	MPC->SetScalarParameterValue(TEXT("AerosAshDepth"), S.AshDepth);
	MPC->SetScalarParameterValue(TEXT("AerosAshFall"), S.AshFall);
	MPC->SetScalarParameterValue(TEXT("AerosBlowingSnow"), S.BlowingSnow);
	MPC->SetScalarParameterValue(TEXT("AerosShake"), Owner->GetGroundShakingAt(Owner->GetViewerLocation()));
	MPC->SetScalarParameterValue(TEXT("AerosSeaLevel"), S.SeaLevelAnomaly);
	MPC->SetVectorParameterValue(TEXT("AerosWind"), FLinearColor(float(S.Wind.X), float(S.Wind.Y), float(S.Wind.Z), S.WindSpeed));
	const FVector4 D = Owner->GetDomainRect();
	MPC->SetVectorParameterValue(TEXT("AerosDomain"), FLinearColor(float(D.X), float(D.Y), float(D.Z), float(D.W)));
}
