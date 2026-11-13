// Copyright Aeros. All Rights Reserved.

#include "AerosLightning.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosCloudManager.h"
#include "AerosAssetUtils.h"
#include "AerosWeatherModule.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "Components/PointLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

// Unity build: helpers in a uniquely named namespace (see CLAUDE.md).
namespace AerosLightningInternal
{
	constexpr int32 MaxSegments = 700;

	FVector GroundBelow(const UWorld* World, const FVector& Loc)
	{
		if (!World) return Loc;
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosLightningGround), false);
		if (World->LineTraceSingleByObjectType(Hit, FVector(Loc.X, Loc.Y, 2000000.0), FVector(Loc.X, Loc.Y, -2000000.0),
			FCollisionObjectQueryParams(ECC_WorldStatic), Params))
		{
			return Hit.ImpactPoint;
		}
		return FVector(Loc.X, Loc.Y, 0.0);
	}

	/** Fractal channel between A and B: iterative midpoint displacement perpendicular to AB.
	 *  Roughness ~0.15-0.25 gives the zig-zag tortuosity of real lightning at every scale. */
	void FractalPath(const FVector& A, const FVector& B, float Roughness, int32 Depth, FRandomStream& Rand, TArray<FVector>& Out)
	{
		const FVector Dir = (B - A).GetSafeNormal();
		FVector Side = FVector::CrossProduct(Dir, FVector::UpVector);
		if (Side.IsNearlyZero()) Side = FVector::CrossProduct(Dir, FVector::ForwardVector);
		Side.Normalize();
		const FVector Side2 = FVector::CrossProduct(Dir, Side);

		Out.Reset();
		Out.Add(A);
		Out.Add(B);
		float Amp = float((B - A).Size()) * Roughness;
		for (int32 D = 0; D < Depth; ++D)
		{
			TArray<FVector> Next;
			Next.Reserve(Out.Num() * 2);
			for (int32 K = 0; K + 1 < Out.Num(); ++K)
			{
				Next.Add(Out[K]);
				const FVector Mid = (Out[K] + Out[K + 1]) * 0.5
					+ Side * (Rand.FRandRange(-1.f, 1.f) * Amp) + Side2 * (Rand.FRandRange(-1.f, 1.f) * Amp);
				Next.Add(Mid);
			}
			Next.Add(Out.Last());
			Out = MoveTemp(Next);
			Amp *= 0.55f;
		}
	}

	/** Engine cylinder (1 m tall, 1 m wide, centred pivot) stretched from A to B. */
	FTransform SegmentTransform(const FVector& A, const FVector& B, float WidthCm)
	{
		const FVector D = B - A;
		return FTransform(FRotationMatrix::MakeFromZ(D.GetSafeNormal()).Rotator(), (A + B) * 0.5,
			FVector(WidthCm / 100.f, WidthCm / 100.f, FMath::Max(float(D.Size()), 1.f) / 100.f));
	}
}

AAerosLightningFlash::AAerosLightningFlash()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	RootComponent = Light;
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(0.f);
	Light->SetAttenuationRadius(2000000.f);
	Light->SetLightColor(FLinearColor(0.82f, 0.86f, 1.f));
	Light->SetCastShadows(false);
	Light->SetMobility(EComponentMobility::Movable);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> BoltMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	Bolt = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Bolt"));
	Bolt->SetupAttachment(RootComponent);
	Bolt->SetUsingAbsoluteLocation(true);
	Bolt->SetUsingAbsoluteRotation(true);
	Bolt->SetUsingAbsoluteScale(true);
	Bolt->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Bolt->SetCastShadow(false);
	Bolt->SetVisibility(false);
	Bolt->SetNumCustomDataFloats(1);
	if (BoltMesh.Succeeded()) Bolt->SetStaticMesh(BoltMesh.Object);
}

void AAerosLightningFlash::GrowBranch(const TArray<FVector>& Path, int32 Level, float Width, float StartArrival, float Speed,
	const FVector& Gravity, FRandomStream& Rand)
{
	using namespace AerosLightningInternal;
	static const float LevelGlow[] = { 1.f, 0.45f, 0.22f, 0.1f };
	const float Glow = LevelGlow[FMath::Min(Level, 3)];

	TArray<float> ArrivalAt;
	ArrivalAt.SetNum(Path.Num());
	ArrivalAt[0] = StartArrival;
	for (int32 K = 0; K + 1 < Path.Num(); ++K)
	{
		ArrivalAt[K + 1] = ArrivalAt[K] + float(FVector::Dist(Path[K], Path[K + 1])) / Speed;
		if (Segments.Num() >= MaxSegments) return;
		FSegment S;
		S.A = Path[K];
		S.B = Path[K + 1];
		S.Width = Width;
		S.Arrival = ArrivalAt[K + 1];
		S.Glow = Glow;
		S.Level = Level;
		Segments.Add(S);
	}
	if (Level >= 3) return;

	// Branches fork mostly from the upper part of a channel and die out (stepped leader)
	const float Chance = (Level == 0 ? 0.16f : 0.09f) * (Type == EAerosLightningType::PositiveCloudToGround ? 0.35f : 1.f);
	float Remaining = 0.f;
	for (int32 K = 0; K + 1 < Path.Num(); ++K) Remaining += float(FVector::Dist(Path[K], Path[K + 1]));
	float Walked = 0.f;
	for (int32 K = 1; K + 1 < Path.Num(); ++K)
	{
		Walked += float(FVector::Dist(Path[K - 1], Path[K]));
		const float Frac = Walked / FMath::Max(Remaining, 1.f);
		if (Frac > 0.85f || Rand.FRand() > Chance * (1.2f - Frac)) continue;

		const FVector Dir = (Path[K + 1] - Path[K]).GetSafeNormal();
		FVector Lateral = FVector(Rand.FRandRange(-1.f, 1.f), Rand.FRandRange(-1.f, 1.f), Gravity.IsNearlyZero() ? 0.f : Rand.FRandRange(-0.3f, 0.3f));
		Lateral = (Lateral - Dir * FVector::DotProduct(Lateral, Dir)).GetSafeNormal();
		const FVector ChildDir = (Dir * 0.55f + Lateral * 0.85f + Gravity * 0.45f).GetSafeNormal();
		const float Length = (Remaining - Walked) * Rand.FRandRange(0.12f, 0.4f) * FMath::Pow(0.7f, float(Level));
		if (Length < 3000.f) continue;

		TArray<FVector> Child;
		FractalPath(Path[K], Path[K] + ChildDir * Length, 0.22f, Level == 0 ? 5 : 4, Rand, Child);
		GrowBranch(Child, Level + 1, Width * 0.45f, ArrivalAt[K], Speed * Rand.FRandRange(0.8f, 1.f), Gravity, Rand);
	}
}

void AAerosLightningFlash::BuildChannel(const FVector& Start, const FVector& End, const FVector& ViewerLocation, FRandomStream& Rand)
{
	using namespace AerosLightningInternal;
	Segments.Reset();
	const bool bIntra = Type == EAerosLightningType::IntraCloud;
	const float Roughness = Type == EAerosLightningType::PositiveCloudToGround ? 0.12f : (bIntra ? 0.2f : 0.17f);
	TArray<FVector> Main;
	FractalPath(Start, End, Roughness, 6, Rand, Main); // 64 segments

	float Length = 0.f;
	for (int32 K = 0; K + 1 < Main.Num(); ++K) Length += float(FVector::Dist(Main[K], Main[K + 1]));
	const float Speed = Length / FMath::Max(LeaderDuration, 0.01f);
	const FVector Gravity = bIntra ? FVector::ZeroVector : FVector(0, 0, -1);
	GrowBranch(Main, 0, 1.f, 0.f, Speed, Gravity, Rand);

	// Width: physical channel is thin, but it must stay ~2 px wide on screen or it shimmers away
	const float Current = FMath::Sqrt(FMath::Max(Peak, 1.f) / 1.2e7f);
	for (FSegment& S : Segments)
	{
		const float DistCm = float(FVector::Dist((S.A + S.B) * 0.5, ViewerLocation));
		const float Physical = 120.f * Current * S.Width;
		S.Width = FMath::Max(Physical, DistCm * 0.0016f * FMath::Sqrt(S.Width));
	}
	Segments.Sort([](const FSegment& A, const FSegment& B) { return A.Arrival < B.Arrival; });
	NumShown = 0;
	bBranchesDimmed = false;
	bVisibleChannel = true;
	Bolt->ClearInstances();
	Bolt->SetVisibility(true);
}

void AAerosLightningFlash::RevealSegments()
{
	using namespace AerosLightningInternal;
	if (!bVisibleChannel || NumShown >= Segments.Num()) return;
	// The leader grows segment by segment; when the return stroke fires the whole channel is lit
	const float Limit = Age >= LeaderDuration ? 1e9f : Age;
	TArray<FTransform> Add;
	const int32 First = NumShown;
	while (NumShown < Segments.Num() && Segments[NumShown].Arrival <= Limit)
	{
		const FSegment& S = Segments[NumShown++];
		Add.Add(SegmentTransform(S.A, S.B, S.Width));
	}
	if (Add.Num() == 0) return;
	Bolt->AddInstances(Add, false, true);
	for (int32 K = 0; K < Add.Num(); ++K)
	{
		Bolt->SetCustomDataValue(First + K, 0, Segments[First + K].Glow, false);
	}
	Bolt->MarkRenderStateDirty();
}

float AAerosLightningFlash::StrokeBrightness(float T) const
{
	float B = 0.f;
	for (int32 K = 0; K < StrokeTimes.Num(); ++K)
	{
		if (T >= StrokeTimes[K]) B += StrokeAmps[K] * FMath::Exp(-(T - StrokeTimes[K]) / 0.03f);
	}
	if (ContinuingCurrent > 0.f && T >= LeaderDuration)
	{
		B += T <= ContinuingUntil ? ContinuingCurrent : ContinuingCurrent * FMath::Exp(-(T - ContinuingUntil) / 0.06f);
	}
	return B;
}

float AAerosLightningFlash::GetBrightness() const
{
	if (Age < LeaderDuration) return Type == EAerosLightningType::IntraCloud ? StrokeBrightness(Age) : 0.1f; // faint stepped leader
	return StrokeBrightness(Age);
}

void AAerosLightningFlash::Fire(const FAerosLightningStrike& Strike, const FVector& ViewerLocation)
{
	using namespace AerosLightningInternal;
	const FVector Ground = GroundBelow(GetWorld(), Strike.Location);
	FRandomStream Rand(int32(uint32(Strike.Sequence) * 2654435761u));
	const float Roll = Rand.FRand();
	Type = !Strike.bCloudToGround ? EAerosLightningType::IntraCloud
		: (Roll < 0.1f ? EAerosLightningType::PositiveCloudToGround : EAerosLightningType::NegativeCloudToGround);

	float CurrentKA = Strike.PeakCurrent;
	FVector Start = Ground, End = Ground, LightPos;
	StrokeTimes.Reset();
	StrokeAmps.Reset();
	ContinuingCurrent = 0.f;
	FlashWeight = 1.f;
	bool bChannel = true;

	switch (Type)
	{
	case EAerosLightningType::NegativeCloudToGround:
	{
		// From just above the cloud base, a few km of tortuous channel; 1-8 return strokes
		Start = Ground + FVector(Rand.FRandRange(-150000.f, 150000.f), Rand.FRandRange(-150000.f, 150000.f), Rand.FRandRange(150000.f, 260000.f));
		LeaderDuration = Rand.FRandRange(0.03f, 0.06f);
		const int32 Strokes = 1 + FMath::Min(7, int32(-FMath::Loge(FMath::Max(1.f - Rand.FRand(), 0.001f)) * 2.2f));
		float T = LeaderDuration;
		for (int32 K = 0; K < Strokes; ++K)
		{
			StrokeTimes.Add(T);
			StrokeAmps.Add(K == 0 ? 1.f : Rand.FRandRange(0.45f, 0.9f));
			T += Rand.FRandRange(0.035f, 0.12f);
		}
		if (Rand.FRand() < 0.35f)
		{
			ContinuingCurrent = 0.22f;
			ContinuingUntil = StrokeTimes.Last() + Rand.FRandRange(0.08f, 0.25f);
		}
		break;
	}
	case EAerosLightningType::PositiveCloudToGround:
	{
		// From the anvil, far from the core: long slanted channel, one huge stroke, long continuing current
		CurrentKA *= 2.5f;
		const float Angle = Rand.FRandRange(0.f, 2.f * PI);
		const float Reach = Rand.FRandRange(600000.f, 1500000.f);
		Start = Ground + FVector(FMath::Cos(Angle) * Reach, FMath::Sin(Angle) * Reach, Rand.FRandRange(700000.f, 1000000.f));
		LeaderDuration = Rand.FRandRange(0.06f, 0.1f);
		StrokeTimes.Add(LeaderDuration);
		StrokeAmps.Add(1.6f);
		ContinuingCurrent = 0.6f;
		ContinuingUntil = LeaderDuration + Rand.FRandRange(0.2f, 0.5f);
		break;
	}
	case EAerosLightningType::IntraCloud:
	{
		// Mostly hidden inside the cloud (only the cloud glows); sometimes a slow "anvil crawler"
		FlashWeight = 0.6f;
		CurrentKA *= 0.6f;
		bChannel = Rand.FRand() < 0.45f;
		const float Angle = Rand.FRandRange(0.f, 2.f * PI);
		const float Reach = Rand.FRandRange(600000.f, 1800000.f);
		Start = Ground + FVector(0, 0, Rand.FRandRange(250000.f, 400000.f));
		End = Start + FVector(FMath::Cos(Angle) * Reach, FMath::Sin(Angle) * Reach, Rand.FRandRange(-50000.f, 80000.f));
		LeaderDuration = Rand.FRandRange(0.25f, 0.6f);
		const int32 Pulses = Rand.RandRange(3, 6);
		for (int32 K = 0; K < Pulses; ++K)
		{
			StrokeTimes.Add(LeaderDuration * Rand.FRandRange(0.1f, 1.3f));
			StrokeAmps.Add(Rand.FRandRange(0.3f, 0.7f));
		}
		StrokeTimes.Sort();
		if (!bChannel) Start.Z = Ground.Z + Rand.FRandRange(500000.f, 700000.f);
		break;
	}
	}

	EndTime = FMath::Max(StrokeTimes.Num() > 0 ? StrokeTimes.Last() + 0.25f : 0.3f, ContinuingCurrent > 0.f ? ContinuingUntil + 0.3f : 0.f);
	Peak = CurrentKA * (Type == EAerosLightningType::IntraCloud ? 4.0e4f : 1.0e5f);
	BaseIntensity = 25.f + 1.2f * CurrentKA;
	LightPos = bChannel ? (Start + End) * 0.5 : Start;
	SetActorLocation(LightPos);
	Age = 0.f;
	Segments.Reset();
	bVisibleChannel = false;

	const UAerosWeatherSettings* Settings = UAerosWeatherSettings::Get();
	if (UNiagaraSystem* BoltSystem = Settings->LightningSystem.LoadSynchronous(); BoltSystem && Strike.bCloudToGround)
	{
		if (UNiagaraComponent* NC = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, BoltSystem, Ground))
		{
			NC->SetVariableVec3(TEXT("Start"), Start);
			NC->SetVariableVec3(TEXT("End"), Ground);
		}
	}
	else if (Settings->bUseFallbackVisuals && bChannel)
	{
		if (!BoltMID)
		{
			UMaterialInterface* Mat = Aeros::LoadOptional(TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/AerosWeather/Materials/M_AerosLightning.M_AerosLightning"))));
			if (!Mat) Mat = Aeros::LoadOptional(TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/AerosWeather/Materials/M_AerosEmissive.M_AerosEmissive"))));
			if (Mat) Bolt->SetMaterial(0, Mat);
			BoltMID = Bolt->CreateDynamicMaterialInstance(0);
			if (BoltMID) BoltMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.75f, 0.82f, 1.f));
		}
		BuildChannel(Start, End, ViewerLocation, Rand);
	}

	UE_LOG(LogAeros, Verbose, TEXT("Aeros lightning: type %d, %.1f kA, %.1f km from viewer, %d segments"), int32(Type), CurrentKA,
		FVector::Dist2D(Ground, ViewerLocation) * 1e-5, Segments.Num());

	// Thunder: speed of sound, real time (sound does not care about the weather time scale)
	const float DistM = float(FVector::Dist(Ground, ViewerLocation)) * 0.01f;
	ThunderDelay = DistM / 343.f;
	ThunderVolume = FMath::Clamp(1.f - DistM / (Settings->LightningVisualRangeKm * 1000.f), 0.1f, 1.f);
	bThunderPlayed = false;
	SetLifeSpan(FMath::Max(EndTime + 0.2f, ThunderDelay + 0.5f));
}

void AAerosLightningFlash::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	const float B = GetBrightness();
	Light->SetIntensity(Peak * FMath::Min(B, 1.6f));

	if (bVisibleChannel)
	{
		RevealSegments();
		// Later strokes only follow the main channel: the branches fade after the first one
		if (!bBranchesDimmed && StrokeTimes.Num() > 0 && Age > StrokeTimes[0] + 0.05f)
		{
			bBranchesDimmed = true;
			for (int32 K = 0; K < NumShown; ++K)
			{
				if (Segments[K].Level > 0) Bolt->SetCustomDataValue(K, 0, Segments[K].Glow * 0.15f, false);
			}
			Bolt->MarkRenderStateDirty();
		}
		if (BoltMID) BoltMID->SetScalarParameterValue(TEXT("Intensity"), BaseIntensity * B);
		if (Age > EndTime && B < 0.01f)
		{
			bVisibleChannel = false;
			Bolt->SetVisibility(false);
		}
	}

	// Light up the clouds / screen, stroke by stroke
	if (Age <= EndTime + 0.2f)
	{
		if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
		{
			if (UAerosCloudManager* Clouds = Sub->GetCloudManager())
			{
				const float DistKm = float(FVector::Dist2D(GetActorLocation(), Sub->GetViewerLocation())) * 1e-5f;
				Clouds->ReportLightningFlash(FlashWeight * FMath::Min(B, 1.f) * FMath::Clamp(1.f - DistKm / 30.f, 0.f, 1.f), GetActorLocation());
			}
		}
	}

	if (!bThunderPlayed && Age >= ThunderDelay)
	{
		bThunderPlayed = true;
		if (USoundBase* Snd = UAerosWeatherSettings::Get()->ThunderSound.LoadSynchronous())
		{
			UGameplayStatics::PlaySoundAtLocation(this, Snd, GetActorLocation(), ThunderVolume);
		}
	}
}
