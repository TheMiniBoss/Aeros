// Copyright Aeros. All Rights Reserved.

#include "AerosDemoPawn.h"
#include "AerosDemoHUD.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherRadar.h"
#include "AerosWeatherLibrary.h"
#include "AerosPrecipitationComponent.h"
#include "AerosTornado.h"
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
	Notify(TEXT("Aeros demo ready. F1 = help."));
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

	const FRotator Rot = PC->GetControlRotation();
	const FVector Fwd = Rot.Vector();
	const FVector Right = FRotationMatrix(Rot).GetScaledAxis(EAxis::Y);
	if (PC->IsInputKeyDown(EKeys::W)) AddMovementInput(Fwd, 1.f);
	if (PC->IsInputKeyDown(EKeys::S)) AddMovementInput(Fwd, -1.f);
	if (PC->IsInputKeyDown(EKeys::D)) AddMovementInput(Right, 1.f);
	if (PC->IsInputKeyDown(EKeys::A)) AddMovementInput(Right, -1.f);
	if (PC->IsInputKeyDown(EKeys::E) || PC->IsInputKeyDown(EKeys::SpaceBar)) AddMovementInput(FVector::UpVector, 1.f);
	if (PC->IsInputKeyDown(EKeys::Q) || PC->IsInputKeyDown(EKeys::C)) AddMovementInput(FVector::UpVector, -1.f);
}

void AAerosDemoPawn::HandleHotkeys(APlayerController* PC)
{
	UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(this);
	if (!Sub) return;

	// Scenarios 1..7
	const FKey ScenarioKeys[] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven };
	for (int32 K = 0; K < 7; ++K)
	{
		if (PC->WasInputKeyJustPressed(ScenarioKeys[K]))
		{
			const EAerosScenario Sc = static_cast<EAerosScenario>(K);
			Sub->SetScenario(Sc);
			Notify(FString::Printf(TEXT("Scenario: %s (the atmosphere adjusts over the next hours)"), *UAerosWeatherLibrary::ScenarioToText(Sc).ToString()));
		}
	}

	const FVector Ahead = GetActorLocation() + PC->GetControlRotation().Vector().GetSafeNormal2D() * 1500000.0;

	if (PC->WasInputKeyJustPressed(EKeys::B))
	{
		Sub->InjectThermalBubble(Ahead, 6.f, 4.f, 3.f);
		Notify(TEXT("Thermal bubble injected 15 km ahead (+4 K, +3 g/kg)."));
	}
	if (PC->WasInputKeyJustPressed(EKeys::T))
	{
		if (AAerosTornado* T = Sub->ForceTornado(-1))
			Notify(FString::Printf(TEXT("Tornado EF%d spawned (%.0f km away)."), T->GetEFRating(), FVector::Dist2D(T->GetActorLocation(), GetActorLocation()) * 1e-5));
	}
	if (PC->WasInputKeyJustPressed(EKeys::H))
	{
		Sub->SpawnHurricane(GetActorLocation() + FVector(25000000.0, 0, 0), 960.f, 35.f);
		Notify(TEXT("Hurricane seeded 250 km east (over the ocean)."));
	}
	if (PC->WasInputKeyJustPressed(EKeys::Equals) || PC->WasInputKeyJustPressed(EKeys::Add))
	{
		Sub->SetTimeScale(FMath::Min(Sub->GetTimeScale() * 2.f, 7680.f));
		Notify(FString::Printf(TEXT("Time scale x%.0f"), Sub->GetTimeScale()));
	}
	if (PC->WasInputKeyJustPressed(EKeys::Hyphen) || PC->WasInputKeyJustPressed(EKeys::Subtract))
	{
		Sub->SetTimeScale(FMath::Max(Sub->GetTimeScale() * 0.5f, 1.f));
		Notify(FString::Printf(TEXT("Time scale x%.0f"), Sub->GetTimeScale()));
	}
	if (PC->WasInputKeyJustPressed(EKeys::P))
	{
		Sub->SetPaused(!Sub->IsPaused());
		Notify(Sub->IsPaused() ? TEXT("Weather paused") : TEXT("Weather running"));
	}
	const FAerosDateTime Now = Sub->GetDateTime();
	if (PC->WasInputKeyJustPressed(EKeys::N)) { Sub->SetDateTime(Now.DayOfYear, FMath::Fmod(Now.Hour + 23.f, 24.f)); Notify(TEXT("-1 hour")); }
	if (PC->WasInputKeyJustPressed(EKeys::M)) { Sub->SetDateTime(Now.DayOfYear, FMath::Fmod(Now.Hour + 1.f, 24.f)); Notify(TEXT("+1 hour")); }
	if (PC->WasInputKeyJustPressed(EKeys::J)) { Sub->SetDateTime((Now.DayOfYear + 365 - 31) % 365 + 1, Now.Hour); Notify(TEXT("-1 month")); }
	if (PC->WasInputKeyJustPressed(EKeys::K)) { Sub->SetDateTime((Now.DayOfYear + 30) % 365 + 1, Now.Hour); Notify(TEXT("+1 month")); }

	if (PC->WasInputKeyJustPressed(EKeys::R))
	{
		for (TActorIterator<AAerosWeatherRadar> It(GetWorld()); It; ++It)
		{
			const EAerosRadarProduct Next = static_cast<EAerosRadarProduct>((int32(It->Product) + 1) % 3);
			It->SetProduct(Next);
			Notify(FString::Printf(TEXT("Radar: %s"), *StaticEnum<EAerosRadarProduct>()->GetDisplayNameTextByValue(int64(Next)).ToString()));
		}
	}
	if (PC->WasInputKeyJustPressed(EKeys::F)) FlyToNearestStorm();
	if (PC->WasInputKeyJustPressed(EKeys::V))
	{
		DebugLevel = (DebugLevel + 1) % 3;
		UKismetSystemLibrary::ExecuteConsoleCommand(this, FString::Printf(TEXT("aeros.Debug %d"), DebugLevel), PC);
		Notify(FString::Printf(TEXT("Debug draw %d"), DebugLevel));
	}
	if (PC->WasInputKeyJustPressed(EKeys::F1) || PC->WasInputKeyJustPressed(EKeys::Tab))
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
