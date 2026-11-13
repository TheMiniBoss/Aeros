// Copyright Aeros. All Rights Reserved.
//
// AAerosLightningFlash - one lightning discharge, procedurally generated and animated (local, cosmetic).
//
// Types (chosen deterministically from the strike sequence, so every machine sees the same bolt):
//   - Negative cloud-to-ground (~90 % of ground strikes): stepped leader that branches downward,
//     then 1-8 return strokes 40-120 ms apart along the main channel (the "flicker").
//   - Positive cloud-to-ground (~10 %): starts high in the anvil, several km away from the storm
//     core ("bolt from the blue"), one very bright stroke with a long continuing current.
//   - Intra-cloud: mostly a glow inside the cloud; sometimes a horizontal "anvil crawler" that
//     spreads slowly under the cloud base.
// The channel is a fractal path (midpoint displacement) with recursive branches. Brightness,
// width and light follow the peak current. Thunder arrives after distance / 343 m/s.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AerosTypes.h"
#include "AerosLightning.generated.h"

class UPointLightComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;

UENUM(BlueprintType)
enum class EAerosLightningType : uint8
{
	NegativeCloudToGround UMETA(DisplayName = "Negative cloud-to-ground"),
	PositiveCloudToGround UMETA(DisplayName = "Positive cloud-to-ground"),
	IntraCloud            UMETA(DisplayName = "Intra-cloud"),
};

UCLASS(NotPlaceable, Transient)
class AEROSWEATHER_API AAerosLightningFlash : public AActor
{
	GENERATED_BODY()

public:
	AAerosLightningFlash();
	virtual void Tick(float DeltaSeconds) override;

	void Fire(const FAerosLightningStrike& Strike, const FVector& ViewerLocation);

	EAerosLightningType GetLightningType() const { return Type; }

	/** Channel brightness 0..~1.6 at the current age (leader, return strokes, continuing current). */
	float GetBrightness() const;

	UPROPERTY(VisibleAnywhere, Category = "Aeros")
	TObjectPtr<UPointLightComponent> Light;

	/** Visible channel (fallback when no LightningSystem Niagara asset is set). One instance per segment,
	 *  custom data 0 = segment brightness (main channel 1, branches dimmer). */
	UPROPERTY(VisibleAnywhere, Category = "Aeros")
	TObjectPtr<UInstancedStaticMeshComponent> Bolt;

private:
	struct FSegment
	{
		FVector A, B;
		float Width = 100.f;   // cm
		float Arrival = 0.f;   // s after the start: when the leader reaches the end of this segment
		float Glow = 1.f;      // brightness factor (branch level)
		int32 Level = 0;       // 0 = main channel
	};

	void BuildChannel(const FVector& Start, const FVector& End, const FVector& ViewerLocation, FRandomStream& Rand);
	void GrowBranch(const TArray<FVector>& Path, int32 Level, float Width, float StartArrival, float Speed,
		const FVector& Gravity, FRandomStream& Rand);
	void RevealSegments();
	float StrokeBrightness(float T) const;

	EAerosLightningType Type = EAerosLightningType::NegativeCloudToGround;
	TArray<FSegment> Segments;  // sorted by arrival time
	int32 NumShown = 0;
	bool bBranchesDimmed = false;
	bool bVisibleChannel = false;

	TArray<float> StrokeTimes;   // s after the start
	TArray<float> StrokeAmps;
	float LeaderDuration = 0.04f;
	float ContinuingCurrent = 0.f;   // amplitude
	float ContinuingUntil = 0.f;     // s
	float EndTime = 0.5f;
	float BaseIntensity = 40.f;      // emissive scale from the peak current
	float FlashWeight = 1.f;         // how much this flash lights the clouds / screen at the viewer

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BoltMID;

	float Age = 0.f;
	float Peak = 0.f;
	float ThunderDelay = -1.f;
	float ThunderVolume = 1.f;
	bool bThunderPlayed = true;
};
