// Copyright Aeros. All Rights Reserved.

#include "AerosCloudViewExtension.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "SceneTexturesConfig.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"

namespace AerosCloudRenderInternal
{
	TAutoConsoleVariable<int32> CVarEnable(TEXT("r.AerosClouds"), 0,
		TEXT("Aeros custom cloud renderer (spike). 0 off, 1 on."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarWind(TEXT("r.AerosClouds.Wind"), 10.f,
		TEXT("Cloud drift speed in m/s (600 ~ a x60 time-lapse of a 10 m/s wind)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarFlash(TEXT("r.AerosClouds.Flash"), 0.f,
		TEXT("Test lightning: flashes per second inside the test tower (0 off)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarBlend(TEXT("r.AerosClouds.HistoryBlend"), 0.1f,
		TEXT("Weight of the new frame in the temporal accumulation (1 = no history)."), ECVF_RenderThreadSafe);

	BEGIN_SHADER_PARAMETER_STRUCT(FCommonParameters, )
		SHADER_PARAMETER(FVector3f, CameraKm)
		SHADER_PARAMETER(FVector3f, WindKmS)
		SHADER_PARAMETER(float, TimeS)
		SHADER_PARAMETER(uint32, FrameIndex)
		SHADER_PARAMETER(FIntPoint, ViewMin)
		SHADER_PARAMETER(FIntPoint, HalfSize)
		SHADER_PARAMETER(FIntPoint, FullSize)
	END_SHADER_PARAMETER_STRUCT()

	class FTraceCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FTraceCS);
		SHADER_USE_PARAMETER_STRUCT(FTraceCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_STRUCT_INCLUDE(FCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutColor)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutDepth)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FTraceCS, "/Plugin/AerosWeather/Private/AerosClouds.usf", "TraceCS", SF_Compute);

	class FTemporalCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FTemporalCS);
		SHADER_USE_PARAMETER_STRUCT(FTemporalCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_STRUCT_INCLUDE(FCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CurColor)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CurDepth)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HistColor)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HistDepth)
			SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
			SHADER_PARAMETER(FMatrix44f, TranslatedToPrevClip)
			SHADER_PARAMETER(float, DeltaTime)
			SHADER_PARAMETER(float, NewWeight)
			SHADER_PARAMETER(uint32, bHistoryValid)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutColor)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutDepth)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FTemporalCS, "/Plugin/AerosWeather/Private/AerosClouds.usf", "TemporalCS", SF_Compute);

	class FCompositeCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FCompositeCS);
		SHADER_USE_PARAMETER_STRUCT(FCompositeCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_STRUCT_INCLUDE(FCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CloudColor)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CloudDepth)
			SHADER_PARAMETER(FVector3f, FlashKm)
			SHADER_PARAMETER(FVector3f, FlashColor)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSceneColor)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FCompositeCS, "/Plugin/AerosWeather/Private/AerosClouds.usf", "CompositeCS", SF_Compute);

	// Deterministic test flash: on for 0.12 s, Rate times per second, at a hashed spot in the tower.
	bool TestFlash(double Time, float Rate, FVector3f& OutKm)
	{
		if (Rate <= 0.f)
		{
			return false;
		}
		const double Period = 1.0 / Rate;
		const int64 Slot = (int64)FMath::FloorToDouble(Time / Period);
		if (Time - Slot * Period > 0.12)
		{
			return false;
		}
		const uint32 H = (uint32)(Slot * 2654435761u);
		OutKm = FVector3f(8.f + ((H & 255) / 255.f - 0.5f) * 4.f, (((H >> 8) & 255) / 255.f - 0.5f) * 4.f, 4.f + ((H >> 16) & 255) / 255.f * 5.f);
		return true;
	}
}

FAerosCloudViewExtension::FAerosCloudViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FAerosCloudViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return AerosCloudRenderInternal::CVarEnable.GetValueOnAnyThread() > 0;
}

void FAerosCloudViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::BeforeDOF)
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FAerosCloudViewExtension::Render_RenderThread));
	}
}

FScreenPassTexture FAerosCloudViewExtension::Render_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	using namespace AerosCloudRenderInternal;

	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	if (!SceneColor.IsValid() || !Inputs.SceneTextures.SceneTextures)
	{
		return SceneColor;
	}
	RDG_EVENT_SCOPE(GraphBuilder, "AerosClouds");

	FRDGTextureRef SceneDepth = Inputs.SceneTextures.SceneTextures->GetContents()->SceneDepthTexture;
	const FIntRect ViewRect = SceneColor.ViewRect;
	const FIntPoint FullSize = ViewRect.Size();
	const FIntPoint HalfSize = FIntPoint::DivideAndRoundUp(FullSize, 2);

	const double Time = View.Family ? View.Family->Time.GetWorldTimeSeconds() : 0.0;
	const float DeltaTime = PrevTime >= 0.0 ? (float)FMath::Clamp(Time - PrevTime, 0.0, 0.5) : 0.f;
	const float WindMs = CVarWind.GetValueOnRenderThread();

	FCommonParameters Common;
	Common.CameraKm = FVector3f(View.ViewMatrices.GetViewOrigin() / 100000.0);
	Common.WindKmS = FVector3f(WindMs / 1000.f, 0.f, 0.f);
	Common.TimeS = (float)FMath::Fmod(Time, 4096.0);
	Common.FrameIndex = FrameIndex++;
	Common.ViewMin = ViewRect.Min;
	Common.HalfSize = HalfSize;
	Common.FullSize = FullSize;

	// 1. Trace at half resolution.
	const FRDGTextureDesc ColorDesc = FRDGTextureDesc::Create2D(HalfSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	const FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(HalfSize, PF_G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef TraceColor = GraphBuilder.CreateTexture(ColorDesc, TEXT("AerosClouds.TraceColor"));
	FRDGTextureRef TraceDepth = GraphBuilder.CreateTexture(DepthDesc, TEXT("AerosClouds.TraceDepth"));
	{
		FTraceCS::FParameters* P = GraphBuilder.AllocParameters<FTraceCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->Common = Common;
		P->SceneDepthTexture = SceneDepth;
		P->OutColor = GraphBuilder.CreateUAV(TraceColor);
		P->OutDepth = GraphBuilder.CreateUAV(TraceDepth);
		TShaderMapRef<FTraceCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Trace %dx%d", HalfSize.X, HalfSize.Y), Shader, P,
			FComputeShaderUtils::GetGroupCount(HalfSize, 8));
	}

	// 2. Temporal accumulation, reprojected along camera motion and wind.
	const bool bHistoryValid = HistoryColor.IsValid() && HistoryColor->GetDesc().Extent == HalfSize && DeltaTime > 0.f;
	FRDGTextureRef HistColor = bHistoryValid ? GraphBuilder.RegisterExternalTexture(HistoryColor) : TraceColor;
	FRDGTextureRef HistDepth = bHistoryValid ? GraphBuilder.RegisterExternalTexture(HistoryDepth) : TraceDepth;
	FRDGTextureRef AccumColor = GraphBuilder.CreateTexture(ColorDesc, TEXT("AerosClouds.AccumColor"));
	FRDGTextureRef AccumDepth = GraphBuilder.CreateTexture(DepthDesc, TEXT("AerosClouds.AccumDepth"));

	const FMatrix ViewProj = View.ViewMatrices.GetWorldToView() * View.ViewMatrices.ComputeProjectionNoAAMatrix();
	{
		const FMatrix TranslatedToPrevClip = FTranslationMatrix(-View.ViewMatrices.GetPreViewTranslation()) * (bHistoryValid ? PrevViewProj : ViewProj);

		FTemporalCS::FParameters* P = GraphBuilder.AllocParameters<FTemporalCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->Common = Common;
		P->CurColor = TraceColor;
		P->CurDepth = TraceDepth;
		P->HistColor = HistColor;
		P->HistDepth = HistDepth;
		P->LinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->TranslatedToPrevClip = FMatrix44f(TranslatedToPrevClip);
		P->DeltaTime = DeltaTime;
		P->NewWeight = FMath::Clamp(CVarBlend.GetValueOnRenderThread(), 0.02f, 1.f);
		P->bHistoryValid = bHistoryValid ? 1u : 0u;
		P->OutColor = GraphBuilder.CreateUAV(AccumColor);
		P->OutDepth = GraphBuilder.CreateUAV(AccumDepth);
		TShaderMapRef<FTemporalCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Temporal"), Shader, P,
			FComputeShaderUtils::GetGroupCount(HalfSize, 8));
	}
	GraphBuilder.QueueTextureExtraction(AccumColor, &HistoryColor);
	GraphBuilder.QueueTextureExtraction(AccumDepth, &HistoryDepth);
	PrevViewProj = ViewProj;
	PrevTime = Time;

	// 3. Depth-aware upsample + composite over the scene, plus the fresh lightning term.
	FRDGTextureDesc OutDesc = SceneColor.Texture->Desc;
	OutDesc.Flags |= TexCreate_UAV | TexCreate_ShaderResource;
	OutDesc.Flags &= ~(TexCreate_FastVRAM);
	FRDGTextureRef Output = GraphBuilder.CreateTexture(OutDesc, TEXT("AerosClouds.SceneColor"));
	{
		FVector3f FlashKm(0.f);
		const bool bFlash = TestFlash(Time, CVarFlash.GetValueOnRenderThread(), FlashKm);

		FCompositeCS::FParameters* P = GraphBuilder.AllocParameters<FCompositeCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->Common = Common;
		P->SceneColorTexture = SceneColor.Texture;
		P->SceneDepthTexture = SceneDepth;
		P->CloudColor = AccumColor;
		P->CloudDepth = AccumDepth;
		P->FlashKm = FlashKm + FVector3f(Common.WindKmS) * Common.TimeS;
		P->FlashColor = bFlash ? FVector3f(0.75f, 0.8f, 1.f) * 4000.f : FVector3f(0.f);
		P->OutSceneColor = GraphBuilder.CreateUAV(Output);
		TShaderMapRef<FCompositeCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Composite"), Shader, P,
			FComputeShaderUtils::GetGroupCount(FullSize, 8));
	}

	return FScreenPassTexture(Output, ViewRect);
}
