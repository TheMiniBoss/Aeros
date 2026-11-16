// Copyright Aeros. All Rights Reserved.

#include "AerosChasePawn.h"
#include "AerosChaseHUD.h"
#include "AerosDemoGameMode.h"
#include "AerosDemoTerrain.h"
#include "AerosWeatherSubsystem.h"
#include "AerosPrecipitationComponent.h"
#include "AerosWaterSurfaceComponent.h"
#include "AerosTornado.h"
#include "AerosWeatherSettings.h"
#include "AerosWeatherRadar.h"
#include "Core/AerosFunnel.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "InputCoreTypes.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"

namespace AerosChasePawnInternal
{
	bool KeyPressed(APlayerController* PC, std::initializer_list<FKey> Keys)
	{
		for (const FKey& K : Keys) if (PC->WasInputKeyJustPressed(K)) return true;
		return false;
	}

	bool KeyDown(APlayerController* PC, std::initializer_list<FKey> Keys)
	{
		for (const FKey& K : Keys) if (PC->IsInputKeyDown(K)) return true;
		return false;
	}

	FString Km(float Km)
	{
		return Km < 10.f ? FString::Printf(TEXT("%.1f km"), Km).Replace(TEXT("."), TEXT(",")) : FString::Printf(TEXT("%.0f km"), Km);
	}

	// 300 km/h: not a real car, but a supercell moves at ~30 m/s on screen (x2), and the storms are
	// tens of km apart: at 180 km/h closing in from behind took a quarter of an hour.
	constexpr float MaxSpeedMs = 85.f;
	constexpr float CameraHeightCm = 170.f;
}

AAerosChasePawn::AAerosChasePawn()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->bUsePawnControlRotation = false;
	Camera->SetFieldOfView(80.f);

	Precipitation = CreateDefaultSubobject<UAerosPrecipitationComponent>(TEXT("Precipitation"));
	Precipitation->SetupAttachment(Camera);

	WaterSurface = CreateDefaultSubobject<UAerosWaterSurfaceComponent>(TEXT("WaterSurface"));
	WaterSurface->SetupAttachment(RootComponent);

	AutoPossessPlayer = EAutoReceiveInput::Disabled;
}

void AAerosChasePawn::BeginPlay()
{
	Super::BeginPlay();
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetInputMode(FInputModeGameOnly());
		PC->bShowMouseCursor = false;
	}
	HeadingDeg = GetActorRotation().Yaw;
	PlaceOnGround();
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		Sub->OnLightningStrike.AddDynamic(this, &AAerosChasePawn::HandleLightning);
	}
	// Test options: -AerosChase starts the day at once, -AerosChaseAuto drives itself (photos included),
	// -AerosChaseShots=N saves a screenshot every N s, -AerosChaseQuitAfter=S quits,
	// -AerosChaseEndAfter=S ends the day after S s of driving (summary screen), -AerosChaseNear=km
	// sets how close the autopilot keeps to a tornado (3 km)
	bAutopilot = FParse::Param(FCommandLine::Get(), TEXT("AerosChaseAuto"));
	FParse::Value(FCommandLine::Get(), TEXT("AerosChaseQuitAfter="), QuitAfter);
	FParse::Value(FCommandLine::Get(), TEXT("AerosChaseEndAfter="), EndDayAfter);
	FParse::Value(FCommandLine::Get(), TEXT("AerosChaseNear="), AutoTornadoKm);
	FParse::Value(FCommandLine::Get(), TEXT("AerosChaseFromMenu="), MenuEnterAfter);
	if (FParse::Param(FCommandLine::Get(), TEXT("AerosChase")) || bAutopilot) StartChase();
}

void AAerosChasePawn::TuneTornadoGenesis(int32 Mode)
{
	// The plugin rolls for a tornado every 600 simulated s per supercell: at the chase's x2 that is one
	// roll every 5 real minutes. A game needs a roll every ~30 s (still 15-45 % each, and only under
	// a rotating storm in a tornadic environment). None during the briefing: the player should see
	// the first one come down. Restored when the day ends.
	UAerosWeatherSettings* S = GetMutableDefault<UAerosWeatherSettings>();
	if (Mode == 0)
	{
		if (SavedTornadoInterval < 0.f) return;
		S->TornadoCheckInterval = SavedTornadoInterval;
		S->TornadoFrequency = SavedTornadoFrequency;
		SavedTornadoInterval = -1.f;
		return;
	}
	if (SavedTornadoInterval < 0.f)
	{
		SavedTornadoInterval = S->TornadoCheckInterval;
		SavedTornadoFrequency = S->TornadoFrequency;
	}
	S->TornadoCheckInterval = Mode == 2 ? 60.f : SavedTornadoInterval;
	S->TornadoFrequency = Mode == 2 ? FMath::Max(SavedTornadoFrequency, 1.5f) : 0.f;
}

void AAerosChasePawn::SetRadarZoom(bool bIn)
{
	for (TActorIterator<AAerosWeatherRadar> It(GetWorld()); It; ++It)
	{
		if (bIn)
		{
			if (SavedRadarRange < 0.f) SavedRadarRange = It->RangeKm;
			It->RangeKm = RadarRangeKm;
		}
		else if (SavedRadarRange > 0.f)
		{
			It->RangeKm = SavedRadarRange;
		}
	}
	if (!bIn) SavedRadarRange = -1.f;
}

void AAerosChasePawn::RestoreWeather()
{
	// Back to a normal world for the menu and the technical demo
	TuneTornadoGenesis(0);
	SetRadarZoom(false);
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) Sub->SetConvectionControl(1.f, false);
}

void AAerosChasePawn::EndPlay(const EEndPlayReason::Type Reason)
{
	RestoreWeather();
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		Sub->OnLightningStrike.RemoveDynamic(this, &AAerosChasePawn::HandleLightning);
	}
	Super::EndPlay(Reason);
}

float AAerosChasePawn::GetHour() const
{
	const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	return Sub ? Sub->GetDateTime().Hour : 12.f;
}

int32 AAerosChasePawn::BestPhotoPoints() const
{
	int32 Best = 0;
	for (const FAerosChasePhoto& P : Album) Best = FMath::Max(Best, P.Points);
	return Best;
}

float AAerosChasePawn::BearingTo(const FVector& World) const
{
	// Unreal: X = east, -Y = north
	const FVector D = World - GetActorLocation();
	return FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(float(D.X), float(-D.Y))) + 360.f, 360.f);
}

double AAerosChasePawn::GroundZAt(double X, double Y) const
{
	for (TActorIterator<AAerosDemoTerrain> It(GetWorld()); It; ++It) return It->GetGroundZ(X, Y);
	return 0.0;
}

void AAerosChasePawn::PlaceOnGround()
{
	FVector L = GetActorLocation();
	double Z = GroundZAt(L.X, L.Y);
	if (const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		Z = FMath::Max(Z, double(Sub->GetSimulationSettings().SeaLevelZ));
	}
	L.Z = Z + AerosChasePawnInternal::CameraHeightCm;
	SetActorLocation(L);
}

void AAerosChasePawn::ShowToast(const FString& Text)
{
	Toast = Text;
	ToastTime = 4.f;
}

void AAerosChasePawn::ApplyCamera()
{
	SetActorRotation(FRotator(0.f, HeadingDeg, 0.f));
	Camera->SetRelativeRotation(FRotator(LookPitch, LookYaw, 0.f));
	Camera->SetFieldOfView(FMath::FInterpTo(Camera->FieldOfView, bZoom ? 22.f : 80.f, GetWorld()->GetDeltaSeconds(), 10.f));
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetControlRotation(Camera->GetComponentRotation());
	}
}

void AAerosChasePawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	SessionTime += DeltaSeconds;
	StateTime += DeltaSeconds;
	ToastTime = FMath::Max(0.f, ToastTime - DeltaSeconds);
	DamageFlash = FMath::Max(0.f, DamageFlash - DeltaSeconds * 1.5f);
	ShutterFlash = FMath::Max(0.f, ShutterFlash - DeltaSeconds * 4.f);
	LastPhotoTime += DeltaSeconds;
	if (PhotoHideFrames > 0)
	{
		--PhotoHideFrames;
		if (PhotoHideFrames == 1 && !PendingPhotoFile.IsEmpty()) CapturePhoto();
		if (PhotoHideFrames == 0) ShutterFlash = 1.f;
	}

	if (QuitAfter > 0.f && SessionTime > QuitAfter)
	{
		QuitAfter = -1.f;
		UKismetSystemLibrary::QuitGame(this, Cast<APlayerController>(GetController()), EQuitPreference::Quit, false);
		return;
	}
	if (EndDayAfter > 0.f && DrivingTime > EndDayAfter && State == EAerosChaseState::Driving)
	{
		EndDayAfter = -1.f;
		EndDay();
	}
	float ShotEvery = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("AerosChaseShots="), ShotEvery) && ShotEvery > 0.f && (AutoShotTimer += DeltaSeconds) >= ShotEvery)
	{
		AutoShotTimer = 0.f;
		static int32 ShotIndex = 0;
		FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / TEXT("AerosChase") / FString::Printf(TEXT("auto_%03d.png"), ShotIndex++), true, false);
	}

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC || !PC->IsLocalController()) return;
	switch (State)
	{
	case EAerosChaseState::Menu:
		TickMenu(PC);
		// Slow look around the sky behind the menu
		LookYaw = FMath::Fmod(LookYaw + DeltaSeconds * 2.f, 360.f);
		LookPitch = 8.f;
		break;
	case EAerosChaseState::Briefing:
		TickBriefing(DeltaSeconds);
		break;
	case EAerosChaseState::Driving:
		DrivingTime += DeltaSeconds;
		TickDriving(PC, DeltaSeconds);
		break;
	case EAerosChaseState::Destroyed:
		if (StateTime > 3.f) Respawn();
		break;
	case EAerosChaseState::Paused:
		if (AerosChasePawnInternal::KeyPressed(PC, { EKeys::Escape, EKeys::P }))
		{
			State = EAerosChaseState::Driving;
			StateTime = 0.f;
			if (UAerosWeatherSubsystem* PauseSub = UAerosWeatherSubsystem::Get(this)) PauseSub->SetPaused(false);
		}
		if (AerosChasePawnInternal::KeyPressed(PC, { EKeys::BackSpace })) GoToMenu();
		else if (AerosChasePawnInternal::KeyPressed(PC, { EKeys::Enter })) { if (UAerosWeatherSubsystem* EndSub = UAerosWeatherSubsystem::Get(this)) EndSub->SetPaused(false); EndReason = TEXT("Journ\u00E9e arr\u00EAt\u00E9e."); EndDay(); }
		break;
	case EAerosChaseState::Summary:
		TickSummary(PC);
		break;
	}
	ApplyCamera();
}

// ---------------------------------------------------------------- menu / summary

void AAerosChasePawn::TickMenu(APlayerController* PC)
{
	using namespace AerosChasePawnInternal;
	if (KeyPressed(PC, { EKeys::Up, EKeys::Z })) MenuIndex = (MenuIndex + 2) % 3;
	if (KeyPressed(PC, { EKeys::Down, EKeys::S })) MenuIndex = (MenuIndex + 1) % 3;
	const bool bTestEnter = MenuEnterAfter >= 0.f && StateTime > MenuEnterAfter;
	if (bTestEnter) MenuEnterAfter = -1.f;
	if (bTestEnter || KeyPressed(PC, { EKeys::Enter, EKeys::SpaceBar }))
	{
		if (MenuIndex == 0) StartChase();
		else if (MenuIndex == 1) GoToTechDemo();
		else UKismetSystemLibrary::QuitGame(this, PC, EQuitPreference::Quit, false);
	}
}

void AAerosChasePawn::TickSummary(APlayerController* PC)
{
	using namespace AerosChasePawnInternal;
	if (KeyPressed(PC, { EKeys::Up, EKeys::Down, EKeys::Z, EKeys::S })) SummaryIndex = 1 - SummaryIndex;
	if (KeyPressed(PC, { EKeys::Enter, EKeys::SpaceBar }))
	{
		if (SummaryIndex == 0) StartChase();
		else GoToMenu();
	}
	if (KeyPressed(PC, { EKeys::BackSpace })) GoToMenu();
}

void AAerosChasePawn::GoToMenu()
{
	State = EAerosChaseState::Menu;
	StateTime = 0.f;
	bZoom = false;
	RestoreWeather();
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) { Sub->SetPaused(false); Sub->SetTimeScale(60.f); }
}

void AAerosChasePawn::GoToTechDemo()
{
	if (AAerosDemoGameMode* GM = GetWorld()->GetAuthGameMode<AAerosDemoGameMode>())
	{
		GM->SwitchDemo(Cast<APlayerController>(GetController()), true);
	}
}

// ---------------------------------------------------------------- the day

void AAerosChasePawn::StartChase()
{
	Score = 0;
	Destroyed = 0;
	HailIntercepts = 0;
	Health = 100.f;
	SpeedMs = 0.f;
	Album.Reset();
	HailStorms.Reset();
	PhotoCount = 0;
	DrivingTime = 0.f;
	LastPhotoText.Empty();
	LookYaw = 0.f;
	LookPitch = 5.f;
	State = EAerosChaseState::Briefing;
	StateTime = 0.f;
	BriefingProgress = 0.f;
	BriefingSetup = 0;
	BriefingSetupTimer = 0.f;
	BubbleTries = 0;
	BubbleTimer = 0.f;
	EndReason.Empty();
	BriefingText = TEXT("Situation : air chaud et humide au sol, vent qui tourne avec l'altitude. Une supercellule va na\u00EEtre \u00E0 l'ouest. Rejoins-la, approche-toi par le c\u00F4t\u00E9 (hors de la pluie) et guette la tornade : elle n'est pas encore l\u00E0. La journ\u00E9e finit quand l'orage meurt, ou au coucher du soleil (20 h 30).");
}

void AAerosChasePawn::TickBriefing(float DeltaSeconds)
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	// A supercell outbreak: the storms grow fast (x600, x40-100 in practice: the simulation cannot
	// keep up) from 15:15 while the briefing is shown. Sent again until the clock shows it.
	if (BriefingSetup < 2 && (BriefingSetupTimer -= DeltaSeconds) <= 0.f)
	{
		const float Now = Sub->GetDateTime().Hour;
		// Only our own clock counts: from the menu the clock may already be close to 16:00, and the
		// day started 4 s after the storm's seed, before any storm had grown
		if (BriefingSetup == 1 && FMath::Abs(Now - BriefingStartHour) < 0.2f)
		{
			BriefingSetup = 2;
		}
		else
		{
			Sub->SetPaused(false);
			// One storm only: no spontaneous initiation, the running ones (and their tornadoes) removed
			Sub->SetScenario(EAerosScenario::SupercellOutbreak);
			Sub->SetConvectionControl(0.f, true);
			for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It) It->Destroy();
			TuneTornadoGenesis(1);
			Sub->SetDateTime(Sub->GetDateTime().DayOfYear, BriefingStartHour);
			Sub->SetTimeScale(600.f);
			BriefingSetup = 1;
			BriefingSetupTimer = 0.5f;
		}
	}
	if (BriefingSetup < 2) return;
	// The storm's seed: a warm bubble on land 15-25 km from the player where the air is the most
	// unstable, again (stronger) if nothing grew
	BubbleTimer += DeltaSeconds;
	if (BubbleTries == 0 || (Sub->GetStorms().Num() == 0 && BubbleTimer > 8.f && BubbleTries < 4))
	{
		if (BubbleTries == 0) BubbleAt = PickBubbleSpot(Sub);
		Sub->InjectThermalBubble(BubbleAt, 6.f + 1.f * BubbleTries, 3.5f + 0.5f * BubbleTries, 2.f);
		UE_LOG(LogTemp, Display, TEXT("Aeros chase: bubble %d at (%.0f, %.0f) km, CAPE %.0f"), BubbleTries + 1,
			BubbleAt.X * 1e-5, BubbleAt.Y * 1e-5, Sub->GetWeatherAtLocation(BubbleAt).CAPE);
		++BubbleTries;
		BubbleTimer = 0.f;
	}
	if ((BriefingLogTimer += DeltaSeconds) > 5.f)
	{
		BriefingLogTimer = 0.f;
		float Meso = 0.f;
		for (const FAerosStormData& S : Sub->GetStorms()) Meso = FMath::Max(Meso, S.Mesocyclone);
		UE_LOG(LogTemp, Display, TEXT("Aeros chase: briefing %.2f h, %d storms, mesocyclone %.2f"), Sub->GetDateTime().Hour, Sub->GetStorms().Num(), Meso);
	}
	const float Hour = Sub->GetDateTime().Hour;
	BriefingProgress = FMath::Clamp((Hour - BriefingStartHour) / (DayStartHour - BriefingStartHour), 0.f, 1.f);
	bBriefingReady = false;
	// Ready = a mature supercell: rotating, its anvil spread at the tropopause, an hour old at least
	for (const FAerosStormData& S : Sub->GetStorms())
	{
		bBriefingReady |= S.Type == EAerosStormType::Supercell && S.Mesocyclone > 0.5f && S.CloudTop > 10000.f && Hour >= BriefingStartHour + 1.f;
	}
	APlayerController* PC = Cast<APlayerController>(GetController());
	const bool bSkip = bBriefingReady && StateTime > 2.f && PC
		&& AerosChasePawnInternal::KeyPressed(PC, { EKeys::Enter, EKeys::SpaceBar, EKeys::LeftMouseButton });
	const bool bAutoSkip = bAutopilot && bBriefingReady && StateTime > 3.f;
	// The day starts once the supercell rotates, not at a fixed hour: with one storm the simulation
	// really runs x600, 15:15 -> 16:00 took 4 s and the day began on a newborn cumulus that needed
	// another 15 real minutes at x2. The clock just keeps running; after 2 minutes it starts anyway.
	const bool bReadyAtFour = bBriefingReady && Hour >= DayStartHour - 0.02f && StateTime > 4.f;
	if (bSkip || bAutoSkip || bReadyAtFour || StateTime > 120.f) BeginDay();
}

FVector AAerosChasePawn::PickBubbleSpot(UAerosWeatherSubsystem* Sub) const
{
	// The player may stand anywhere (the technical demo hands over its position: at sea, by the
	// volcano...): try points 8-16 km around, keep low plains (on the mountains, 20 km west, the cloud
	// base was under the ground and nothing could be seen), the most unstable air, west first (the
	// storm moves north-east and the coast is east)
	const FVector L = GetActorLocation();
	const double SeaZ = Sub->GetSimulationSettings().SeaLevelZ;
	FVector Best = L + FVector(-1000000.0, 300000.0, 0.0);
	float BestScore = -1e9f;
	for (int32 K = 0; K < 24; ++K)
	{
		const float A = 2.f * PI * (K % 12) / 12.f;
		const double R = K < 12 ? 800000.0 : 1600000.0;
		const FVector P = L + FVector(FMath::Cos(A), FMath::Sin(A), 0.0) * R;
		const double Z = GroundZAt(P.X, P.Y);
		if (Z < SeaZ + 500.0 || Z > SeaZ + 30000.0) continue;
		const float Rank = Sub->GetWeatherAtLocation(P).CAPE - (P.X > L.X ? 1500.f : 0.f);
		if (Rank > BestScore) { BestScore = Rank; Best = P; }
	}
	return FVector(Best.X, Best.Y, 0.0);
}

void AAerosChasePawn::BeginDay()
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	Sub->SetTimeScale(DayTimeScale);
	TuneTornadoGenesis(1);   // no tornado for the first 45 s: the player gets to see the first one come down
	bTornadoesOn = false;
	SetRadarZoom(true);
	NoStormTime = 0.f;
	bStormSeen = false;
	// Start 20 km from the strongest storm (its echo spreads 20 km and more), on its right flank, dry
	SetActorLocation(FindSafeStart(Sub, 20.f));
	PlaceOnGround();
	float BestI = -1.f;
	FVector Target = GetActorLocation() + FVector(100000.0, 0.0, 0.0);
	for (const FAerosStormData& S : Sub->GetStorms())
	{
		if (S.Intensity > BestI) { BestI = S.Intensity; Target = S.Location; }
	}
	HeadingDeg = (Target - GetActorLocation()).Rotation().Yaw;
	State = EAerosChaseState::Driving;
	StateTime = 0.f;
	UE_LOG(LogTemp, Display, TEXT("Aeros chase: day starts at %.2f h with %d storms"), Sub->GetDateTime().Hour, Sub->GetStorms().Num());
	ShowToast(TEXT("C'est parti ! Le radar en haut \u00E0 droite montre la pluie, les losanges les orages."));
}

FVector AAerosChasePawn::FindSafeStart(UAerosWeatherSubsystem* Sub, float DistanceKm, const FVector* Around) const
{
	FVector Center = GetActorLocation();
	float BestI = -1.f;
	for (const FAerosStormData& S : Sub->GetStorms())
	{
		if (S.Intensity > BestI) { BestI = S.Intensity; Center = S.Location; }
	}
	if (Around) Center = *Around;
	const double SeaZ = Sub->GetSimulationSettings().SeaLevelZ;
	// Try directions around the storm, its right flank first, keep the first point on dry land out of
	// the rain
	const float A0 = FlankAngle(Sub);
	for (int32 K = 0; K < 16; ++K)
	{
		const float A = A0 + (K % 2 == 0 ? 1.f : -1.f) * float((K + 1) / 2) * (PI / 8.f);
		for (float D = DistanceKm; D <= DistanceKm * 2.f; D += FMath::Max(1.f, DistanceKm / 6.f))
		{
			const FVector P = Center + FVector(FMath::Cos(A), FMath::Sin(A), 0.0) * double(D) * 100000.0;
			const double GZ = GroundZAt(P.X, P.Y);
			if (GZ < SeaZ + 300.0 || GZ > SeaZ + 60000.0) continue;   // dry land, no mountains
			// Dry here and 5 km around: a supercell's rain reaches 20 km and more from its centre
			bool bDry = true;
			for (int32 R = 0; R < 5 && bDry; ++R)
			{
				const FVector Q = R == 0 ? P : P + FVector(FMath::Cos(R * HALF_PI), FMath::Sin(R * HALF_PI), 0.0) * 500000.0;
				const FAerosWeatherSample W = Sub->GetWeatherAtLocation(Q);
				bDry = W.PrecipitationRate <= 0.5f && W.HailDiameter <= 0.f && W.Visibility >= 10000.f;
			}
			if (!bDry) continue;
			return FVector(P.X, P.Y, 0.0);
		}
	}
	return FVector(Center.X - DistanceKm * 100000.0, Center.Y, 0.0);
}

float AAerosChasePawn::FlankAngle(UAerosWeatherSubsystem* Sub) const
{
	const FAerosStormData* Best = nullptr;
	for (const FAerosStormData& S : Sub->GetStorms())
	{
		if (!Best || S.Intensity > Best->Intensity) Best = &S;
	}
	if (!Best || Best->Velocity.Size2D() < 1.0) return PI;   // west (the coast is east)
	const FVector M = Best->Velocity.GetSafeNormal2D();
	const FVector Right(-M.Y, M.X, 0.0);                      // as UAerosStormManager::SpawnTornado
	const FVector D = (Right - 0.3 * M).GetSafeNormal2D();
	return FMath::Atan2(float(D.Y), float(D.X));
}

void AAerosChasePawn::EndDay()
{
	State = EAerosChaseState::Summary;
	StateTime = 0.f;
	TuneTornadoGenesis(0);
	SetRadarZoom(false);
	UE_LOG(LogTemp, Display, TEXT("Aeros chase: day over, score %d, %d photos, destroyed %d"), Score, Album.Num(), Destroyed);
	SummaryIndex = 0;
	bZoom = false;
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) Sub->SetTimeScale(60.f);
}

void AAerosChasePawn::Respawn()
{
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		SetActorLocation(FindSafeStart(Sub, 8.f));
	}
	PlaceOnGround();
	Health = 100.f;
	SpeedMs = 0.f;
	State = EAerosChaseState::Driving;
	StateTime = 0.f;
	ShowToast(TEXT("Nouveau v\u00E9hicule. Prudence pr\u00E8s des tornades !"));
}

// ---------------------------------------------------------------- driving

void AAerosChasePawn::TickDriving(APlayerController* PC, float DeltaSeconds)
{
	using namespace AerosChasePawnInternal;
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	if (Sub->GetDateTime().Hour >= DayEndHour) { EndReason = TEXT("Le soleil s'est couch\u00E9."); EndDay(); return; }
	if (!bTornadoesOn && DrivingTime > 45.f) { TuneTornadoGenesis(2); bTornadoesOn = true; }
	// The day is the storm's life: it ends once the storm has died
	if (Sub->GetStorms().Num() > 0) { bStormSeen = true; NoStormTime = 0.f; }
	else NoStormTime += DeltaSeconds;
	if (bStormSeen && NoStormTime > 15.f) { EndReason = TEXT("L'orage s'est dissip\u00E9."); EndDay(); return; }
	if (KeyPressed(PC, { EKeys::Escape, EKeys::P })) { State = EAerosChaseState::Paused; StateTime = 0.f; Sub->SetPaused(true); return; }
	if (KeyPressed(PC, { EKeys::BackSpace })) { GoToMenu(); return; }

	if (bAutopilot) TickAutopilot(DeltaSeconds);
	else
	{
		Drive(PC, DeltaSeconds);
		Look(PC, DeltaSeconds);
		bZoom = PC->IsInputKeyDown(EKeys::RightMouseButton);
		if (KeyPressed(PC, { EKeys::LeftMouseButton, EKeys::SpaceBar })) TakePhoto();
		if (KeyPressed(PC, { EKeys::T })) FastTravel();
	}
	ApplyHazards(Sub, DeltaSeconds);
	Preview = EvaluateShot();
	UpdateAlerts(Sub);
}

void AAerosChasePawn::Drive(APlayerController* PC, float DeltaSeconds)
{
	using namespace AerosChasePawnInternal;
	const float Throttle = (KeyDown(PC, { EKeys::Z, EKeys::Up }) ? 1.f : 0.f) - (KeyDown(PC, { EKeys::S, EKeys::Down }) ? 1.f : 0.f);
	const float Steer = (KeyDown(PC, { EKeys::D, EKeys::Right }) ? 1.f : 0.f) - (KeyDown(PC, { EKeys::Q, EKeys::Left }) ? 1.f : 0.f);
	if (Throttle > 0.f) SpeedMs += (SpeedMs < 0.f ? 20.f : 14.f) * DeltaSeconds;
	else if (Throttle < 0.f) SpeedMs -= (SpeedMs > 0.f ? 25.f : 4.f) * DeltaSeconds;
	else SpeedMs = FMath::FInterpTo(SpeedMs, 0.f, DeltaSeconds, 0.4f);
	const float Speed01 = FMath::Abs(SpeedMs) / MaxSpeedMs;
	HeadingDeg += Steer * FMath::Lerp(75.f, 35.f, Speed01) * DeltaSeconds * (SpeedMs >= 0.f ? 1.f : -1.f) * FMath::Min(FMath::Abs(SpeedMs) / 3.f, 1.f);
}

void AAerosChasePawn::Look(APlayerController* PC, float DeltaSeconds)
{
	float DX = 0.f, DY = 0.f;
	PC->GetInputMouseDelta(DX, DY);
	const float Sens = bZoom ? 0.04f : 0.12f;
	LookYaw = FMath::Clamp(LookYaw + DX * Sens * 10.f, -170.f, 170.f);
	LookPitch = FMath::Clamp(LookPitch + DY * Sens * 10.f, -30.f, 70.f);
}

void AAerosChasePawn::ApplyHazards(UAerosWeatherSubsystem* Sub, float DeltaSeconds)
{
	using namespace AerosChasePawnInternal;
	const FVector L = GetActorLocation();
	const FAerosWeatherSample W = Sub->GetWeatherAtLocation(L);

	// Speed limits: flooded roads, the sea
	float Limit = MaxSpeedMs;
	if (W.FloodDepth > 0.3f) Limit *= 0.35f;
	if (W.HailDiameter > 20.f || W.Visibility < 500.f) Limit *= 0.6f;
	SpeedMs = FMath::Clamp(SpeedMs, -8.f, Limit);
	FVector Move = FRotator(0.f, HeadingDeg, 0.f).Vector() * double(SpeedMs) * 100.0 * DeltaSeconds;

	// Tornado winds push the vehicle and hurt it
	float TornadoWind = 0.f;
	FVector Push = FVector::ZeroVector;
	for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It)
	{
		const FVector V = It->GetWindAt(L);
		TornadoWind = FMath::Max(TornadoWind, float(V.Size2D()));
		Push += FVector(V.X, V.Y, 0.0);
	}
	Move += Push * 0.25 * 100.0 * DeltaSeconds;

	const FVector Next = L + Move;
	const double SeaZ = Sub->GetSimulationSettings().SeaLevelZ;
	if (GroundZAt(Next.X, Next.Y) < SeaZ - 50.0)
	{
		SpeedMs = 0.f;
		if (ToastTime <= 0.f) ShowToast(TEXT("Eau profonde : demi-tour."));
	}
	else
	{
		SetActorLocation(Next);
	}
	PlaceOnGround();

	// Damage
	const float Dps = AerosChase::DamagePerSecond(TornadoWind, W.HailDiameter, W.FloodDepth);
	if (Dps > 0.f)
	{
		Health -= Dps * DeltaSeconds;
		DamageFlash = FMath::Min(1.f, DamageFlash + Dps * DeltaSeconds * 0.1f);
	}
	else if (Health < 100.f)
	{
		Health = FMath::Min(100.f, Health + 0.5f * DeltaSeconds);   // slow field repairs out of danger
	}

	// Hail core intercept: points once per storm (2 s inside hail >= 20 mm)
	if (W.HailDiameter >= 20.f)
	{
		HailTime += DeltaSeconds;
		int32 Nearest = -1;
		double BestD = 1e18;
		for (const FAerosStormData& S : Sub->GetStorms())
		{
			const double D = FVector::Dist2D(S.Location, L);
			if (D < BestD) { BestD = D; Nearest = S.Id; }
		}
		if (HailTime > 2.f && Nearest >= 0 && !HailStorms.Contains(Nearest))
		{
			HailStorms.Add(Nearest);
			const int32 P = AerosChase::HailPoints(W.HailDiameter);
			Score += P;
			++HailIntercepts;
			LastPhotoText = FString::Printf(TEXT("+%d pts - C\u0153ur de gr\u00EAle travers\u00E9 (%.0f mm)"), P, W.HailDiameter);
			LastPhotoTime = 0.f;
		}
	}
	else
	{
		HailTime = 0.f;
	}

	if (Health <= 0.f)
	{
		Health = 0.f;
		++Destroyed;
		LastPenalty = AerosChase::RespawnPenalty(Score);
		Score -= LastPenalty;
		State = EAerosChaseState::Destroyed;
		StateTime = 0.f;
		SpeedMs = 0.f;
	}
}

void AAerosChasePawn::HandleLightning(const FAerosLightningStrike& Strike)
{
	if (!Strike.bCloudToGround) return;
	LastStrikeTime = GetWorld()->GetTimeSeconds();
	LastStrikeLocation = Strike.Location;
	LastStrikeBrightness = FMath::Clamp(Strike.PeakCurrent / 40.f, 0.4f, 1.f);
	if (State != EAerosChaseState::Driving) return;
	const float DistM = float(FVector::Dist(Strike.Location, GetActorLocation())) * 0.01f;
	const float Hit = AerosChase::LightningDamage(DistM);
	if (Hit > 0.f)
	{
		Health -= Hit;
		DamageFlash = 1.f;
		ShowToast(TEXT("La foudre est tomb\u00E9e tout pr\u00E8s !"));
	}
}

void AAerosChasePawn::UpdateAlerts(UAerosWeatherSubsystem* Sub)
{
	using namespace AerosChasePawnInternal;
	Alerts.Reset();
	const FVector L = GetActorLocation();
	float NearestT = 1e9f;
	int32 NearestEF = 0;
	for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It)
	{
		if (It->GetStrength() < 0.1f) continue;
		const float D = float(FVector::Dist2D(It->GetActorLocation(), L)) * 1e-5f;
		if (D < NearestT) { NearestT = D; NearestEF = It->GetEFRating(); }
	}
	if (NearestT < 1.5f) Alerts.Add(FString::Printf(TEXT("DANGER : tornade EF%d \u00E0 %s !"), NearestEF, *Km(NearestT)));
	else if (NearestT < 8.f) Alerts.Add(FString::Printf(TEXT("Tornade EF%d \u00E0 %s"), NearestEF, *Km(NearestT)));
	const FAerosWeatherSample W = Sub->GetWeatherAtLocation(L);
	if (W.HailDiameter > 20.f) Alerts.Add(FString::Printf(TEXT("Gr\u00EAle %.0f mm : le v\u00E9hicule est touch\u00E9"), W.HailDiameter));
	if (W.FloodDepth > 0.3f) Alerts.Add(FString::Printf(TEXT("Route inond\u00E9e (%.1f m)"), W.FloodDepth));
	if (W.Visibility < 1500.f) Alerts.Add(TEXT("Visibilit\u00E9 tr\u00E8s faible"));
}

void AAerosChasePawn::FastTravel()
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	const FVector L = GetActorLocation();
	const FAerosStormData* Best = nullptr;
	double BestD = 1e18;
	TArray<FAerosStormData> Storms = Sub->GetStorms();
	for (const FAerosStormData& S : Storms)
	{
		if (S.Type != EAerosStormType::Supercell) continue;
		const double D = FVector::Dist2D(S.Location, L);
		if (D < BestD) { BestD = D; Best = &S; }
	}
	// A tornado on the ground beats any supercell
	const AAerosTornado* Twister = nullptr;
	double TwisterD = 1e18;
	for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It)
	{
		if (It->GetStrength() < 0.15f) continue;
		const double D = FVector::Dist2D(It->GetActorLocation(), L);
		if (D < TwisterD) { TwisterD = D; Twister = *It; }
	}
	if (!Best)
	{
		for (const FAerosStormData& S : Storms)
		{
			const double D = FVector::Dist2D(S.Location, L);
			if (D < BestD) { BestD = D; Best = &S; }
		}
	}
	if (!Best && !Twister) { ShowToast(TEXT("Aucun orage en vue pour l'instant.")); return; }
	const bool bTwister = Twister != nullptr;
	const double TargetD = bTwister ? TwisterD : BestD;
	const TCHAR* What = bTwister ? TEXT("de la tornade") : TEXT("de la supercellule");
	if (TargetD < (FastTravelKm + 2.f) * 100000.0) { ShowToast(FString::Printf(TEXT("Tu es d\u00E9j\u00E0 pr\u00E8s %s."), What)); return; }
	const FVector Center = bTwister ? Twister->GetActorLocation() : Best->Location;
	SetActorLocation(FindSafeStart(Sub, FastTravelKm, &Center));
	PlaceOnGround();
	HeadingDeg = (Center - GetActorLocation()).Rotation().Yaw;
	LookYaw = 0.f;
	SpeedMs = 0.f;
	const FAerosDateTime Now = Sub->GetDateTime();
	Sub->SetDateTime(Now.DayOfYear, Now.Hour + FastTravelHours);
	ShowToast(FString::Printf(TEXT("Trajet rapide : 10 minutes plus tard, \u00E0 4 km %s (hors de la pluie)."), What));
}

// ---------------------------------------------------------------- photos

FAerosChaseShotPreview AAerosChasePawn::EvaluateShot() const
{
	using namespace AerosChasePawnInternal;
	FAerosChaseShotPreview Best;
	const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return Best;
	const FVector Eye = Camera->GetComponentLocation();
	const FVector Fwd = Camera->GetForwardVector();
	const float HalfFov = FMath::DegreesToRadians(Camera->FieldOfView * 0.5f);
	// Rain between the camera and the subject hides it: the visibility is the worst one met along
	// the line of sight (the subject's own spot is left out: a rain-wrapped tornado is still "seen"
	// through the last stretch, the sample at 75 % already catches a heavy veil).
	auto SightVisKm = [&](const FVector& Target)
	{
		float V = 1e9f;
		for (const float T : { 0.f, 0.2f, 0.4f, 0.6f, 0.75f })
		{
			FVector S = FMath::Lerp(Eye, Target, double(T));
			S.Z = Eye.Z;
			V = FMath::Min(V, Sub->GetWeatherAtLocation(S).Visibility * 0.001f);
		}
		return V;
	};
	// Hills and dunes in the way: the subject must be seen at its top or at its base
	FCollisionQueryParams Q(SCENE_QUERY_STAT(AerosChaseSight), false, this);
	const FCollisionObjectQueryParams Static(ECC_WorldStatic);
	auto InSight = [&](const FVector& Target, double BaseDropCm)
	{
		FHitResult Hit;
		if (!GetWorld()->LineTraceSingleByObjectType(Hit, Eye, Target, Static, Q)) return true;
		if (BaseDropCm <= 0.0) return false;
		const FVector Base = Target - FVector(0, 0, BaseDropCm);
		return !GetWorld()->LineTraceSingleByObjectType(Hit, Eye, Base, Static, Q);
	};

	auto Consider = [&](AerosChase::ESubject Subject, const FVector& Target, int32 EF, float Brightness, const FString& Label, const FString& Key, double BaseDropCm)
	{
		const FVector To = Target - Eye;
		const float DistKm = float(To.Size()) * 1e-5f;
		const float Angle = FMath::Acos(FMath::Clamp(float(FVector::DotProduct(To.GetSafeNormal(), Fwd)), -1.f, 1.f));
		AerosChase::FPhotoInput I;
		I.Subject = Subject;
		I.EF = EF;
		I.DistanceKm = DistKm;
		I.CenterOffset = Angle / FMath::Max(HalfFov, 0.01f);
		I.VisibilityKm = 1e9f;
		I.Brightness = Brightness;
		if (AerosChase::PhotoPoints(I) <= Best.Points) return;   // cheap test first: framing and range
		if (!InSight(Target, BaseDropCm))
		{
			if (Best.Points == 0) Best.Label = FString::Printf(TEXT("%s cach\u00E9 par le relief"), *Label);
			return;
		}
		I.VisibilityKm = SightVisKm(Target);
		const int32 Points = AerosChase::PhotoPoints(I);
		if (Points <= 0 && Best.Points == 0 && I.CenterOffset < 1.f) Best.Label = FString::Printf(TEXT("%s noy\u00E9 dans la pluie"), *Label);
		if (Points > Best.Points)
		{
			Best.Points = Points;
			Best.Subject = Subject;
			Best.Label = FString::Printf(TEXT("%s \u00E0 %s"), *Label, *Km(DistKm));
			Best.Key = Key;
		}
	};

	for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It)
	{
		if (It->GetStrength() < 0.15f) continue;   // no condensed funnel yet / any more
		Consider(AerosChase::ESubject::Tornado, It->GetActorLocation() + FVector(0, 0, 30000.0), It->GetEFRating(), 1.f,
			FString::Printf(TEXT("Tornade EF%d"), It->GetEFRating()), FString::Printf(TEXT("T%s"), *It->GetName()), 28000.0);
	}
	for (const FAerosStormData& S : Sub->GetStorms())
	{
		if (S.Mesocyclone > 0.3f)
		{
			Consider(AerosChase::ESubject::WallCloud, S.UpdraftLocation + FVector(0, 0, FMath::Max(S.LCL, 300.f) * 100.0), 0, 1.f,
				TEXT("Nuage-mur"), FString::Printf(TEXT("W%d"), S.Id), 0.0);
		}
		if (S.Type == EAerosStormType::Supercell)
		{
			Consider(AerosChase::ESubject::Supercell, S.Location + FVector(0, 0, FMath::Max(S.CloudTop, 4000.f) * 50.0), 0, 1.f,
				TEXT("Supercellule"), FString::Printf(TEXT("S%d"), S.Id), 0.0);
		}
	}
	const float SinceStrike = float(GetWorld()->GetTimeSeconds() - LastStrikeTime);
	if (SinceStrike < 0.6f)
	{
		Consider(AerosChase::ESubject::Lightning, LastStrikeLocation + FVector(0, 0, 200000.0), 0, LastStrikeBrightness * (1.f - SinceStrike / 0.6f),
			TEXT("\u00C9clair"), FString::Printf(TEXT("L%d"), PhotoCount), 0.0);
	}
	return Best;
}

void AAerosChasePawn::CapturePhoto()
{
	// Called one frame after the HUD hid itself: the picture is clean, then the shutter flash shows
	FScreenshotRequest::RequestScreenshot(PendingPhotoFile, false, false);
	PendingPhotoFile.Empty();
}

void AAerosChasePawn::TakePhoto()
{
	++PhotoCount;
	const FAerosChaseShotPreview Shot = EvaluateShot();
	const FString File = FPaths::ScreenShotDir() / TEXT("AerosChase") / FString::Printf(TEXT("photo_%03d.png"), PhotoCount);
	PendingPhotoFile = File;
	PhotoHideFrames = 3;
	if (Shot.Points <= 0)
	{
		LastPhotoText = Shot.Label.IsEmpty() ? FString(TEXT("Rien d'int\u00E9ressant dans le cadre")) : Shot.Label;
		LastPhotoTime = 0.f;
		return;
	}
	// The best photo of each subject counts: a better one only adds the difference
	FAerosChasePhoto* Existing = Album.FindByPredicate([&Shot](const FAerosChasePhoto& P) { return P.Key == Shot.Key; });
	int32 Gain = Shot.Points;
	if (Existing)
	{
		Gain = FMath::Max(0, Shot.Points - Existing->Points);
		if (Shot.Points > Existing->Points) { Existing->Points = Shot.Points; Existing->Label = Shot.Label; Existing->File = File; Existing->Hour = GetHour(); }
	}
	else
	{
		FAerosChasePhoto P;
		P.Label = Shot.Label;
		P.Key = Shot.Key;
		P.Points = Shot.Points;
		P.Hour = GetHour();
		P.File = File;
		Album.Add(P);
	}
	Score += Gain;
	LastPhotoText = Gain > 0 ? FString::Printf(TEXT("+%d pts - %s"), Gain, *Shot.Label)
		: FString::Printf(TEXT("D\u00E9j\u00E0 mieux photographi\u00E9 (%d pts) - %s"), Existing ? Existing->Points : 0, *Shot.Label);
	LastPhotoTime = 0.f;
}

// ---------------------------------------------------------------- autopilot (tests)

void AAerosChasePawn::TickAutopilot(float DeltaSeconds)
{
	const FVector L = GetActorLocation();
	FVector Target = FVector::ZeroVector;
	float Want = 12.f;   // a supercell is worth most from 10-20 km
	bool bHave = false;
	float BestD = 1e9f;
	for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It)
	{
		if (It->GetStrength() < 0.15f) continue;
		const float D = float(FVector::Dist2D(It->GetActorLocation(), L)) * 1e-5f;
		if (D < BestD && D < 40.f) { BestD = D; Target = It->GetActorLocation() + FVector(0, 0, 30000.0); Want = AutoTornadoKm; bHave = true; }
	}
	if (!bHave)
	{
		const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
		float BestI = -1.f;
		for (const FAerosStormData& S : Sub->GetStorms())
		{
			if (S.Intensity > BestI) { BestI = S.Intensity; Target = S.Location + FVector(0, 0, FMath::Max(S.CloudTop, 4000.f) * 50.0); bHave = true; }
		}
		BestD = float(FVector::Dist2D(Target, L)) * 1e-5f;
	}
	if (!bHave) return;
	AutoTravelTimer += DeltaSeconds;
	if (BestD > Want + 8.f && AutoTravelTimer > 20.f)
	{
		AutoTravelTimer = 0.f;
		FastTravel();
		return;
	}
	if ((AutoLogTimer += DeltaSeconds) > 10.f)
	{
		AutoLogTimer = 0.f;
		UE_LOG(LogTemp, Display, TEXT("Aeros chase: auto target %.1f km (want %.0f), speed %.0f km/h, score %d"), BestD, Want, SpeedMs * 3.6f, Score);
	}
	const float ToYaw = (Target - L).Rotation().Yaw;
	const float DriveYaw = BestD > Want + 1.f ? ToYaw : (BestD < Want - 1.f ? ToYaw + 180.f : ToYaw + 90.f);
	HeadingDeg = FMath::FixedTurn(HeadingDeg, DriveYaw, 40.f * DeltaSeconds);
	SpeedMs = FMath::FInterpTo(SpeedMs, FMath::Abs(BestD - Want) > 1.f ? (BestD > Want + 5.f ? 80.f : 35.f) : 6.f, DeltaSeconds, 1.f);
	// Aim the camera at the target
	const FRotator Aim = (Target - Camera->GetComponentLocation()).Rotation();
	LookYaw = FMath::Clamp(FRotator::NormalizeAxis(Aim.Yaw - HeadingDeg), -170.f, 170.f);
	LookPitch = FMath::Clamp(Aim.Pitch, -30.f, 70.f);
	bZoom = BestD > 6.f;
	if ((AutoPhotoTimer += DeltaSeconds) > 6.f)
	{
		AutoPhotoTimer = 0.f;
		TakePhoto();
	}
}
