// Copyright Aeros. All Rights Reserved.
//
// Aeros::TFlashList - pure C++ (no UObject): the N brightest lightning flashes reported this frame,
// sorted by intensity (brightest first). The cloud renderer lights the clouds around each of them.

#pragma once

#include "CoreMinimal.h"

namespace Aeros
{
	struct FFlashEntry
	{
		float Intensity = 0.f;
		FVector Location = FVector::ZeroVector;   // world cm
	};

	template <int N>
	struct TFlashList
	{
		/** Keeps the flash if it is among the N brightest; zero or negative intensities are ignored. */
		void Add(float Intensity, const FVector& Location)
		{
			if (!(Intensity > 0.f) || (Count == N && Intensity <= Entries[N - 1].Intensity))
			{
				return;
			}
			int Pos = Count < N ? Count : N - 1;
			while (Pos > 0 && Entries[Pos - 1].Intensity < Intensity)
			{
				Entries[Pos] = Entries[Pos - 1];
				--Pos;
			}
			Entries[Pos].Intensity = Intensity;
			Entries[Pos].Location = Location;
			if (Count < N)
			{
				++Count;
			}
		}

		void Reset() { Count = 0; }
		int Num() const { return Count; }
		const FFlashEntry& operator[](int I) const { return Entries[I]; }
		const FFlashEntry* GetData() const { return Entries; }

	private:
		FFlashEntry Entries[N];
		int Count = 0;
	};
}
