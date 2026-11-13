// Copyright Aeros. All Rights Reserved.

#include "AerosLiveWeather.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherSettings.h"
#include "AerosWeatherModule.h"
#include "Core/AerosPhysics.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	double NumberOr(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Fallback)
	{
		double V;
		return (Obj.IsValid() && Obj->TryGetNumberField(Field, V)) ? V : Fallback;
	}

	/** Value of an hourly array at Index, or Fallback (missing / null). */
	double HourlyAt(const TSharedPtr<FJsonObject>& Hourly, const TCHAR* Field, int32 Index, double Fallback)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Hourly.IsValid() || !Hourly->TryGetArrayField(Field, Arr) || !Arr->IsValidIndex(Index)) return Fallback;
		double V;
		return (*Arr)[Index].IsValid() && (*Arr)[Index]->TryGetNumber(V) ? V : Fallback;
	}

	/** Meteorological "from" direction + speed -> vector toward (x east, y north). */
	FVector2D WindVector(double Speed, double FromDeg)
	{
		const double R = FMath::DegreesToRadians(FromDeg);
		return FVector2D(-Speed * FMath::Sin(R), -Speed * FMath::Cos(R));
	}

	float DewPointFromRH(float Tc, float RHPercent)
	{
		const float E = FMath::Max(RHPercent, 1.f) * 0.01f * Aeros::Phys::SatVaporPressure(Tc);
		const float L = FMath::Loge(E / 6.112f);
		return 243.5f * L / (17.67f - L);
	}
}

void UAerosLiveWeather::Initialize(UAerosWeatherSubsystem* InOwner)
{
	Owner = InOwner;
}

void UAerosLiveWeather::Shutdown()
{
	bActive = false;
	Owner = nullptr;
}

void UAerosLiveWeather::Request(float InLatitude, float InLongitude, const FString& InPlaceName, bool bInApplyLocalTime, bool bInKeepUpdating)
{
	Latitude = FMath::Clamp(InLatitude, -89.f, 89.f);
	Longitude = FMath::Wrap(InLongitude, -180.f, 180.f);
	PlaceName = InPlaceName;
	bApplyLocalTime = bInApplyLocalTime;
	bKeepUpdating = bInKeepUpdating;
	bActive = true;
	bFirstApply = true;
	RefreshTimer = 0.f;
	SendForecast();
}

void UAerosLiveWeather::Stop()
{
	bActive = false;
	Status = TEXT("stopped");
}

void UAerosLiveWeather::Tick(float DeltaTime)
{
	if (!bActive || !bKeepUpdating || bBusy) return;
	RefreshTimer += DeltaTime;
	if (RefreshTimer >= UAerosWeatherSettings::Get()->LiveRefreshMinutes * 60.f)
	{
		RefreshTimer = 0.f;
		SendForecast();
	}
}

void UAerosLiveWeather::Fail(const FString& Why)
{
	bBusy = false;
	Status = TEXT("error: ") + Why;
	UE_LOG(LogAeros, Warning, TEXT("Aeros live weather: %s"), *Why);
	OnError.Broadcast(Why);
}

void UAerosLiveWeather::SendForecast()
{
	bBusy = true;
	Status = FString::Printf(TEXT("requesting %s..."), *PlaceName);
	const FString Url = FString::Printf(
		TEXT("%s?latitude=%.4f&longitude=%.4f")
		TEXT("&current=temperature_2m,relative_humidity_2m,dew_point_2m,pressure_msl,wind_speed_10m,wind_direction_10m,precipitation,cloud_cover,weather_code,cape,soil_moisture_0_to_1cm")
		TEXT("&hourly=temperature_850hPa,relative_humidity_850hPa,wind_speed_850hPa,wind_direction_850hPa,temperature_500hPa,wind_speed_500hPa,wind_direction_500hPa")
		TEXT("&wind_speed_unit=ms&timezone=auto&forecast_days=1"),
		*UAerosWeatherSettings::Get()->OpenMeteoForecastURL, Latitude, Longitude);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Req->SetTimeout(20.f);
	Req->OnProcessRequestComplete().BindUObject(this, &UAerosLiveWeather::OnForecast);
	Req->ProcessRequest();
}

void UAerosLiveWeather::OnForecast(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk)
{
	if (!bOk || !Resp.IsValid()) { Fail(TEXT("no response from Open-Meteo (network?)")); return; }
	if (Resp->GetResponseCode() != 200) { Fail(FString::Printf(TEXT("HTTP %d: %s"), Resp->GetResponseCode(), *Resp->GetContentAsString().Left(200))); return; }

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { Fail(TEXT("invalid JSON")); return; }

	const TSharedPtr<FJsonObject>* CurPtr = nullptr;
	if (!Root->TryGetObjectField(TEXT("current"), CurPtr)) { Fail(TEXT("no 'current' block")); return; }
	const TSharedPtr<FJsonObject> Cur = *CurPtr;
	const TSharedPtr<FJsonObject>* HourlyPtr = nullptr;
	const TSharedPtr<FJsonObject> Hourly = Root->TryGetObjectField(TEXT("hourly"), HourlyPtr) ? *HourlyPtr : nullptr;

	FAerosRealObservation O;
	O.PlaceName = PlaceName;
	O.Latitude = Latitude;
	O.Longitude = Longitude;
	Cur->TryGetStringField(TEXT("time"), O.LocalTime);
	O.Temperature = float(NumberOr(Cur, TEXT("temperature_2m"), 15.0));
	O.RelativeHumidity = float(NumberOr(Cur, TEXT("relative_humidity_2m"), 60.0));
	O.DewPoint = float(NumberOr(Cur, TEXT("dew_point_2m"), DewPointFromRH(O.Temperature, O.RelativeHumidity)));
	O.PressureMSL = float(NumberOr(Cur, TEXT("pressure_msl"), 1013.0));
	O.WindSpeed = float(NumberOr(Cur, TEXT("wind_speed_10m"), 3.0));
	O.WindDirection = float(NumberOr(Cur, TEXT("wind_direction_10m"), 270.0));
	O.Precipitation = float(NumberOr(Cur, TEXT("precipitation"), 0.0));
	O.CloudCover = float(NumberOr(Cur, TEXT("cloud_cover"), 30.0));
	O.WeatherCode = int32(NumberOr(Cur, TEXT("weather_code"), 0.0));
	O.CAPE = float(NumberOr(Cur, TEXT("cape"), 0.0));
	const double SoilMoisture = NumberOr(Cur, TEXT("soil_moisture_0_to_1cm"), -1.0);

	// Hourly upper-air values: pick the hour of the current observation
	int32 Index = 0;
	const TArray<TSharedPtr<FJsonValue>>* Times = nullptr;
	if (Hourly.IsValid() && Hourly->TryGetArrayField(TEXT("time"), Times))
	{
		const FString Hour = O.LocalTime.Left(13); // "YYYY-MM-DDTHH"
		for (int32 K = 0; K < Times->Num(); ++K)
		{
			if ((*Times)[K]->AsString().StartsWith(Hour)) { Index = K; break; }
		}
	}
	O.Temperature850 = float(HourlyAt(Hourly, TEXT("temperature_850hPa"), Index, O.Temperature - 9.0));
	O.Temperature500 = float(HourlyAt(Hourly, TEXT("temperature_500hPa"), Index, O.Temperature - 33.0));
	const float RH850 = float(HourlyAt(Hourly, TEXT("relative_humidity_850hPa"), Index, 50.0));
	const FVector2D W850 = WindVector(HourlyAt(Hourly, TEXT("wind_speed_850hPa"), Index, O.WindSpeed * 2.0), HourlyAt(Hourly, TEXT("wind_direction_850hPa"), Index, O.WindDirection));
	const FVector2D W500 = WindVector(HourlyAt(Hourly, TEXT("wind_speed_500hPa"), Index, O.WindSpeed * 4.0), HourlyAt(Hourly, TEXT("wind_direction_500hPa"), Index, O.WindDirection));
	O.Description = DescribeWeatherCode(O.WeatherCode);

	Last = O;
	// Stash the extra fields used by ToSynoptic in the struct-independent cache
	PendingDewPoint850 = DewPointFromRH(O.Temperature850, RH850);
	PendingWind850 = W850;
	PendingWind500 = W500;
	PendingAridity = SoilMoisture >= 0.0 ? FMath::Clamp(1.f - float(SoilMoisture) / 0.25f, 0.f, 1.f) * 0.8f : 0.2f;

	SendMarine();
}

void UAerosLiveWeather::SendMarine()
{
	const FString Url = FString::Printf(TEXT("%s?latitude=%.4f&longitude=%.4f&current=sea_surface_temperature&timezone=auto"),
		*UAerosWeatherSettings::Get()->OpenMeteoMarineURL, Latitude, Longitude);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetTimeout(15.f);
	Req->OnProcessRequestComplete().BindUObject(this, &UAerosLiveWeather::OnMarine);
	Req->ProcessRequest();
}

void UAerosLiveWeather::OnMarine(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk)
{
	// Optional: inland places have no sea temperature, that is fine
	if (bOk && Resp.IsValid() && Resp->GetResponseCode() == 200)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
		const TSharedPtr<FJsonObject>* Cur = nullptr;
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() && Root->TryGetObjectField(TEXT("current"), Cur))
		{
			Last.SeaSurfaceTemperature = float(NumberOr(*Cur, TEXT("sea_surface_temperature"), -99.0));
		}
	}
	Apply(bFirstApply);
	bFirstApply = false;
}

FAerosSynopticState UAerosLiveWeather::ToSynoptic(const FAerosRealObservation& O)
{
	FAerosSynopticState S;
	S.SurfaceTemperature = O.Temperature;
	S.SurfaceDewPoint = FMath::Min(O.DewPoint, O.Temperature);
	S.Temperature850 = O.Temperature850;
	S.Temperature500 = O.Temperature500;
	S.WindSurface = FVector2D(-O.WindSpeed * FMath::Sin(FMath::DegreesToRadians(O.WindDirection)), -O.WindSpeed * FMath::Cos(FMath::DegreesToRadians(O.WindDirection)));
	S.SeaSurfaceTemperature = O.SeaSurfaceTemperature > -50.f ? O.SeaSurfaceTemperature : FMath::Clamp(O.Temperature + 2.f, -1.f, 31.f);

	// Large-scale vertical motion from what is observed (WMO weather codes)
	const int32 C = O.WeatherCode;
	if ((C >= 51 && C <= 67) || (C >= 71 && C <= 77)) S.LargeScaleAscent = 0.02f + 0.01f * FMath::Min(O.Precipitation, 5.f); // drizzle, rain, snow
	else if (C <= 1) S.LargeScaleAscent = -0.01f;                                                                            // clear: subsidence
	else if (C <= 3) S.LargeScaleAscent = 0.01f * O.CloudCover / 100.f - 0.003f;
	else if (C == 45 || C == 48) S.LargeScaleAscent = -0.005f;                                                               // fog: stable
	else S.LargeScaleAscent = 0.005f;                                                                                        // showers / thunder
	return S;
}

void UAerosLiveWeather::Apply(bool bFirst)
{
	bBusy = false;
	if (!Owner || !bActive) return;

	FAerosSynopticState S = ToSynoptic(Last);
	S.DewPoint850 = FMath::Min(PendingDewPoint850, Last.Temperature850);
	S.Wind850 = PendingWind850;
	S.Wind500 = PendingWind500;
	S.Aridity = PendingAridity;

	Owner->SetLatitude(Latitude);
	Owner->SetCustomSynoptic(S);

	if (bApplyLocalTime && Last.LocalTime.Len() >= 16)
	{
		// "YYYY-MM-DDTHH:MM" -> day of year + hour (local solar time is close enough to local clock time)
		const int32 Y = FCString::Atoi(*Last.LocalTime.Mid(0, 4)), Mo = FCString::Atoi(*Last.LocalTime.Mid(5, 2)), D = FCString::Atoi(*Last.LocalTime.Mid(8, 2));
		const float H = FCString::Atoi(*Last.LocalTime.Mid(11, 2)) + FCString::Atoi(*Last.LocalTime.Mid(14, 2)) / 60.f;
		static const int32 Start[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
		const int32 Doy = FMath::Clamp(Start[FMath::Clamp(Mo, 1, 12) - 1] + D + ((Mo > 2 && FDateTime::IsLeapYear(Y)) ? 1 : 0), 1, 365);
		if (bFirst || FMath::Abs(Owner->GetDateTime().Hour - H) > 0.5f) Owner->SetDateTime(Doy, H);
	}

	if (bFirst)
	{
		// Warm start: the physics needs time to build clouds; seed what is already happening
		const FVector Here = Owner->GetViewerLocation();
		if (Last.Precipitation > 0.05f) Owner->InjectRain(Here, 80.f, FMath::Min(Last.Precipitation * 0.5f, 20.f));
		if (Last.WeatherCode >= 95) // thunderstorm reported
		{
			for (int32 K = 0; K < 3; ++K) Owner->InjectThermalBubble(Here + FVector(FMath::FRandRange(-1.f, 1.f), FMath::FRandRange(-1.f, 1.f), 0.f) * 3000000.0, 6.f, 3.f, 2.f);
		}
		if (bKeepUpdating) Owner->SetTimeScale(1.f); // live: the simulation follows the real clock
	}

	Status = FString::Printf(TEXT("%s: %s, %.0f C, wind %.0f km/h (%s)"), *PlaceName, *Last.Description, Last.Temperature, Last.WindSpeed * 3.6f, *Last.LocalTime);
	UE_LOG(LogAeros, Log, TEXT("Aeros live weather applied: %s"), *Status);
	OnReceived.Broadcast(Last);
}

FString UAerosLiveWeather::DescribeWeatherCode(int32 Code)
{
	switch (Code)
	{
	case 0: return TEXT("Clear sky");
	case 1: return TEXT("Mainly clear");
	case 2: return TEXT("Partly cloudy");
	case 3: return TEXT("Overcast");
	case 45: case 48: return TEXT("Fog");
	case 51: case 53: case 55: return TEXT("Drizzle");
	case 56: case 57: return TEXT("Freezing drizzle");
	case 61: case 63: case 65: return TEXT("Rain");
	case 66: case 67: return TEXT("Freezing rain");
	case 71: case 73: case 75: return TEXT("Snow");
	case 77: return TEXT("Snow grains");
	case 80: case 81: case 82: return TEXT("Rain showers");
	case 85: case 86: return TEXT("Snow showers");
	case 95: return TEXT("Thunderstorm");
	case 96: case 99: return TEXT("Thunderstorm with hail");
	default: return FString::Printf(TEXT("WMO %d"), Code);
	}
}
