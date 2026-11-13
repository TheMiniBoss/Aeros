// Copyright Aeros. All Rights Reserved.

#include "AerosWeatherLibrary.h"
#include "AerosWeatherSubsystem.h"
#include "AerosWeatherRadar.h"
#include "Core/AerosPhysics.h"

#define LOCTEXT_NAMESPACE "AerosWeather"

UAerosWeatherSubsystem* UAerosWeatherLibrary::GetAerosWeather(const UObject* WorldContextObject)
{
	return UAerosWeatherSubsystem::Get(WorldContextObject);
}

FAerosWeatherSample UAerosWeatherLibrary::GetWeatherAt(const UObject* WorldContextObject, FVector WorldLocation)
{
	const UAerosWeatherSubsystem* Sub = UAerosWeatherSubsystem::Get(WorldContextObject);
	return Sub ? Sub->GetWeatherAtLocation(WorldLocation) : FAerosWeatherSample();
}

FText UAerosWeatherLibrary::PhenomenonToText(EAerosPhenomenon Phenomenon)
{
	return StaticEnum<EAerosPhenomenon>()->GetDisplayNameTextByValue(int64(Phenomenon));
}

FText UAerosWeatherLibrary::PrecipitationTypeToText(EAerosPrecipType Type)
{
	return StaticEnum<EAerosPrecipType>()->GetDisplayNameTextByValue(int64(Type));
}

FText UAerosWeatherLibrary::SeasonToText(EAerosSeason Season)
{
	return StaticEnum<EAerosSeason>()->GetDisplayNameTextByValue(int64(Season));
}

FText UAerosWeatherLibrary::ScenarioToText(EAerosScenario Scenario)
{
	return StaticEnum<EAerosScenario>()->GetDisplayNameTextByValue(int64(Scenario));
}

int32 UAerosWeatherLibrary::EnhancedFujita(float WindSpeed)
{
	return Aeros::Phys::EFScale(WindSpeed);
}

int32 UAerosWeatherLibrary::SaffirSimpson(float WindSpeed)
{
	return Aeros::Phys::SaffirSimpson(WindSpeed);
}

FLinearColor UAerosWeatherLibrary::ReflectivityColor(float DBZ)
{
	return AAerosWeatherRadar::ReflectivityColor(DBZ);
}

bool UAerosWeatherLibrary::IsSevere(const FAerosWeatherSample& S)
{
	switch (S.Phenomenon)
	{
	case EAerosPhenomenon::SevereThunderstorm:
	case EAerosPhenomenon::Supercell:
	case EAerosPhenomenon::Tornado:
	case EAerosPhenomenon::Hurricane:
	case EAerosPhenomenon::Blizzard:
		return true;
	default:
		return S.HailDiameter >= 25.f || S.WindSpeed >= 25.f || S.PrecipitationRate >= 50.f;
	}
}

FString UAerosWeatherLibrary::DescribeWeather(const FAerosWeatherSample& S)
{
	FString Out = FString::Printf(TEXT("%s, %.0f C"), *PhenomenonToText(S.Phenomenon).ToString(), S.Temperature);
	if (S.PrecipitationRate > 0.05f)
	{
		Out += FString::Printf(TEXT(", %.1f mm/h %s"), S.PrecipitationRate, *PrecipitationTypeToText(S.PrecipitationType).ToString().ToLower());
	}
	if (S.HailDiameter > 0.f) Out += FString::Printf(TEXT(", hail %.0f mm"), S.HailDiameter);
	Out += FString::Printf(TEXT(", wind %.0f km/h"), S.WindSpeed * 3.6f);
	return Out;
}

#undef LOCTEXT_NAMESPACE
