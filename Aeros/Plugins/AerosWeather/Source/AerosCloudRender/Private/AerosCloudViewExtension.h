// Copyright Aeros. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "SceneViewExtension.h"
#include "AerosCloudRender.h"

struct FScreenPassTexture;
struct FAerosCloudNoiseData;
struct FPostProcessMaterialInputs;

/** Temporal history of one view (each player, split screen half or scene capture has its own). */
struct FAerosCloudHistory
{
	TRefCountPtr<IPooledRenderTarget> Color;
	TRefCountPtr<IPooledRenderTarget> Depth;
	FMatrix ViewProj = FMatrix::Identity;
	double Time = -1.0;
	FVector ViewOrigin = FVector::ZeroVector;
	float Blend = 1.f;
	uint32 LastFrame = 0;
};

/**
 * Draws the simulated clouds with our own ray march, before depth of field.
 * Enabled with r.AerosClouds 1. The pass traces at half resolution with a per-frame
 * jitter, accumulates over frames with a reprojection that follows the camera AND the
 * storm motion, then upsamples with depth weights. Lightning light is added fresh each
 * frame (never stored in the history), so a flash can't leave a dither trail.
 */
class FAerosCloudViewExtension : public FSceneViewExtensionBase
{
public:
	FAerosCloudViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	/** Render thread: latest simulation data (see AerosCloudRender::SetParams). */
	void SetParams_RenderThread(const FAerosCloudRenderParams& InParams) { RTParams = InParams; }

	/** The module registers the live extension here so SetParams can reach it. */
	static void SetGlobal(const TSharedPtr<FAerosCloudViewExtension, ESPMode::ThreadSafe>& Extension);

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	FScreenPassTexture Render_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);

	FAerosCloudRenderParams RTParams;

	// Generated noise: built on a worker at startup, uploaded once on the render thread.
	TSharedPtr<FAerosCloudNoiseData, ESPMode::ThreadSafe> NoiseData;
	TRefCountPtr<IPooledRenderTarget> ShapeNoiseRT;
	TRefCountPtr<IPooledRenderTarget> DetailNoiseRT;
	bool UploadNoise_RenderThread(FRHICommandListImmediate& RHICmdList);

	// Render-thread state: one history per view, keyed by its view state (0 without one).
	// Entries unused for 120 frames are dropped. Held by pointer: the graph extracts into them,
	// and a second view added in the same graph must not move the first one's history.
	TMap<uint32, TUniquePtr<FAerosCloudHistory>> Histories;
	uint32 FrameIndex = 0;
};
