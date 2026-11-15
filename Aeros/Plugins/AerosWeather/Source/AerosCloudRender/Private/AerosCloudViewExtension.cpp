// Copyright Aeros. All Rights Reserved.

#include "AerosCloudViewExtension.h"
#include "AerosCloudRender.h"
#include "AerosCloudNoise.h"
#include "Async/Async.h"

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
#include "SystemTextures.h"
#include "TextureResource.h"

namespace AerosCloudRenderInternal
{
	TAutoConsoleVariable<int32> CVarEnable(TEXT("r.AerosClouds"), 0,
		TEXT("Aeros custom cloud renderer (spike). 0 off, 1 on."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarWind(TEXT("r.AerosClouds.Wind"), 0.f,
		TEXT("Test override of the storm motion used for reprojection, m/s along +X (0 = use the simulation)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarFlash(TEXT("r.AerosClouds.Flash"), 0.f,
		TEXT("Test lightning: flashes per second near the camera (0 = simulation flashes only, < 0 = no flash at all)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarBlend(TEXT("r.AerosClouds.HistoryBlend"), 0.1f,
		TEXT("Weight of the new frame in the temporal accumulation (1 = no history)."), ECVF_RenderThreadSafe);

	BEGIN_SHADER_PARAMETER_STRUCT(FCommonParameters, )
		SHADER_PARAMETER(FVector3f, CameraKm)
		SHADER_PARAMETER(FVector3f, MotionKmS)
		SHADER_PARAMETER(float, TimeS)
		SHADER_PARAMETER(uint32, FrameIndex)
		SHADER_PARAMETER(FIntPoint, ViewMin)
		SHADER_PARAMETER(FIntPoint, HalfSize)
		SHADER_PARAMETER(FIntPoint, FullSize)
	END_SHADER_PARAMETER_STRUCT()

	// Simulation data, same layout as the old cloud material parameters.
	BEGIN_SHADER_PARAMETER_STRUCT(FCloudDataParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WeatherMap)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ShapeMap)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, VolumePrev)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, VolumeCur)
		SHADER_PARAMETER_SAMPLER(SamplerState, DataSampler)
		SHADER_PARAMETER(FVector4f, DomainCm)
		SHADER_PARAMETER(FVector4f, RectPrevCm)
		SHADER_PARAMETER(FVector4f, RectCurCm)
		SHADER_PARAMETER(float, VolumeBlend)
		SHADER_PARAMETER(float, VolumeWeight)
		SHADER_PARAMETER(FVector3f, NoiseOffsetKm)
	END_SHADER_PARAMETER_STRUCT()

	BEGIN_SHADER_PARAMETER_STRUCT(FNoiseParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, ShapeNoise)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, DetailNoise)
		SHADER_PARAMETER_SAMPLER(SamplerState, NoiseSampler)
	END_SHADER_PARAMETER_STRUCT()

	class FTraceCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FTraceCS);
		SHADER_USE_PARAMETER_STRUCT(FTraceCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_STRUCT_INCLUDE(FCommonParameters, Common)
			SHADER_PARAMETER_STRUCT_INCLUDE(FCloudDataParameters, Data)
			SHADER_PARAMETER_STRUCT_INCLUDE(FNoiseParameters, Noise)
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

/** CPU-side noise with its mip chains (box filtered, wrapped), ready for upload. */
struct FAerosCloudNoiseData
{
	struct FVolume { int32 Size = 0; TArray<TArray<uint8>> Mips; };
	FVolume Shape;
	FVolume Detail;
	std::atomic<bool> bReady{ false };
};

namespace AerosCloudRenderInternal
{
	TWeakPtr<FAerosCloudViewExtension, ESPMode::ThreadSafe> GExtension;

	void BuildMips(const std::vector<uint8_t>& Level0, int32 Size, FAerosCloudNoiseData::FVolume& Out)
	{
		Out.Size = Size;
		Out.Mips.Reset();
		Out.Mips.Emplace(Level0.data(), int32(Level0.size()));
		for (int32 S = Size / 2; S >= 1; S /= 2)
		{
			const TArray<uint8>& Src = Out.Mips.Last();
			const int32 P = S * 2;
			TArray<uint8> Dst;
			Dst.SetNumUninitialized(S * S * S * 4);
			for (int32 Z = 0; Z < S; ++Z) for (int32 Y = 0; Y < S; ++Y) for (int32 X = 0; X < S; ++X) for (int32 C = 0; C < 4; ++C)
			{
				int32 Sum = 0;
				for (int32 K = 0; K < 8; ++K)
				{
					const int32 Sx = X * 2 + (K & 1), Sy = Y * 2 + ((K >> 1) & 1), Sz = Z * 2 + (K >> 2);
					Sum += Src[((Sz * P + Sy) * P + Sx) * 4 + C];
				}
				Dst[((Z * S + Y) * S + X) * 4 + C] = uint8((Sum + 4) / 8);
			}
			Out.Mips.Add(MoveTemp(Dst));
		}
	}

	TRefCountPtr<IPooledRenderTarget> UploadVolume(FRHICommandListImmediate& RHICmdList, const FAerosCloudNoiseData::FVolume& V, const TCHAR* Name)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D(Name, FIntVector(V.Size), PF_R8G8B8A8)
			.SetNumMips(uint8(V.Mips.Num()))
			.SetFlags(ETextureCreateFlags::ShaderResource)
			.SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		for (int32 Mip = 0; Mip < V.Mips.Num(); ++Mip)
		{
			const uint32 S = uint32(FMath::Max(V.Size >> Mip, 1));
			const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, S, S, S);
			RHICmdList.UpdateTexture3D(Tex, Mip, Region, S * 4, S * S * 4, V.Mips[Mip].GetData());
		}
		return CreateRenderTarget(Tex, Name);
	}

	FRDGTextureRef RegisterOr(FRDGBuilder& GraphBuilder, FTextureResource* Resource, const TCHAR* Name, bool bVolume)
	{
		FRHITexture* RHI = Resource ? Resource->TextureRHI.GetReference() : nullptr;
		if (RHI)
		{
			return RegisterExternalTexture(GraphBuilder, RHI, Name);
		}
		return bVolume ? GSystemTextures.GetVolumetricBlackDummy(GraphBuilder) : GSystemTextures.GetBlackDummy(GraphBuilder);
	}
}

void AerosCloudRender::SetParams(const FAerosCloudRenderParams& Params)
{
	TSharedPtr<FAerosCloudViewExtension, ESPMode::ThreadSafe> Ext = AerosCloudRenderInternal::GExtension.Pin();
	if (!Ext.IsValid())
	{
		return;
	}
	ENQUEUE_RENDER_COMMAND(AerosCloudSetParams)([Ext, Params](FRHICommandListImmediate&)
	{
		Ext->SetParams_RenderThread(Params);
	});
}

bool AerosCloudRender::IsEnabled()
{
	return AerosCloudRenderInternal::CVarEnable.GetValueOnGameThread() > 0;
}

void FAerosCloudViewExtension::SetGlobal(const TSharedPtr<FAerosCloudViewExtension, ESPMode::ThreadSafe>& Extension)
{
	AerosCloudRenderInternal::GExtension = Extension;
}

FAerosCloudViewExtension::FAerosCloudViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	// About 2 s of work: generate on a worker, the clouds start once it is uploaded.
	NoiseData = MakeShared<FAerosCloudNoiseData, ESPMode::ThreadSafe>();
	TSharedPtr<FAerosCloudNoiseData, ESPMode::ThreadSafe> Data = NoiseData;
	Async(EAsyncExecution::ThreadPool, [Data]()
	{
		AerosCloudRenderInternal::BuildMips(AerosCloudNoise::MakeShape(128, 1), 128, Data->Shape);
		AerosCloudRenderInternal::BuildMips(AerosCloudNoise::MakeDetail(32, 2), 32, Data->Detail);
		Data->bReady = true;
	});
}

bool FAerosCloudViewExtension::UploadNoise_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	if (ShapeNoiseRT.IsValid())
	{
		return true;
	}
	if (!NoiseData.IsValid() || !NoiseData->bReady)
	{
		return false;
	}
	ShapeNoiseRT = AerosCloudRenderInternal::UploadVolume(RHICmdList, NoiseData->Shape, TEXT("AerosClouds.ShapeNoise"));
	DetailNoiseRT = AerosCloudRenderInternal::UploadVolume(RHICmdList, NoiseData->Detail, TEXT("AerosClouds.DetailNoise"));
	NoiseData.Reset();
	return true;
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
	if (!SceneColor.IsValid() || !Inputs.SceneTextures.SceneTextures || !UploadNoise_RenderThread(GraphBuilder.RHICmdList))
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
	const FAerosCloudRenderParams& Data = RTParams;

	FCommonParameters Common;
	Common.CameraKm = FVector3f(View.ViewMatrices.GetViewOrigin() / 100000.0);
	Common.MotionKmS = WindMs > 0.f ? FVector3f(WindMs / 1000.f, 0.f, 0.f) : Data.MotionKmS;
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
		P->Data.WeatherMap = RegisterOr(GraphBuilder, Data.WeatherMap, TEXT("AerosClouds.WeatherMap"), false);
		P->Data.ShapeMap = RegisterOr(GraphBuilder, Data.ShapeMap, TEXT("AerosClouds.ShapeMap"), false);
		P->Data.VolumePrev = RegisterOr(GraphBuilder, Data.VolumePrev, TEXT("AerosClouds.VolumePrev"), true);
		P->Data.VolumeCur = RegisterOr(GraphBuilder, Data.VolumeCur, TEXT("AerosClouds.VolumeCur"), true);
		P->Data.DataSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->Data.DomainCm = Data.DomainCm;
		P->Data.RectPrevCm = Data.RectPrevCm;
		P->Data.RectCurCm = Data.RectCurCm;
		P->Data.VolumeBlend = Data.VolumeBlend;
		P->Data.VolumeWeight = Data.VolumeWeight;
		P->Data.NoiseOffsetKm = Data.NoiseOffsetKm;
		P->Noise.ShapeNoise = GraphBuilder.RegisterExternalTexture(ShapeNoiseRT);
		P->Noise.DetailNoise = GraphBuilder.RegisterExternalTexture(DetailNoiseRT);
		P->Noise.NoiseSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
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
		float FlashIntensity = 0.f;
		const float FlashRate = CVarFlash.GetValueOnRenderThread();
		if (FlashRate > 0.f && TestFlash(Time, FlashRate, FlashKm))
		{
			FlashKm += FVector3f(Common.CameraKm.X, Common.CameraKm.Y, 0.f);   // test flash near the camera
			FlashIntensity = 1.f;
		}
		else if (FlashRate == 0.f && Data.Flashes.Num() > 0)
		{
			FlashKm = Data.Flashes[0].PosKm;
			FlashIntensity = Data.Flashes[0].Intensity;
		}

		FCompositeCS::FParameters* P = GraphBuilder.AllocParameters<FCompositeCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->Common = Common;
		P->SceneColorTexture = SceneColor.Texture;
		P->SceneDepthTexture = SceneDepth;
		P->CloudColor = AccumColor;
		P->CloudDepth = AccumDepth;
		P->FlashKm = FlashKm;
		P->FlashColor = FVector3f(0.75f, 0.8f, 1.f) * 4000.f * FlashIntensity;
		P->OutSceneColor = GraphBuilder.CreateUAV(Output);
		TShaderMapRef<FCompositeCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Composite"), Shader, P,
			FComputeShaderUtils::GetGroupCount(FullSize, 8));
	}

	return FScreenPassTexture(Output, ViewRect);
}
