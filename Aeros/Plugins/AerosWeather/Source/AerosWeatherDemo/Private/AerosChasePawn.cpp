// Copyright Aeros. All Rights Reserved.

#include "AerosChasePawn.h"
#include "AerosChaseHUD.h"
#include "AerosDemoGameMode.h"
#include "AerosDemoTerrain.h"
#include "AerosWeatherSubsystem.h"
#include "AerosPrecipitationComponent.h"
#include "AerosWaterSurfaceComponent.h"
#include "AerosTornado.h"
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

	constexpr float MaxSpeedMs = 38.f;   // ~137 km/h
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
	// -AerosChaseEndAfter=S ends the day after S s of driving (summary screen)
	bAutopilot = FParse::Param(FCommandLine::Get(), TEXT("AerosChaseAuto"));
	FParse::Value(FCommandLine::Get(), TEXT("AerosChaseQuitAfter="), QuitAfter);
	FParse::Value(FCommandLine::Get(), TEXT("AerosChaseEndAfter="), EndDayAfter);
	if (FParse::Param(FCommandLine::Get(), TEXT("AerosChase")) || bAutopilot) StartChase();
}

void AAerosChasePawn::EndPlay(const EEndPlayReason::Type Reason)
{
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
	if (KeyPressed(PC, { EKeys::Enter, EKeys::SpaceBar }))
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
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		// A supercell outbreak: the storms grow fast (x600, about x100 in practice: the simulation
		// cannot keep up) from 12:15 while the briefing is shown
		Sub->SetPaused(false);
		Sub->SetScenario(EAerosScenario::SupercellOutbreak);
		const FAerosDateTime Now = Sub->GetDateTime();
		Sub->SetDateTime(Now.DayOfYear, BriefingStartHour);
		Sub->SetTimeScale(600.f);
	}
	BriefingText = TEXT("Situation : air chaud et humide au sol, vent qui tourne avec l'altitude. Des supercellules vont se former cet apr\u00E8s-midi. Trouve-les au radar, approche-toi par le c\u00F4t\u00E9 (hors de la pluie) et photographie ce que le ciel te donne. Coucher du soleil vers 20 h 30.");
}

void AAerosChasePawn::TickBriefing(float DeltaSeconds)
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	const float Hour = Sub->GetDateTime().Hour;
	BriefingProgress = FMath::Clamp((Hour - BriefingStartHour) / (DayStartHour - BriefingStartHour), 0.f, 1.f);
	bBriefingReady = false;
	for (const FAerosStormData& S : Sub->GetStorms()) bBriefingReady |= S.Type == EAerosStormType::Supercell;
	APlayerController* PC = Cast<APlayerController>(GetController());
	const bool bSkip = bBriefingReady && StateTime > 2.f && PC
		&& AerosChasePawnInternal::KeyPressed(PC, { EKeys::Enter, EKeys::SpaceBar, EKeys::LeftMouseButton });
	if (bSkip || (Hour >= DayStartHour - 0.02f && StateTime > 4.f)) BeginDay();
}

void AAerosChasePawn::BeginDay()
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	Sub->SetTimeScale(DayTimeScale);
	// Start 30 km from the strongest storm, on land, facing it
	SetActorLocation(FindSafeStart(Sub, 30.f));
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
	// Try directions around the storm (west first: the ocean is east), keep the first point on dry land
	// outside heavy rain
	for (int32 K = 0; K < 16; ++K)
	{
		const float A = PI + (K % 2 == 0 ? 1.f : -1.f) * float((K + 1) / 2) * (PI / 8.f);
		for (float D = DistanceKm; D <= DistanceKm * 2.f; D += 5.f)
		{
			const FVector P = Center + FVector(FMath::Cos(A), FMath::Sin(A), 0.0) * double(D) * 100000.0;
			if (GroundZAt(P.X, P.Y) < SeaZ + 300.0) continue;
			const FAerosWeatherSample W = Sub->GetWeatherAtLocation(P);
			if (W.PrecipitationRate > 10.f || W.HailDiameter > 5.f) continue;
			return FVector(P.X, P.Y, 0.0);
		}
	}
	return FVector(Center.X - DistanceKm * 100000.0, Center.Y, 0.0);
}

void AAerosChasePawn::EndDay()
{
	State = EAerosChaseState::Summary;
	StateTime = 0.f;
	UE_LOG(LogTemp, Display, TEXT("Aeros chase: day over, score %d, %d photos, destroyed %d"), Score, Album.Num(), Destroyed);
	SummaryIndex = 0;
	bZoom = false;
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) Sub->SetTimeScale(60.f);
}

void AAerosChasePawn::Respawn()
{
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this))
	{
		SetActorLocation(FindSafeStart(Sub, 15.f));
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
	if (Sub->GetDateTime().Hour >= DayEndHour) { EndDay(); return; }
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
	if (Throttle > 0.f) SpeedMs += (SpeedMs < 0.f ? 12.f : 5.f) * DeltaSeconds;
	else if (Throttle < 0.f) SpeedMs -= (SpeedMs > 0.f ? 12.f : 3.f) * DeltaSeconds;
	else SpeedMs = FMath::FInterpTo(SpeedMs, 0.f, DeltaSeconds, 0.4f);
	const float Speed01 = FMath::Abs(SpeedMs) / MaxSpeedMs;
	HeadingDeg += Steer * FMath::Lerp(70.f, 25.f, Speed01) * DeltaSeconds * (SpeedMs >= 0.f ? 1.f : -1.f) * FMath::Min(FMath::Abs(SpeedMs) / 3.f, 1.f);
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
	if (!Best) { ShowToast(TEXT("Aucune supercellule en vue pour l'instant.")); return; }
	if (BestD < 1500000.0) { ShowToast(TEXT("Tu es d\u00E9j\u00E0 pr\u00E8s de la supercellule.")); return; }
	const FVector Center = Best->Location;
	SetActorLocation(FindSafeStart(Sub, 10.f, &Center));
	PlaceOnGround();
	HeadingDeg = (Center - GetActorLocation()).Rotation().Yaw;
	LookYaw = 0.f;
	SpeedMs = 0.f;
	const FAerosDateTime Now = Sub->GetDateTime();
	Sub->SetDateTime(Now.DayOfYear, Now.Hour + FastTravelHours);
	ShowToast(TEXT("Trajet rapide : 20 minutes plus tard, \u00E0 10 km de la supercellule."));
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
	float Want = 25.f;
	bool bHave = false;
	float BestD = 1e9f;
	for (TActorIterator<AAerosTornado> It(GetWorld()); It; ++It)
	{
		if (It->GetStrength() < 0.15f) continue;
		const float D = float(FVector::Dist2D(It->GetActorLocation(), L)) * 1e-5f;
		if (D < BestD) { BestD = D; Target = It->GetActorLocation() + FVector(0, 0, 30000.0); Want = 3.f; bHave = true; }
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
	if (BestD > 25.f && StateTime > 5.f && (AutoPhotoTimer > 5.f))
	{
		FastTravel();
		return;
	}
	const float ToYaw = (Target - L).Rotation().Yaw;
	const float DriveYaw = BestD > Want + 1.f ? ToYaw : (BestD < Want - 1.f ? ToYaw + 180.f : ToYaw + 90.f);
	HeadingDeg = FMath::FixedTurn(HeadingDeg, DriveYaw, 40.f * DeltaSeconds);
	SpeedMs = FMath::FInterpTo(SpeedMs, FMath::Abs(BestD - Want) > 1.f ? 30.f : 6.f, DeltaSeconds, 1.f);
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
