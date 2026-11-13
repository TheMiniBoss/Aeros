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
#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "InputCoreTypes.h"
#include "EngineUtils.h"

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
	Notify(TEXT("Aeros demo ready (AZERTY). F1 = help."));
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
