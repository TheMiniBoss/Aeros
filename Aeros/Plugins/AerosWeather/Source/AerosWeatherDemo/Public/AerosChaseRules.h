// Copyright Aeros. All Rights Reserved.
//
// AerosChase - pure C++ (no UObject): the rules of the storm chaser game (photo points, vehicle damage,
// ranks). Kept apart from the actors so they are unit-tested in Tests/Standalone.

#pragma once

#include "CoreMinimal.h"
#include <cmath>

namespace AerosChase
{
	enum class ESubject : uint8
	{
		Tornado,
		WallCloud,
		Lightning,
		Supercell,
	};

	struct FPhotoInput
	{
		ESubject Subject = ESubject::Tornado;
		int32 EF = 0;
		float DistanceKm = 10.f;
		float CenterOffset = 0.f;    // 0 = screen centre, 1 = edge of the frame, > 1 = off screen
		float VisibilityKm = 30.f;   // at the photographer (rain, fog, dust)
		float Brightness = 1.f;      // lightning: flash strength 0-1 at the shot
	};

	inline float Smooth01(float X)
	{
		X = FMath::Clamp(X, 0.f, 1.f);
		return X * X * (3.f - 2.f * X);
	}

	/** Closer is better (x2 at the funnel, x1 near 3 km, x0.4 far), nothing beyond 20 km. */
	inline float CloseRangeFactor(float DistanceKm)
	{
		const float D = FMath::Max(DistanceKm, 0.f);
		return (0.4f + 1.6f * std::exp(-D / 2.f)) * (1.f - Smooth01((D - 15.f) / 5.f));
	}

	/** Full value inside the central quarter of the frame, nothing at the edge or off screen. */
	inline float FramingFactor(float CenterOffset)
	{
		return CenterOffset >= 1.f ? 0.f : 1.f - Smooth01((CenterOffset - 0.25f) / 0.75f);
	}

	/** Rain or fog between the camera and the subject. */
	inline float VisibilityFactor(float VisibilityKm, float DistanceKm)
	{
		return FMath::Clamp(VisibilityKm / FMath::Max(DistanceKm, 0.1f), 0.f, 1.f);
	}

	inline int32 PhotoPoints(const FPhotoInput& I)
	{
		const float Frame = FramingFactor(I.CenterOffset);
		const float Vis = VisibilityFactor(I.VisibilityKm, I.DistanceKm);
		float Points = 0.f;
		switch (I.Subject)
		{
		case ESubject::Tornado:
			Points = 100.f * float(FMath::Clamp(I.EF, 0, 5) + 1) * CloseRangeFactor(I.DistanceKm) * Frame * Vis;
			break;
		case ESubject::WallCloud:
			Points = 60.f * CloseRangeFactor(I.DistanceKm) * Frame * Vis;
			break;
		case ESubject::Lightning:
			Points = 40.f * FMath::Clamp(I.Brightness, 0.f, 1.f) * (1.f - Smooth01((I.DistanceKm - 10.f) / 15.f)) * Frame * Vis;
			break;
		case ESubject::Supercell:
			// The whole storm in the frame: from 15-60 km away
			Points = 80.f * Smooth01((I.DistanceKm - 10.f) / 10.f) * (1.f - Smooth01((I.DistanceKm - 60.f) / 20.f)) * Frame
				* FMath::Clamp(I.VisibilityKm / 15.f, 0.f, 1.f);
			break;
		}
		return int32(Points + 0.5f);
	}

	/** Damage to the vehicle per second (health 100): tornado wind above 35 m/s, hail above 20 mm, flood above 0.4 m. */
	inline float DamagePerSecond(float TornadoWindMs, float HailMm, float FloodDepthM)
	{
		float D = 0.f;
		if (TornadoWindMs > 35.f) D += (TornadoWindMs - 35.f) * 0.8f;
		if (HailMm > 20.f) D += (HailMm - 20.f) * 0.4f;
		if (FloodDepthM > 0.4f) D += 5.f + (FloodDepthM - 0.4f) * 10.f;
		return D;
	}

	/** One-off damage from a cloud-to-ground strike at this distance (m). */
	inline float LightningDamage(float DistanceM)
	{
		return DistanceM >= 300.f ? 0.f : 40.f * (1.f - DistanceM / 300.f);
	}

	/** Points for driving through a hail core (once per storm). */
	inline int32 HailPoints(float HailMm)
	{
		return HailMm < 20.f ? 0 : int32(15.f * HailMm / 10.f + 0.5f);
	}

	inline int32 RespawnPenalty(int32 Score)
	{
		return FMath::Max(Score, 0) / 4;
	}

	/** 0 beginner, 1 spotter, 2 chaser, 3 veteran, 4 legend. */
	inline int32 RankIndex(int32 Score)
	{
		return Score >= 6000 ? 4 : Score >= 3000 ? 3 : Score >= 1200 ? 2 : Score >= 400 ? 1 : 0;
	}
}
