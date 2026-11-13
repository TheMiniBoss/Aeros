// Copyright Aeros. All Rights Reserved.
//
// UAerosLiveWeather - projects the real, current weather of a place into the simulation.
// Source: Open-Meteo (free, no API key): surface observations/analysis + 850 / 500 hPa levels
// (+ sea surface temperature from the marine API). The result becomes the synoptic state,
// the latitude and the local date/time of the simulation; storms, fronts and clouds then
// evolve from it with the full physics. Refreshes periodically. Server only (replicated).

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Interfaces/IHttpRequest.h"
#include "AerosTypes.h"
#include "AerosLiveWeather.generated.h"

class UAerosWeatherSubsystem;

USTRUCT(BlueprintType)
struct AEROSWEATHER_API FAerosRealObservation
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") FString PlaceName;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float Latitude = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float Longitude = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") FString LocalTime;          // ISO 8601, local
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float Temperature = 15.f;   // C
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float DewPoint = 5.f;       // C
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float RelativeHumidity = 50.f; // %
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float PressureMSL = 1013.f; // hPa
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float WindSpeed = 0.f;      // m/s
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float WindDirection = 0.f;  // deg, from
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float Precipitation = 0.f;  // mm (last hour)
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float CloudCover = 0.f;     // %
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") int32 WeatherCode = 0;      // WMO code
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float CAPE = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float Temperature850 = 5.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float Temperature500 = -20.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") float SeaSurfaceTemperature = -99.f;
	UPROPERTY(BlueprintReadOnly, Category = "Aeros|Live") FString Description;        // readable WMO text
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnRealWeather, const FAerosRealObservation&, Observation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAerosOnRealWeatherError, const FString&, Error);

UCLASS(BlueprintType)
class AEROSWEATHER_API UAerosLiveWeather : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UAerosWeatherSubsystem* InOwner);
	void Shutdown();
	void Tick(float DeltaTime);

	void Request(float InLatitude, float InLongitude, const FString& InPlaceName, bool bInApplyLocalTime, bool bInKeepUpdating);
	void Stop();

	UFUNCTION(BlueprintPure, Category = "Aeros|Live")
	bool IsActive() const { return bActive; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Live")
	bool IsBusy() const { return bBusy; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Live")
	FString GetStatus() const { return Status; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Live")
	FAerosRealObservation GetLastObservation() const { return Last; }

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Live")
	FAerosOnRealWeather OnReceived;

	UPROPERTY(BlueprintAssignable, Category = "Aeros|Live")
	FAerosOnRealWeatherError OnError;

	/** WMO weather interpretation code -> text. */
	static FString DescribeWeatherCode(int32 Code);

	/** Convert an observation to the simulation's synoptic state. */
	static FAerosSynopticState ToSynoptic(const FAerosRealObservation& Obs);

private:
	void SendForecast();
	void SendMarine();
	void OnForecast(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk);
	void OnMarine(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk);
	void Apply(bool bFirst);
	void Fail(const FString& Why);

	UPROPERTY(Transient) TObjectPtr<UAerosWeatherSubsystem> Owner;

	FAerosRealObservation Last;
	FString Status = TEXT("idle");
	float Latitude = 0.f, Longitude = 0.f;
	FString PlaceName;
	bool bApplyLocalTime = true;
	bool bKeepUpdating = true;
	bool bActive = false;
	bool bBusy = false;
	bool bFirstApply = true;
	float RefreshTimer = 0.f;

	// Upper-air values parsed with the forecast, applied after the marine request
	float PendingDewPoint850 = 0.f;
	FVector2D PendingWind850 = FVector2D::ZeroVector;
	FVector2D PendingWind500 = FVector2D::ZeroVector;
	float PendingAridity = 0.2f;
};
