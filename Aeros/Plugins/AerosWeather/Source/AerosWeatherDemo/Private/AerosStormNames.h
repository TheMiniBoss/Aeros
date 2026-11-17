// Copyright Aeros. All Rights Reserved.
// French names of a storm's kind and life stage, for the demo HUDs (ASCII source: accents as \u escapes).

#pragma once

#include "CoreMinimal.h"
#include "AerosTypes.h"

namespace AerosStormNames
{
	/** "Supercellule classique", "Supercellule HP", "Orage multicellulaire"... */
	inline FString KindFr(const FAerosStormData& S)
	{
		if (S.Type == EAerosStormType::Supercell)
		{
			switch (S.SupercellType)
			{
			case EAerosSupercellType::Classic:    return TEXT("Supercellule classique");
			case EAerosSupercellType::HighPrecip: return TEXT("Supercellule HP");
			case EAerosSupercellType::LowPrecip:  return TEXT("Supercellule LP");
			case EAerosSupercellType::LowTopped:  return TEXT("Mini-supercellule");
			default:                              return TEXT("Supercellule");
			}
		}
		return S.Type == EAerosStormType::Multicell ? TEXT("Orage multicellulaire") : TEXT("Orage");
	}

	/** "en formation", "mature", "en fin de vie" */
	inline FString StageFr(const FAerosStormData& S)
	{
		switch (S.Stage)
		{
		case EAerosStormStage::Developing:  return TEXT("en formation");
		case EAerosStormStage::Dissipating: return TEXT("en fin de vie");
		default:                            return TEXT("mature");
		}
	}

	/** One line for a marker or a caption: "Supercellule HP, mature" */
	inline FString LabelFr(const FAerosStormData& S)
	{
		return KindFr(S) + TEXT(", ") + StageFr(S);
	}
}
