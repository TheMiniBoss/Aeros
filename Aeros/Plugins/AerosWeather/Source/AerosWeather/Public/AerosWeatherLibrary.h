// Copyright Aeros. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AerosTypes.h"
#include "AerosWeatherLibrary.generated.h"

class UAerosWeatherSubsystem;

UCLASS()
class AEROSWEATHER_API UAerosWeatherLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Aeros", meta = (WorldContext = "WorldContextObject"))
	static UAerosWeatherSubsystem* GetAerosWeather(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Aeros", meta = (WorldContext = "WorldContextObject"))
	static FAerosWeatherSample GetWeatherAt(const UObject* WorldContextObject, FVector WorldLocation);

	UFUNCTION(BlueprintPure, Category = "Aeros|Text")
	static FText PhenomenonToText(EAerosPhenomenon Phenomenon);

	UFUNCTION(BlueprintPure, Category = "Aeros|Text")
	static FText PrecipitationTypeToText(EAerosPrecipType Type);

	UFUNCTION(BlueprintPure, Category = "Aeros|Text")
	static FText SeasonToText(EAerosSeason Season);

	UFUNCTION(BlueprintPure, Category = "Aeros|Text")
	static FText ScenarioToText(EAerosScenario Scenario);

	/** Enhanced Fujita rating (0-5) from a 3 s gust in m/s. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Scales")
	static int32 EnhancedFujita(float WindSpeed);

	/** Saffir-Simpson category (0 = tropical storm or weaker) from sustained wind in m/s. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Scales")
	static int32 SaffirSimpson(float WindSpeed);

	/** Radar colour for a reflectivity in dBZ. */
	UFUNCTION(BlueprintPure, Category = "Aeros|Scales")
	static FLinearColor ReflectivityColor(float DBZ);

	UFUNCTION(BlueprintPure, Category = "Aeros|Units")
	static float CelsiusToFahrenheit(float Celsius) { return Celsius * 1.8f + 32.f; }

	UFUNCTION(BlueprintPure, Category = "Aeros|Units")
	static float MetersPerSecondToKmh(float Speed) { return Speed * 3.6f; }

	/** True for weather a player should take shelter from. */
	UFUNCTION(BlueprintPure, Category = "Aeros")
	static bool IsSevere(const FAerosWeatherSample& Sample);

	/** Readable one-line summary ("Thunderstorm, 27 C, 45 mm/h rain, hail 25 mm"). */
	UFUNCTION(BlueprintPure, Category = "Aeros|Text")
	static FString DescribeWeather(const FAerosWeatherSample& Sample);
};
