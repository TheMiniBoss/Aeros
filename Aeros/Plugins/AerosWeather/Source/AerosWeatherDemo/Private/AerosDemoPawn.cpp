// Copyright Aeros. All Rights Reserved.

#include "AerosDemoPawn.h"
#include "AerosDemoHUD.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherRadar.h"
#include "AerosWeatherLibrary.h"
#include "AerosPrecipitationComponent.h"
#include "AerosTornado.h"
#include "AerosVolcano.h"
#include "AerosWaterSurfaceComponent.h"
#include "AerosLightning.h"
#include "AerosCloudManager.h"
#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "InputCoreTypes.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "ShaderCompiler.h"
#include "Misc/Paths.h"

namespace AerosDemoPawnInternal
{
	/** Shaders still compiling (uncooked game / editor): the tour waits, or its screenshots show default materials. */
	int32 PendingShaderJobs()
	{
		return GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0;
	}
}

AAerosDemoPawn::AAerosDemoPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(35.f);
	Collision->SetCollisionProfileName(TEXT("Pawn"));
	RootComponent = Collision;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->bUsePawnControlRotation = true;

	Precipitation = CreateDefaultSubobject<UAerosPrecipitationComponent>(TEXT("Precipitation"));
	Precipitation->SetupAttachment(Camera);

	WaterSurface = CreateDefaultSubobject<UAerosWaterSurfaceComponent>(TEXT("WaterSurface"));
	WaterSurface->SetupAttachment(RootComponent);

	Movement = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Movement"));
	Movement->UpdatedComponent = Collision;
	Movement->MaxSpeed = BaseSpeed;
	Movement->Acceleration = 20000.f;
	Movement->Deceleration = 20000.f;

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = true;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
}

void AAerosDemoPawn::BeginPlay()
{
	Super::BeginPlay();
	SetActorLocation(GetActorLocation() + FVector(0, 0, 200.0));
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetInputMode(FInputModeGameOnly());
		PC->bShowMouseCursor = false;
	}
	Notify(TEXT("Aeros demo ready (AZERTY). F1 = help, F2 = guided tour."));
	if (FParse::Param(FCommandLine::Get(), TEXT("AerosTour")) || FParse::Param(FCommandLine::Get(), TEXT("AerosTourQuit")))
	{
		bQuitAfterTour = FParse::Param(FCommandLine::Get(), TEXT("AerosTourQuit"));
		AutoTourDelay = 5.f; // let the simulation start and the first snapshot arrive
	}
}

void AAerosDemoPawn::Notify(const FString& Message)
{
	LastMessage = Message;
	MessageTime = 4.f;
}

void AAerosDemoPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	MessageTime = FMath::Max(0.f, MessageTime - DeltaSeconds);
	if (AutoTourDelay > 0.f)
	{
		// The countdown only runs once every shader is compiled
		const int32 Pending = AerosDemoPawnInternal::PendingShaderJobs();
		if (Pending > 0) Notify(FString::Printf(TEXT("Compilation des shaders : %d restants, la visite attend..."), Pending));
		WaitForShaders(Pending > 0);
		if (Pending == 0 && (AutoTourDelay -= DeltaSeconds) <= 0.f) StartTour();
	}
	TickTour(DeltaSeconds);
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC || !PC->IsLocalController()) return;
	HandleMovement(PC, DeltaSeconds);
	HandleHotkeys(PC);
}

void AAerosDemoPawn::HandleMovement(APlayerController* PC, float DeltaSeconds)
{
	float MX = 0.f, MY = 0.f;
	PC->GetInputMouseDelta(MX, MY);
	AddControllerYawInput(MX * MouseSensitivity * 10.f);
	AddControllerPitchInput(-MY * MouseSensitivity * 10.f);

	float Mult = 1.f;
	if (PC->IsInputKeyDown(EKeys::LeftShift)) Mult = 10.f;
	if (PC->IsInputKeyDown(EKeys::LeftControl)) Mult = 100.f;
	Movement->MaxSpeed = BaseSpeed * Mult;
	Movement->Acceleration = Movement->MaxSpeed * 8.f;
	Movement->Deceleration = Movement->MaxSpeed * 8.f;

	// AZERTY: Z Q S D (+ arrows). Up: Space / E, down: A / C
	const FRotator Rot = PC->GetControlRotation();
	const FVector Fwd = Rot.Vector();
	const FVector Right = FRotationMatrix(Rot).GetScaledAxis(EAxis::Y);
	auto Down = [PC](const FKey& A, const FKey& B) { return PC->IsInputKeyDown(A) || PC->IsInputKeyDown(B); };
	if (Down(EKeys::Z, EKeys::Up)) AddMovementInput(Fwd, 1.f);
	if (Down(EKeys::S, EKeys::Down)) AddMovementInput(Fwd, -1.f);
	if (Down(EKeys::D, EKeys::Right)) AddMovementInput(Right, 1.f);
	if (Down(EKeys::Q, EKeys::Left)) AddMovementInput(Right, -1.f);
	if (Down(EKeys::E, EKeys::SpaceBar)) AddMovementInput(FVector::UpVector, 1.f);
	if (Down(EKeys::A, EKeys::C)) AddMovementInput(FVector::UpVector, -1.f);
}

namespace
{
	struct FCity { const TCHAR* Name; float Lat; float Lon; };
	const FCity Cities[] = {
		{ TEXT("Paris"), 48.857f, 2.352f },
		{ TEXT("Oklahoma City"), 35.468f, -97.516f },
		{ TEXT("Miami"), 25.762f, -80.192f },
		{ TEXT("Tokyo"), 35.676f, 139.650f },
		{ TEXT("Reykjavik"), 64.147f, -21.943f },
		{ TEXT("Phoenix"), 33.448f, -112.074f },
		{ TEXT("Mumbai"), 19.076f, 72.878f },
		{ TEXT("Montreal"), 45.502f, -73.567f },
		{ TEXT("Dakar"), 14.716f, -17.467f },
		{ TEXT("Sydney"), -33.869f, 151.209f },
	};
}

void AAerosDemoPawn::HandleHotkeys(APlayerController* PC)
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	auto Pressed = [PC](std::initializer_list<FKey> Keys)
	{
		for (const FKey& K : Keys) if (PC->WasInputKeyJustPressed(K)) return true;
		return false;
	};

	// Scenarios on the AZERTY number row (& e " ' ( - e _ c a), QWERTY digits work too
	struct FScenarioKey { FKey Azerty; FKey Qwerty; EAerosScenario Scenario; };
	const FScenarioKey ScenarioKeys[] = {
		{ EKeys::Ampersand, EKeys::One, EAerosScenario::Climate },
		{ EKeys::E_AccentAigu, EKeys::Two, EAerosScenario::FairWeather },
		{ EKeys::Quote, EKeys::Three, EAerosScenario::SummerThunderstorms },
		{ EKeys::Apostrophe, EKeys::Four, EAerosScenario::SupercellOutbreak },
		{ EKeys::LeftParantheses, EKeys::Five, EAerosScenario::WinterStorm },
		{ EKeys::Hyphen, EKeys::Six, EAerosScenario::TropicalSeason },
		{ EKeys::E_AccentGrave, EKeys::Seven, EAerosScenario::FrontalRain },
		{ EKeys::Underscore, EKeys::Eight, EAerosScenario::Blizzard },
		{ EKeys::C_Cedille, EKeys::Nine, EAerosScenario::DesertDust },
		{ EKeys::A_AccentGrave, EKeys::Zero, EAerosScenario::Monsoon },
	};
	for (const FScenarioKey& K : ScenarioKeys)
	{
		if (Pressed({ K.Azerty, K.Qwerty }))
		{
			Sub->SetScenario(K.Scenario);
			Notify(FString::Printf(TEXT("Scenario: %s (the atmosphere adjusts over the next hours)"), *UAerosWeatherLibrary::ScenarioToText(K.Scenario).ToString()));
		}
	}

	const FVector Here = GetActorLocation();
	const FVector Ahead = Here + PC->GetControlRotation().Vector().GetSafeNormal2D() * 1500000.0;

	// ---- Weather ----------------------------------------------------------------------
	if (Pressed({ EKeys::B }))
	{
		Sub->InjectThermalBubble(Ahead, 6.f, 4.f, 3.f);
		Notify(TEXT("Thermal bubble 15 km ahead (+4 K, +3 g/kg)."));
	}
	if (Pressed({ EKeys::T }))
	{
		if (AAerosTornado* T = Sub->ForceTornado(-1))
			Notify(FString::Printf(TEXT("Tornado EF%d (%.0f km away)."), T->GetEFRating(), FVector::Dist2D(T->GetActorLocation(), Here) * 1e-5));
	}
	if (Pressed({ EKeys::H }))
	{
		FVector Coast, Offshore;
		if (Sub->FindNearestOcean(Here, Coast, Offshore, 150.f)) Sub->SpawnHurricane(Offshore, 950.f, 35.f);
		else Sub->SpawnHurricane(Here + FVector(25000000.0, 0, 0), 950.f, 35.f);
		Notify(TEXT("Hurricane seeded over the ocean (storm surge on the coast)."));
	}

	// ---- Disasters --------------------------------------------------------------------
	if (Pressed({ EKeys::G }))
	{
		Sub->TriggerEarthquake(Ahead, 6.6f, 10.f);
		Notify(TEXT("Earthquake M6.6, 15 km ahead, 10 km deep."));
	}
	if (Pressed({ EKeys::Y }))
	{
		FVector Coast, Offshore;
		if (Sub->FindNearestOcean(Here, Coast, Offshore, 70.f))
		{
			Sub->TriggerEarthquake(Offshore, 9.0f, 20.f);
			Notify(FString::Printf(TEXT("Submarine earthquake M9.0, %.0f km away: TSUNAMI. Go to the coast (key I)."), FVector::Dist2D(Offshore, Here) * 1e-5));
		}
		else Notify(TEXT("No ocean in the simulated area."));
	}
	if (Pressed({ EKeys::U }))
	{
		AAerosVolcano* Best = nullptr;
		for (AAerosVolcano* V : Sub->GetVolcanoes())
			if (!Best || FVector::DistSquared(V->GetActorLocation(), Here) < FVector::DistSquared(Best->GetActorLocation(), Here)) Best = V;
		if (Best)
		{
			VolcanoVEI = VolcanoVEI % 5 + 2; // 3, 4, 5, 6, 2, ...
			Best->StartEruption(VolcanoVEI);
			Notify(FString::Printf(TEXT("Volcano: unrest, then VEI %d eruption (%.0f km away). Key V to go there."), VolcanoVEI, FVector::Dist2D(Best->GetActorLocation(), Here) * 1e-5));
		}
	}
	if (Pressed({ EKeys::O }))
	{
		Sub->InjectRain(Here, 15.f, 250.f);
		Notify(TEXT("Cloudburst: 250 mm over 15 km. Flash flood incoming."));
	}
	if (Pressed({ EKeys::L }))
	{
		Sub->InjectOutflow(Ahead, 25.f, -12.f);
		Notify(TEXT("Downburst 15 km ahead: haboob over dry ground (try scenario 9 / c-cedilla)."));
	}
	if (Pressed({ EKeys::W }))
	{
		const FCity& City = Cities[CityIndex % UE_ARRAY_COUNT(Cities)];
		++CityIndex;
		Sub->RequestRealWeather(City.Lat, City.Lon, City.Name, true, true);
		Notify(FString::Printf(TEXT("Real weather: %s (%.2f, %.2f), Open-Meteo..."), City.Name, City.Lat, City.Lon));
	}

	// ---- Travel ------------------------------------------------------------------------
	if (Pressed({ EKeys::F })) FlyToNearestStorm();
	if (Pressed({ EKeys::V }))
	{
		for (AAerosVolcano* V : Sub->GetVolcanoes())
		{
			SetActorLocation(V->GetActorLocation() + FVector(1500000.0, 1500000.0, 150000.0));
			if (AController* C = GetController()) C->SetControlRotation((V->GetVentLocation() - GetActorLocation()).Rotation());
			Notify(TEXT("Teleported 20 km from the volcano."));
			break;
		}
	}
	if (Pressed({ EKeys::I }))
	{
		FVector Coast, Offshore;
		if (Sub->FindNearestOcean(Here, Coast, Offshore, 50.f))
		{
			const FVector Inland = Coast - (Offshore - Coast).GetSafeNormal2D() * 300000.0;
			SetActorLocation(FVector(Inland.X, Inland.Y, 3000.0));
			if (AController* C = GetController()) C->SetControlRotation((Offshore - Inland).Rotation());
			Notify(TEXT("Teleported 3 km inland, facing the sea."));
		}
	}

	// ---- Time ----------------------------------------------------------------------------
	if (Pressed({ EKeys::Equals, EKeys::Add, EKeys::PageUp }))
	{
		Sub->SetTimeScale(FMath::Min(Sub->GetTimeScale() * 2.f, 7680.f));
		Notify(FString::Printf(TEXT("Time scale x%.0f"), Sub->GetTimeScale()));
	}
	if (Pressed({ EKeys::RightParantheses, EKeys::Subtract, EKeys::PageDown }))
	{
		Sub->SetTimeScale(FMath::Max(Sub->GetTimeScale() * 0.5f, 1.f));
		Notify(FString::Printf(TEXT("Time scale x%.0f"), Sub->GetTimeScale()));
	}
	if (Pressed({ EKeys::P }))
	{
		Sub->SetPaused(!Sub->IsPaused());
		Notify(Sub->IsPaused() ? TEXT("Weather paused") : TEXT("Weather running"));
	}
	const FAerosDateTime Now = Sub->GetDateTime();
	if (Pressed({ EKeys::N })) { Sub->SetDateTime(Now.DayOfYear, FMath::Fmod(Now.Hour + 23.f, 24.f)); Notify(TEXT("-1 hour")); }
	if (Pressed({ EKeys::M })) { Sub->SetDateTime(Now.DayOfYear, FMath::Fmod(Now.Hour + 1.f, 24.f)); Notify(TEXT("+1 hour")); }
	if (Pressed({ EKeys::J })) { Sub->SetDateTime((Now.DayOfYear + 365 - 31) % 365 + 1, Now.Hour); Notify(TEXT("-1 month")); }
	if (Pressed({ EKeys::K })) { Sub->SetDateTime((Now.DayOfYear + 30) % 365 + 1, Now.Hour); Notify(TEXT("+1 month")); }

	// ---- Display ----------------------------------------------------------------------------
	if (Pressed({ EKeys::R }))
	{
		for (TActorIterator<AAerosWeatherRadar> It(GetWorld()); It; ++It)
		{
			const EAerosRadarProduct Next = static_cast<EAerosRadarProduct>((int32(It->Product) + 1) % 4);
			It->SetProduct(Next);
			Notify(FString::Printf(TEXT("Radar: %s"), *StaticEnum<EAerosRadarProduct>()->GetDisplayNameTextByValue(int64(Next)).ToString()));
		}
	}
	if (Pressed({ EKeys::X }))
	{
		DebugLevel = (DebugLevel + 1) % 3;
		UKismetSystemLibrary::ExecuteConsoleCommand(this, FString::Printf(TEXT("aeros.Debug %d"), DebugLevel), PC);
		Notify(FString::Printf(TEXT("Debug draw %d"), DebugLevel));
	}
	if (Pressed({ EKeys::F2 }))
	{
		if (IsTourRunning()) StopTour(); else StartTour();
	}
	if (Pressed({ EKeys::F1, EKeys::Tab }))
	{
		if (AAerosDemoHUD* HUD = Cast<AAerosDemoHUD>(PC->GetHUD())) HUD->CycleMode();
	}
}

void AAerosDemoPawn::FlyToNearestStorm()
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	FAerosStormData Storm;
	if (!Sub || !Sub->GetNearestStorm(GetActorLocation(), Storm, 1000.f))
	{
		Notify(TEXT("No storm in the domain yet. Try scenario 4 (supercells) or key B."));
		return;
	}
	// Park 20 km south-east of the core, looking at it: classic chaser position
	const FVector Target = Storm.UpdraftLocation + FVector(1400000.0, 1400000.0, 0.0);
	SetActorLocation(FVector(Target.X, Target.Y, GetActorLocation().Z));
	if (AController* C = GetController())
	{
		C->SetControlRotation((Storm.UpdraftLocation + FVector(0, 0, 300000.0) - GetActorLocation()).Rotation());
	}
	Notify(FString::Printf(TEXT("Teleported near storm %d (%s, %.0f dBZ, hail %.0f mm)"), Storm.Id,
		*StaticEnum<EAerosStormType>()->GetDisplayNameTextByValue(int64(Storm.Type)).ToString(), Storm.MaxReflectivity, Storm.MaxHail));
}

// =====================================================================
//  Guided tour
// =====================================================================

void AAerosDemoPawn::TeleportLookAt(const FVector& Location, const FVector& Target)
{
	SetActorLocation(Location);
	if (AController* C = GetController()) C->SetControlRotation((Target - Location).Rotation());
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AerosTourGround), false, this);
	const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Location, Location - FVector(0, 0, 5000000.0), ECC_WorldStatic, Params);
	UE_LOG(LogTemp, Display, TEXT("Aeros tour: camera at (%.0f, %.0f, %.0f), below: %s at Z=%.0f"), Location.X, Location.Y, Location.Z,
		bHit ? *GetNameSafe(Hit.GetActor()) : TEXT("nothing"), bHit ? Hit.ImpactPoint.Z : 0.0);
}

void AAerosDemoPawn::StartTour()
{
	BuildTour();
	// -AerosTourOnly=1,3 keeps only those steps (1-based): faster visual checks
	FString Only;
	if (FParse::Value(FCommandLine::Get(), TEXT("AerosTourOnly="), Only, false)) // false: do not stop at the commas
	{
		TArray<FString> Parts;
		Only.ParseIntoArray(Parts, TEXT(","));
		TArray<FTourStep> Kept;
		for (const FString& P : Parts)
		{
			const int32 Index = FCString::Atoi(*P) - 1;
			if (Tour.IsValidIndex(Index)) Kept.Add(Tour[Index]);
		}
		Tour = MoveTemp(Kept);
	}
	if (Tour.Num() == 0) return;
	TourStep = 0;
	TourTime = 0.f;
	bTourShotTaken = false;
	TourTitle = Tour[0].Title;
	TourCaption = Tour[0].Caption;
	if (Tour[0].Enter) Tour[0].Enter();
}

void AAerosDemoPawn::StopTour()
{
	TourStep = -1;
	TourTitle.Empty();
	TourCaption.Empty();
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) Sub->SetTimeScale(60.f);
	Notify(TEXT("Visite termin\u00E9e."));
}

void AAerosDemoPawn::WaitForShaders(bool bWait)
{
	// The weather is paused too, so the tour sees the same weather on every run
	if (bWait == bPausedForShaders) return;
	bPausedForShaders = bWait;
	if (UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this)) Sub->SetPaused(bWait);
}

void AAerosDemoPawn::TickTour(float DeltaSeconds)
{
	if (TourStep < 0 || !Tour.IsValidIndex(TourStep)) return;
	// Freeze the step while shaders compile (a new effect appeared): no screenshot of default materials
	const int32 Pending = AerosDemoPawnInternal::PendingShaderJobs();
	WaitForShaders(Pending > 0);
	if (Pending > 0)
	{
		Notify(FString::Printf(TEXT("Compilation des shaders : %d restants (la visite et la m\u00E9t\u00E9o sont en pause)"), Pending));
		return;
	}
	TourTime += DeltaSeconds;
	const FTourStep& Step = Tour[TourStep];

	// One screenshot near the end of each step (HUD included, so the explanation is in the picture)
	bool bLightningNow = false;
	if (Step.bShotOnLightning && !bTourShotTaken && TourTime > 2.f)
	{
		for (TActorIterator<AAerosLightningFlash> It(GetWorld()); It; ++It)
		{
			const FVector To = (It->GetActorLocation() - GetActorLocation()).GetSafeNormal();
			const bool bInView = FVector::DotProduct(To, GetControlRotation().Vector()) > 0.6f;
			if (bInView && It->GetLightningType() != EAerosLightningType::IntraCloud && It->GetBrightness() > 0.5f
				&& FVector::Dist2D(It->GetActorLocation(), GetActorLocation()) < 4000000.0) { bLightningNow = true; break; }
		}
	}
	if (!bTourShotTaken && (bLightningNow || TourTime >= Step.Duration - 0.5f))
	{
		bTourShotTaken = true;
		const FString File = FPaths::ScreenShotDir() / TEXT("AerosTour") / (Step.Shot + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(File, true, false);
		if (FParse::Param(FCommandLine::Get(), TEXT("AerosDumpMaps")))
		{
			if (UAerosWeatherSubsystem* DumpSub = UAerosWeatherSubsystem::Get(this))
			{
				if (DumpSub->GetCloudManager())
				{
					DumpSub->GetCloudManager()->DumpMaps(Step.Shot);
					DumpSub->GetCloudManager()->DumpVolumeSlices(Step.Shot);
				}
			}
		}
	}
	if (TourTime < Step.Duration) return;

	++TourStep;
	TourTime = 0.f;
	bTourShotTaken = false;
	if (!Tour.IsValidIndex(TourStep))
	{
		StopTour();
		if (bQuitAfterTour)
		{
			if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->ConsoleCommand(TEXT("quit"));
		}
		return;
	}
	TourTitle = Tour[TourStep].Title;
	TourCaption = Tour[TourStep].Caption;
	if (Tour[TourStep].Enter) Tour[TourStep].Enter();
}

void AAerosDemoPawn::BuildTour()
{
	Tour.Reset();
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;
	const int32 NumSteps = 11;
	auto Title = [NumSteps](int32 K, const TCHAR* Name) { return FString::Printf(TEXT("Visite %d/%d : %s"), K, NumSteps, Name); };
	auto Forward = [this]() { return GetControlRotation().Vector().GetSafeNormal2D(); };

	FTourStep S;

	S.Title = Title(1, TEXT("une vraie atmosph\u00E8re"));
	S.Caption = TEXT("Aeros calcule l'air sur 320 x 320 km : temp\u00E9rature, humidit\u00E9, vent et pression, \u00E0 3 altitudes. Rien n'est script\u00E9 : nuages, orages et catastrophes naissent de ces calculs. Le temps passe ici 600 fois plus vite pour que les orages se forment.");
	S.Shot = TEXT("01_atmosphere");
	S.Duration = 14.f;
	S.Enter = [this, Sub]()
	{
		Sub->SetScenario(EAerosScenario::SupercellOutbreak);
		Sub->SetTimeScale(600.f);
		TeleportLookAt(FVector(0, 0, 40000.0), FVector(1000000.0, 0, 200000.0));
	};
	Tour.Add(S);

	S.Title = Title(2, TEXT("un orage"));
	S.Caption = TEXT("L'air chaud et humide du sol monte, se refroidit et condense : une tour de nuage grimpe jusqu'\u00E0 10-15 km. La colonne sombre sous la base, c'est la pluie (blanch\u00E2tre s'il y a de la gr\u00EAle). Le losange au-dessus dit quel type d'orage c'est et \u00E0 quelle distance.");
	S.Shot = TEXT("02_orage");
	S.Duration = 10.f;
	S.Enter = [this, Sub]()
	{
		Sub->SetTimeScale(60.f);
		FAerosStormData Storm;
		if (!Sub->GetNearestStorm(GetActorLocation(), Storm, 1000.f))
		{
			Sub->InjectThermalBubble(GetActorLocation() + FVector(1500000.0, 0, 0), 8.f, 5.f, 3.f);
			return;
		}
		TeleportLookAt(Storm.UpdraftLocation + FVector(1800000.0, 1800000.0, 30000.0), Storm.UpdraftLocation + FVector(0, 0, 400000.0));
	};
	Tour.Add(S);

	S.Title = Title(3, TEXT("les \u00E9clairs"));
	S.Caption = TEXT("La nuit, on voit les \u00E9clairs. Le plus courant part du nuage vers le sol (n\u00E9gatif) : un traceur descend en se ramifiant, puis 3 \u00E0 5 arcs repassent dans le m\u00EAme canal, d'o\u00F9 le scintillement. Plus rare, l'\u00E9clair positif tombe de l'enclume, loin de l'orage, avec un seul arc tr\u00E8s puissant. Beaucoup restent dans le nuage et l'illuminent seulement.");
	S.Shot = TEXT("03_eclairs");
	S.Duration = 14.f;
	S.bShotOnLightning = true;
	S.Enter = [this, Sub]()
	{
		const FAerosDateTime Now = Sub->GetDateTime();
		Sub->SetDateTime(Now.DayOfYear, 22.5f);
		Sub->SetTimeScale(60.f);
		FAerosStormData Storm;
		if (Sub->GetNearestStorm(GetActorLocation(), Storm, 1000.f))
		{
			TeleportLookAt(Storm.UpdraftLocation + FVector(1200000.0, 1200000.0, 30000.0), Storm.UpdraftLocation + FVector(0, 0, 150000.0));
		}
	};
	Tour.Add(S);
	S.bShotOnLightning = false;

	S.Title = Title(4, TEXT("vue d'ensemble"));
	S.Caption = TEXT("Vu de haut. Le radar (en haut \u00E0 droite) montre la pluie : vert = faible, rouge = forte, violet = gr\u00EAle. Les X jaunes marquent les orages qui tournent sur eux-m\u00EAmes (m\u00E9socyclones) : ce sont eux qui font les tornades.");
	S.Shot = TEXT("04_vue_ensemble");
	S.Duration = 7.f;
	S.Enter = [this, Sub, Forward]()
	{
		Sub->SetDateTime(Sub->GetDateTime().DayOfYear, 15.f);
		const FVector Here = GetActorLocation();
		TeleportLookAt(FVector(Here.X, Here.Y, 1500000.0), FVector(Here.X, Here.Y, 0.0) + Forward() * 3000000.0);
	};
	Tour.Add(S);

	S.Title = Title(5, TEXT("une tornade"));
	S.Caption = TEXT("Quand l'orage tourne et que la base du nuage est basse, la rotation peut descendre jusqu'au sol : c'est la tornade. Le vent y d\u00E9passe 200 km/h et aspire les caisses. Sa force est not\u00E9e de EF0 \u00E0 EF5.");
	S.Shot = TEXT("05_tornade");
	S.Duration = 10.f;
	S.Enter = [this, Sub]()
	{
		Sub->SetTimeScale(60.f);
		if (AAerosTornado* T = Sub->ForceTornado(-1))
		{
			const FVector At = T->GetActorLocation();
			TeleportLookAt(FVector(At.X + 400000.0, At.Y + 300000.0, 25000.0), FVector(At.X, At.Y, 60000.0));
		}
	};
	Tour.Add(S);

	S.Title = Title(6, TEXT("un volcan"));
	S.Caption = TEXT("Le volcan se r\u00E9veille (petits s\u00E9ismes), puis entre en \u00E9ruption. La colonne de cendres monte jusqu'\u00E0 20-30 km, le vent d'altitude l'emporte et la cendre retombe loin. Coul\u00E9es de lave et bombes volcaniques autour du crat\u00E8re.");
	S.Shot = TEXT("06_volcan");
	S.Duration = 22.f;
	S.Enter = [this, Sub]()
	{
		Sub->SetTimeScale(120.f);
		for (AAerosVolcano* V : Sub->GetVolcanoes())
		{
			V->UnrestSeconds = 6.f; // short warning phase so the tour sees the eruption
			V->StartEruption(5);
			TeleportLookAt(V->GetActorLocation() + FVector(900000.0, 900000.0, 80000.0), V->GetVentLocation() + FVector(0, 0, 300000.0));
			break;
		}
	};
	Tour.Add(S);

	S.Title = Title(7, TEXT("un s\u00E9isme"));
	S.Caption = TEXT("Une faille casse \u00E0 10 km sous terre. Les ondes P (6 km/s) arrivent d'abord, puis les ondes S, plus fortes : la cam\u00E9ra tremble et les caisses tombent. La jauge Secousse donne l'intensit\u00E9 ressentie ici.");
	S.Shot = TEXT("07_seisme");
	S.Duration = 7.f;
	S.Enter = [this, Sub, Forward]()
	{
		Sub->SetTimeScale(60.f);
		TeleportLookAt(FVector(0, 0, 3000.0), FVector(800000.0, 0, 3000.0));
		Sub->TriggerEarthquake(GetActorLocation() + Forward() * 1200000.0, 6.8f, 10.f);
	};
	Tour.Add(S);

	S.Title = Title(8, TEXT("un tsunami"));
	S.Caption = TEXT("Un s\u00E9isme M9 soul\u00E8ve le fond de la mer au large. La vague file \u00E0 700 km/h en eau profonde, puis ralentit et grandit en arrivant sur la c\u00F4te. Souvent la mer se retire d'abord : c'est le signal pour fuir.");
	S.Shot = TEXT("08_tsunami");
	S.Duration = 18.f;
	S.Enter = [this, Sub]()
	{
		FVector Coast, Offshore;
		if (!Sub->FindNearestOcean(GetActorLocation(), Coast, Offshore, 70.f)) return;
		Sub->TriggerEarthquake(Offshore, 9.f, 20.f);
		const FVector Inland = Coast - (Offshore - Coast).GetSafeNormal2D() * 300000.0;
		TeleportLookAt(FVector(Inland.X, Inland.Y, 5000.0), FVector(Offshore.X, Offshore.Y, 0.0));
	};
	Tour.Add(S);

	S.Title = Title(9, TEXT("une crue \u00E9clair"));
	S.Caption = TEXT("300 mm de pluie tombent d'un coup. Le sol ne peut pas tout absorber : l'eau ruisselle, suit la pente et s'accumule dans les creux. Le courant pousse les objets.");
	S.Shot = TEXT("09_crue");
	S.Duration = 14.f;
	S.Enter = [this, Sub, Forward]()
	{
		Sub->SetTimeScale(240.f);
		TeleportLookAt(FVector(-400000.0, 400000.0, 3000.0), FVector(-400000.0, 400000.0, 0.0) + Forward() * 400000.0);
		Sub->InjectRain(GetActorLocation(), 15.f, 300.f);
	};
	Tour.Add(S);

	S.Title = Title(10, TEXT("une temp\u00EAte de sable"));
	S.Caption = TEXT("Au-dessus du d\u00E9sert, la rafale froide qui sort d'un orage frappe le sol sec et soul\u00E8ve le sable : un mur de poussi\u00E8re (haboob) avance et la visibilit\u00E9 tombe \u00E0 quelques centaines de m\u00E8tres.");
	S.Shot = TEXT("10_haboob");
	S.Duration = 14.f;
	S.Enter = [this, Sub, Forward]()
	{
		Sub->SetScenario(EAerosScenario::DesertDust);
		Sub->SetTimeScale(60.f);
		TeleportLookAt(FVector(-12000000.0, 2000000.0, 20000.0), FVector(-10000000.0, 2000000.0, 20000.0));
		Sub->InjectOutflow(GetActorLocation() + Forward() * 1200000.0, 25.f, -12.f);
	};
	Tour.Add(S);

	S.Title = Title(11, TEXT("un blizzard"));
	S.Caption = TEXT("Air glac\u00E9 et vent fort : la neige tombe, puis le vent la soul\u00E8ve \u00E0 nouveau. Tout devient blanc, on ne voit plus l'horizon. Le froid ressenti est bien plus bas que la temp\u00E9rature.");
	S.Shot = TEXT("11_blizzard");
	S.Duration = 16.f;
	S.Enter = [this, Sub]()
	{
		Sub->SetScenario(EAerosScenario::Blizzard);
		Sub->SetTimeScale(900.f);
		TeleportLookAt(FVector(0, 0, 3000.0), FVector(1000000.0, 0, 3000.0));
	};
	Tour.Add(S);
}
