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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"

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
	ShapeMap = Make(TEXT("AerosCloudShapeMap"));
	WeatherPixels.SetNumZeroed(NX * NY);
	SurfacePixels.SetNumZeroed(NX * NY);
	HazardPixels.SetNumZeroed(NX * NY);
	ShapePixels.SetNumZeroed(NX * NY);
	AnvilAcc.SetNumZeroed(NX * NY);
	ShapeGeneration = MAX_uint64;
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
	ShapeTimer += DeltaTime;

	// Cloud noise drifts with the ~1.5-5 km wind (simulated time), so billows move with the weather
	if (FAerosSnapshotPtr Snap = Owner->GetSnapshot())
	{
		const FVector2D W = (Snap->Synoptic.Wind850 + Snap->Synoptic.Wind500) * 0.5f;      // m/s, sim frame
		const float SimSeconds = Owner->IsPaused() ? 0.f : DeltaTime * Owner->GetTimeScale();
		NoiseOffsetKm += FVector(W.X, -W.Y, 0.0) * 0.001 * SimSeconds;                      // Unreal: -Y = north
		NoiseOffsetKm.X = FMath::Fmod(NoiseOffsetKm.X, 10000.0);
		NoiseOffsetKm.Y = FMath::Fmod(NoiseOffsetKm.Y, 10000.0);
	}
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

	// Shape map: only when a new snapshot arrived, at most 4 times per second (anvil splatting costs)
	if (Cur->Generation != ShapeGeneration && ShapeTimer >= 0.25f)
	{
		ShapeTimer = 0.f;
		ShapeGeneration = Cur->Generation;
		UpdateShapeMap(*Cur);
	}
}

void UAerosCloudManager::DumpMaps(const FString& Prefix) const
{
	if (TexW <= 0 || TexH <= 0) return;
	struct FChannel { const TArray<FColor>* Pixels; int32 Index; const TCHAR* Name; };
	const FChannel Channels[] = {
		{ &WeatherPixels, 0, TEXT("cover") }, { &WeatherPixels, 1, TEXT("base") }, { &WeatherPixels, 2, TEXT("top") }, { &WeatherPixels, 3, TEXT("precip") },
		{ &ShapePixels, 0, TEXT("anvil") }, { &ShapePixels, 1, TEXT("updraft") }, { &ShapePixels, 2, TEXT("gust") }, { &ShapePixels, 3, TEXT("conv") },
	};
	for (const FChannel& C : Channels)
	{
		if (C.Pixels->Num() != TexW * TexH) continue;
		TArray<FColor> Grey;
		Grey.SetNumUninitialized(TexW * TexH);
		for (int32 K = 0; K < Grey.Num(); ++K)
		{
			const FColor& P = (*C.Pixels)[K];
			const uint8 V = C.Index == 0 ? P.R : (C.Index == 1 ? P.G : (C.Index == 2 ? P.B : P.A));
			Grey[K] = FColor(V, V, V, 255);
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(TexW, TexH, Grey, Png);
		FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("AerosDebug") / (Prefix + TEXT("_") + C.Name + TEXT(".png"))));
	}
}

void UAerosCloudManager::UpdateShapeMap(const FAerosSnapshot& Snap)
{
	const int32 NX = Snap.NX, NY = Snap.NY;
	const float CellKm = Snap.CellM * 0.001f;
	auto Byte = [](float V) { return uint8(FMath::Clamp(V, 0.f, 1.f) * 255.f + 0.5f); };

	// --- Anvils: deep storms spread their top downwind (500 hPa wind), widening with distance
	FMemory::Memzero(AnvilAcc.GetData(), AnvilAcc.Num() * sizeof(float));
	FVector2D Dir(Snap.Synoptic.Wind500.X, Snap.Synoptic.Wind500.Y);   // sim frame: x east (I), y north (J)
	const float Upper = float(Dir.Size());
	Dir = Upper > 0.5f ? Dir / Upper : FVector2D(1.0, 0.0);
	int32 Sources = 0;
	for (int32 J = 0; J < NY && Sources < 600; ++J)
	{
		for (int32 I = 0; I < NX && Sources < 600; ++I)
		{
			const FAerosSnapCell& C = Snap.Cells[I + J * NX];
			if (C.CloudTop < 9000.f || C.Conv < 0.35f) continue; // only mature cumulonimbus reach the tropopause
			++Sources;
			const float Strength = FMath::Clamp((C.CloudTop - 9000.f) / 4000.f, 0.f, 1.f) * FMath::Clamp((C.Conv - 0.35f) * 2.5f, 0.f, 1.f);
			// Stronger upper wind and stronger storms = longer anvils (15-70 km)
			const float LengthKm = 15.f + 35.f * Strength + 0.8f * FMath::Min(Upper, 20.f);
			const int32 Steps = FMath::Max(2, int32(LengthKm / CellKm));
			for (int32 Step = -2; Step <= Steps; ++Step)
			{
				const float Along = float(Step) * CellKm;
				const float Fade = Step < 0 ? 0.6f : FMath::Sqrt(1.f - float(Step) / float(Steps + 1));
				const float RadiusCells = (3.f + 0.18f * FMath::Max(Along, 0.f)) / CellKm;
				const float CX = I + 0.5f + float(Dir.X) * Along / CellKm;
				const float CY = J + 0.5f + float(Dir.Y) * Along / CellKm;
				const int32 R = FMath::CeilToInt(RadiusCells);
				for (int32 DY = -R; DY <= R; ++DY)
				{
					const int32 Y = int32(CY) + DY;
					if (Y < 0 || Y >= NY) continue;
					for (int32 DX = -R; DX <= R; ++DX)
					{
						const int32 X = int32(CX) + DX;
						if (X < 0 || X >= NX) continue;
						const float D2 = (FMath::Square(X + 0.5f - CX) + FMath::Square(Y + 0.5f - CY)) / FMath::Square(RadiusCells);
						if (D2 >= 1.f) continue;
						float& Acc = AnvilAcc[X + Y * NX];
						Acc = FMath::Max(Acc, Strength * Fade * (1.f - D2 * D2));
					}
				}
			}
		}
	}

	// --- Gust fronts (shelf clouds): surface convergence next to heavy rain (cold outflow edge)
	const float Inv2Dx = 1.f / (2.f * Snap.CellM);
	for (int32 J = 0; J < NY; ++J)
	{
		const int32 Row = (NY - 1 - J) * NX; // texture V grows with world +Y (= south), like the weather map
		for (int32 I = 0; I < NX; ++I)
		{
			const FAerosSnapCell& C = Snap.Cells[I + J * NX];
			const float Div = (Snap.At(I + 1, J).Us - Snap.At(I - 1, J).Us) * Inv2Dx + (Snap.At(I, J + 1).Vs - Snap.At(I, J - 1).Vs) * Inv2Dx;
			float RainNear = 0.f;
			for (int32 DY = -3; DY <= 3; ++DY)
				for (int32 DX = -3; DX <= 3; ++DX)
					RainNear = FMath::Max(RainNear, Snap.At(I + DX, J + DY).PrecipRate);
			const float Gust = FMath::Clamp(-Div * 1500.f, 0.f, 1.f) * FMath::Clamp(RainNear / 15.f, 0.f, 1.f) * (C.PrecipRate < 20.f ? 1.f : 0.3f);
			ShapePixels[Row + I] = FColor(Byte(AnvilAcc[I + J * NX]), Byte(C.W / 30.f), Byte(Gust), Byte(C.Conv));
		}
	}
	Upload(ShapeMap, ShapePixels, NX, NY);
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
