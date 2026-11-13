// Copyright Aeros. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Stats/Stats.h"

AEROSWEATHER_API DECLARE_LOG_CATEGORY_EXTERN(LogAeros, Log, All);

DECLARE_STATS_GROUP(TEXT("Aeros Weather"), STATGROUP_Aeros, STATCAT_Advanced);

class FAerosWeatherModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
