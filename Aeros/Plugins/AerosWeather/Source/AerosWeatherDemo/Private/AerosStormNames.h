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

	/** Cloud genus in French (WMO names), species for cumulus: "Cumulus mediocris", "Altostratus"... */
	inline FString CloudGenusFr(EAerosCloudGenus G, int32 CumulusSpecies = -1)
	{
		switch (G)
		{
		case EAerosCloudGenus::Cumulus:
			return CumulusSpecies == 0 ? TEXT("Cumulus humilis") : (CumulusSpecies == 1 ? TEXT("Cumulus mediocris") : (CumulusSpecies == 2 ? TEXT("Cumulus congestus") : TEXT("Cumulus")));
		case EAerosCloudGenus::Stratus:       return TEXT("Stratus");
		case EAerosCloudGenus::Stratocumulus: return TEXT("Stratocumulus");
		case EAerosCloudGenus::Nimbostratus:  return TEXT("Nimbostratus");
		case EAerosCloudGenus::Altocumulus:   return TEXT("Altocumulus");
		case EAerosCloudGenus::Altostratus:   return TEXT("Altostratus");
		case EAerosCloudGenus::Cirrus:        return TEXT("Cirrus");
		case EAerosCloudGenus::Cirrostratus:  return TEXT("Cirrostratus");
		case EAerosCloudGenus::Cirrocumulus:  return TEXT("Cirrocumulus");
		case EAerosCloudGenus::Cumulonimbus:  return TEXT("Cumulonimbus");
		default:                              return FString();
		}
	}

	/** The clouds above a point, low to high: "Cumulus mediocris, Altocumulus, Cirrus" (empty when clear). */
	inline FString CloudsAboveFr(const FAerosWeatherSample& S)
	{
		TArray<FString> Parts;
		const EAerosCloudGenus Layers[3] = { S.LowCloud, S.MidCloud, S.HighCloud };
		for (EAerosCloudGenus G : Layers)
		{
			const FString Name = CloudGenusFr(G, S.CumulusSpecies);
			if (!Name.IsEmpty()) Parts.Add(Name);
		}
		return FString::Join(Parts, TEXT(", "));
	}

	/** One line for a marker or a caption: "Supercellule HP, mature" */
	inline FString LabelFr(const FAerosStormData& S)
	{
		return KindFr(S) + TEXT(", ") + StageFr(S);
	}
}
