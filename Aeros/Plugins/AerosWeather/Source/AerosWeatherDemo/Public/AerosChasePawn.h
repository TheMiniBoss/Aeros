// Copyright Aeros. All Rights Reserved.
//
// Storm chaser: the playable demo. A ground vehicle (first person) chases supercells over the procedural
// landscape during one afternoon; photos of tornadoes, wall clouds, lightning and whole supercells score
// points (AerosChaseRules.h), tornado winds, hail, close strikes and floods damage the vehicle.
// The pawn also runs the start menu (playable demo / technical demo / quit), the briefing, the pause
// and the end-of-day summary; AAerosChaseHUD draws them.
// Keys (AZERTY): Z/S drive, Q/D steer, mouse look, right click zoom, left click or Space photo,
// Escape pause, Backspace menu. No function keys (the engine uses them for debugging).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "AerosChaseRules.h"
#include "AerosTypes.h"
#include "AerosChasePawn.generated.h"

class UCameraComponent;
class UAerosPrecipitationComponent;
class UAerosWaterSurfaceComponent;
class APlayerController;
class UAerosWeatherSubsystem;

UENUM()
enum class EAerosChaseState : uint8
{
	Menu,
	Briefing,
	Driving,
	Destroyed,
	Paused,
	Summary,
};

/** One photo kept in the album (the best per subject). */
USTRUCT()
struct FAerosChasePhoto
{
	GENERATED_BODY()

	FString Label;            // "Tornade EF3 a 1,2 km"
	FString Key;              // subject identity (tornado actor, storm id...): best photo per key counts
	int32 Points = 0;
	float Hour = 0.f;
	FString File;             // screenshot path
};

/** What the camera would capture right now (the viewfinder shows it before the shot). */
struct FAerosChaseShotPreview
{
	FString Label;
	FString Key;
	int32 Points = 0;
	AerosChase::ESubject Subject = AerosChase::ESubject::Tornado;
};

UCLASS()
class AEROSWEATHERDEMO_API AAerosChasePawn : public APawn
{
	GENERATED_BODY()

public:
	AAerosChasePawn();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Chase")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Chase")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Chase")
	TObjectPtr<UAerosPrecipitationComponent> Precipitation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Aeros Chase")
	TObjectPtr<UAerosWaterSurfaceComponent> WaterSurface;

	// ---- State read by the HUD
	EAerosChaseState State = EAerosChaseState::Menu;
	int32 MenuIndex = 0;            // 0 play, 1 technical demo, 2 quit
	int32 SummaryIndex = 0;         // 0 play again, 1 menu
	float StateTime = 0.f;
	float SpeedMs = 0.f;
	float Health = 100.f;
	int32 Score = 0;
	int32 Destroyed = 0;
	int32 HailIntercepts = 0;
	int32 LastPenalty = 0;
	float DamageFlash = 0.f;        // red vignette, decays
	float ShutterFlash = 0.f;       // white flash of the shutter, decays
	bool bZoom = false;
	float BriefingProgress = 0.f;   // 0-1 while the storms grow
	bool bBriefingReady = false;    // a supercell exists: Enter starts the day at once
	FString EndReason;              // why the day ended (summary screen)
	static constexpr float RadarRangeKm = 30.f;
	TArray<FAerosChasePhoto> Album;
	FAerosChaseShotPreview Preview; // live estimate in the viewfinder
	FString LastPhotoText;          // "+420 pts - Tornade EF3 a 1,2 km"
	float LastPhotoTime = 0.f;      // seconds since it, the card fades
	TArray<FString> Alerts;         // current warnings (tornado near, hail, flood...)
	FString Toast;                  // short message
	float ToastTime = 0.f;
	FString BriefingText;

	static constexpr float BriefingStartHour = 15.25f;
	static constexpr float DayStartHour = 16.f;
	static constexpr float DayEndHour = 20.5f;
	/** x2: supercells move at ~15 m/s, ~30 m/s on screen, a 50 m/s car can catch them (at x20 they
	 *  ran at 300 m/s). The 4.5 h evening lasts up to 2 h 15; the pause screen can end it. */
	static constexpr float DayTimeScale = 2.f;
	/** Fast travel (key T): to ~4 km of the nearest supercell, the clock moves on by this much. */
	static constexpr float FastTravelHours = 1.f / 6.f;
	static constexpr float FastTravelKm = 4.f;

	/** Frames left during which the HUD hides itself (a photo is being captured without it). */
	int32 PhotoHideFrames = 0;

	float GetHour() const;
	float GetHeadingDeg() const { return HeadingDeg; }
	int32 BestPhotoPoints() const;

	/** Bearing (deg, 0 = north, clockwise) from the vehicle to a world point. */
	float BearingTo(const FVector& World) const;

private:
	void TickMenu(APlayerController* PC);
	void TickBriefing(float DeltaSeconds);
	void TickDriving(APlayerController* PC, float DeltaSeconds);
	void TickSummary(APlayerController* PC);
	void TickAutopilot(float DeltaSeconds);

	void StartChase();
	void BeginDay();
	void EndDay();
	void Respawn();
	void GoToMenu();
	void GoToTechDemo();

	void Drive(APlayerController* PC, float DeltaSeconds);
	void Look(APlayerController* PC, float DeltaSeconds);
	void ApplyHazards(UAerosWeatherSubsystem* Sub, float DeltaSeconds);
	void TakePhoto();
	FAerosChaseShotPreview EvaluateShot() const;
	void UpdateAlerts(UAerosWeatherSubsystem* Sub);
	void PlaceOnGround();
	FVector FindSafeStart(UAerosWeatherSubsystem* Sub, float DistanceKm, const FVector* Around = nullptr) const;
	void FastTravel();
	void CapturePhoto();
	double GroundZAt(double X, double Y) const;
	void ShowToast(const FString& Text);
	void ApplyCamera();

	UFUNCTION()
	void HandleLightning(const FAerosLightningStrike& Strike);

	float HeadingDeg = 0.f;        // vehicle yaw (deg, Unreal)
	float LookYaw = 0.f;           // camera yaw relative to the vehicle
	float LookPitch = 5.f;
	float HailTime = 0.f;
	TSet<int32> HailStorms;        // storms whose hail core was crossed (scored once)
	double LastStrikeTime = -100.0;
	FVector LastStrikeLocation = FVector::ZeroVector;
	float LastStrikeBrightness = 0.f;
	int32 PhotoCount = 0;
	FString PendingPhotoFile;
	bool bAutopilot = false;
	float AutoPhotoTimer = 0.f;
	float AutoShotTimer = 0.f;
	float QuitAfter = -1.f;
	float EndDayAfter = -1.f;
	float DrivingTime = 0.f;
	float AutoTravelTimer = 0.f;
	/** Tornado-genesis settings saved while the chase tunes them (negative = not tuned). */
	float SavedTornadoInterval = -1.f;
	float SavedTornadoFrequency = 1.f;
	void TuneTornadoGenesis(int32 Mode);   // 0 = restore, 1 = briefing (no tornado), 2 = the day

	/** One storm only: born from a warm bubble west of the player during the briefing. */
	int32 BubbleTries = 0;
	float BubbleTimer = 0.f;
	/** The day ends once the storm is gone (seen at least once, then absent this long). */
	float NoStormTime = 0.f;
	bool bStormSeen = false;
	/** Radar zoomed on the storm while driving (the technical demo keeps its own range). */
	float SavedRadarRange = -1.f;
	void SetRadarZoom(bool bIn);
	void RestoreWeather();
	float AutoLogTimer = 0.f;
	/** Scenario and clock are set from the briefing's ticks: a command issued before the weather
	 *  thread runs (BeginPlay with -AerosChase) is dropped. 0 = not yet, 1 = sent, 2 = clock seen. */
	int32 BriefingSetup = 0;
	float BriefingSetupTimer = 0.f;
	float SessionTime = 0.f;
};
