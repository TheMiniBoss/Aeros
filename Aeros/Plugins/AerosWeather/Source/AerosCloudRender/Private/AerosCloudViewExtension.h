// Copyright Aeros. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "SceneViewExtension.h"

struct FScreenPassTexture;
struct FPostProcessMaterialInputs;

/**
 * Spike: draws a synthetic cloud field with our own ray march, before depth of field.
 * Enabled with r.AerosClouds 1. The pass traces at half resolution with a per-frame
 * jitter, accumulates over frames with a reprojection that follows the camera AND the
 * wind, then upsamples with depth weights. Lightning light is added fresh each frame
 * (never stored in the history), so a flash can't leave a dither trail.
 */
class FAerosCloudViewExtension : public FSceneViewExtensionBase
{
public:
	FAerosCloudViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	FScreenPassTexture Render_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);

	// Render-thread state (one view is enough for the spike).
	TRefCountPtr<IPooledRenderTarget> HistoryColor;
	TRefCountPtr<IPooledRenderTarget> HistoryDepth;
	FMatrix PrevViewProj = FMatrix::Identity;
	double PrevTime = -1.0;
	uint32 FrameIndex = 0;
};
