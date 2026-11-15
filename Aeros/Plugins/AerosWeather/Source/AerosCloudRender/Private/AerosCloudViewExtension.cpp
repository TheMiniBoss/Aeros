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
	TAutoConsoleVariable<int32> CVarEnable(TEXT("r.AerosClouds"), 1,
		TEXT("Aeros cloud renderer: 1 = custom ray march (default), 0 = engine VolumetricCloud with the old material."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarWind(TEXT("r.AerosClouds.Wind"), 0.f,
		TEXT("Test override of the storm motion used for reprojection, m/s along +X (0 = use the simulation)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarFlash(TEXT("r.AerosClouds.Flash"), 0.f,
		TEXT("Test lightning: flashes per second near the camera (0 = simulation flashes only, < 0 = no flash at all)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarBlend(TEXT("r.AerosClouds.HistoryBlend"), 0.1f,
		TEXT("Weight of the new frame in the temporal accumulation (1 = no history)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarAmbient(TEXT("r.AerosClouds.Ambient"), 1.f,
		TEXT("Scale of the sky light on the clouds (1 = physical)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarHaze(TEXT("r.AerosClouds.Haze"), 1.f,
		TEXT("Scale of the aerial perspective in front of the clouds (1 = physical, 0 = off)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> CVarQuality(TEXT("r.AerosClouds.Quality"), 2,
		TEXT("Cloud quality 0-3: max ray steps 96 / 128 / 192 / 256, sun samples 3 / 4 / 6 / 8, trace at 1/4 resolution at 0 (1/2 otherwise)."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<float> CVarTornadoK(TEXT("r.AerosClouds.TornadoK"), 2.f,
		TEXT("Condensation calibration K of the tornado funnels (Core/AerosFunnel.h): higher = funnels reach lower."), ECVF_Default);
	TAutoConsoleVariable<int32> CVarTornadoDebug(TEXT("r.AerosClouds.TornadoDebug"), 0,
		TEXT("1: draw each tornado's bounding volume as a faint cloud."), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> CVarLogResets(TEXT("r.AerosClouds.LogResets"), 0,
		TEXT("1: log every reset of the cloud history (camera cut, teleport, size, time) at Log level."), ECVF_RenderThreadSafe);

	BEGIN_SHADER_PARAMETER_STRUCT(FCommonParameters, )
		SHADER_PARAMETER(FVector3f, CameraKm)
		SHADER_PARAMETER(FVector3f, MotionKmS)
		SHADER_PARAMETER(FVector3f, DriftKmS)
		SHADER_PARAMETER(float, TimeS)
		SHADER_PARAMETER(uint32, FrameIndex)
		SHADER_PARAMETER(FIntPoint, ViewMin)
		SHADER_PARAMETER(FIntPoint, HalfSize)
		SHADER_PARAMETER(FIntPoint, FullSize)
		SHADER_PARAMETER(float, AmbientScale)
		SHADER_PARAMETER(int32, TraceScale)
		SHADER_PARAMETER(FIntPoint, JitterOffset)
		SHADER_PARAMETER(int32, MaxSteps)
		SHADER_PARAMETER(int32, LightSteps)
		SHADER_PARAMETER(float, HazeScale)
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
		SHADER_PARAMETER_ARRAY(FVector4f, TornadoA, [4])   // ground km xyz, cloud base km
		SHADER_PARAMETER_ARRAY(FVector4f, TornadoB, [4])   // axis Bezier P1.xy, P2.xy (km offsets)
		SHADER_PARAMETER_ARRAY(FVector4f, TornadoC, [4])   // top offset xy (km), Vmax (m/s), core radius (km)
		SHADER_PARAMETER_ARRAY(FVector4f, TornadoD, [4])   // K, spin phase, rope-out, sub-vortices
		SHADER_PARAMETER_ARRAY(FVector4f, TornadoE, [4])   // dust tint rgb, sub-vortex phase
		SHADER_PARAMETER_ARRAY(FVector4f, TornadoF, [4])   // fade
		SHADER_PARAMETER(uint32, NumTornadoes)
		SHADER_PARAMETER(uint32, TornadoDebug)
	END_SHADER_PARAMETER_STRUCT()

	BEGIN_SHADER_PARAMETER_STRUCT(FNoiseParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, ShapeNoise)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, DetailNoise)
		SHADER_PARAMETER_SAMPLER(SamplerState, NoiseSampler)
	END_SHADER_PARAMETER_STRUCT()

	class FShadowMapCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FShadowMapCS);
		SHADER_USE_PARAMETER_STRUCT(FShadowMapCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_STRUCT_INCLUDE(FCloudDataParameters, Data)
			SHADER_PARAMETER_STRUCT_INCLUDE(FNoiseParameters, Noise)
			SHADER_PARAMETER(FVector4f, ShadowRectCm)
			SHADER_PARAMETER(FIntPoint, ShadowSize)
			SHADER_PARAMETER(uint32, ShadowRowStep)
			SHADER_PARAMETER(uint32, ShadowRowPhase)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutShadow)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FShadowMapCS, "/Plugin/AerosWeather/Private/AerosClouds.usf", "ShadowMapCS", SF_Compute);

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
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
			SHADER_PARAMETER(FMatrix44f, TranslatedToPrevClip)
			SHADER_PARAMETER(float, DeltaTime)
			SHADER_PARAMETER(float, NewWeight)
			SHADER_PARAMETER(float, BlendBoost)
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
			SHADER_PARAMETER_ARRAY(FVector4f, FlashPosKm, [8])
			SHADER_PARAMETER(uint32, NumFlashes)
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
	// No weather (no running simulation in this world, e.g. an editor viewport): nothing to draw.
	if (!SceneColor.IsValid() || !Inputs.SceneTextures.SceneTextures || !RTParams.WeatherMap || !UploadNoise_RenderThread(GraphBuilder.RHICmdList))
	{
		return SceneColor;
	}
	RDG_EVENT_SCOPE(GraphBuilder, "AerosClouds");

	FRDGTextureRef SceneDepth = Inputs.SceneTextures.SceneTextures->GetContents()->SceneDepthTexture;
	const FIntRect ViewRect = SceneColor.ViewRect;
	const FIntPoint FullSize = ViewRect.Size();
	const int32 Quality = FMath::Clamp(CVarQuality.GetValueOnRenderThread(), 0, 3);
	const int32 TraceScale = Quality == 0 ? 4 : 2;
	const FIntPoint HalfSize = FIntPoint::DivideAndRoundUp(FullSize, TraceScale);   // traced resolution

	const double Time = View.Family ? View.Family->Time.GetWorldTimeSeconds() : 0.0;
	const float WindMs = CVarWind.GetValueOnRenderThread();
	const FAerosCloudRenderParams& Data = RTParams;

	// History of this view; forget views not rendered for 120 frames.
	const uint32 Frame = uint32(GFrameCounterRenderThread);
	for (auto It = Histories.CreateIterator(); It; ++It)
	{
		if (Frame - It.Value()->LastFrame > 120u)
		{
			It.RemoveCurrent();
		}
	}
	const uint32 ViewKey = View.State ? View.State->GetViewKey() : 0u;
	TUniquePtr<FAerosCloudHistory>& HistoryPtr = Histories.FindOrAdd(ViewKey);
	if (!HistoryPtr.IsValid())
	{
		HistoryPtr = MakeUnique<FAerosCloudHistory>();
	}
	FAerosCloudHistory& History = *HistoryPtr;
	History.LastFrame = Frame;

	const FVector ViewOrigin = View.ViewMatrices.GetViewOrigin();
	const double Dt = History.Time >= 0.0 ? Time - History.Time : -1.0;
	const TCHAR* ResetReason = nullptr;
	if (!History.Color.IsValid() || !History.Depth.IsValid())
	{
		ResetReason = TEXT("first frame");
	}
	else if (History.Color->GetDesc().Extent != FullSize)
	{
		ResetReason = TEXT("size");
	}
	else if (View.bCameraCut)
	{
		ResetReason = TEXT("camera cut");
	}
	else if (FVector::Dist(ViewOrigin, History.ViewOrigin) > 200000.0)
	{
		ResetReason = TEXT("teleport");
	}
	else if (Dt <= 0.0 || Dt > 0.5)
	{
		ResetReason = TEXT("time");
	}
	if (ResetReason)
	{
		UE_LOG(LogTemp, Verbose, TEXT("AerosClouds: history reset (%s)"), ResetReason);
		if (CVarLogResets.GetValueOnRenderThread() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("AerosClouds: history reset (%s) view %u, %dx%d"), ResetReason, ViewKey, HalfSize.X, HalfSize.Y);
		}
	}
	const bool bHistoryValid = ResetReason == nullptr;
	const float DeltaTime = bHistoryValid ? float(Dt) : 0.f;
	// The voxel blend ramps 0 -> 1 in 1 s and restarts at 0 on a grid swap (which shows the same
	// field): only the ramp itself is a change of the data.
	const float BlendStep = Data.VolumeBlend >= History.Blend ? Data.VolumeBlend - History.Blend : Data.VolumeBlend;

	FCommonParameters Common;
	Common.CameraKm = FVector3f(View.ViewMatrices.GetViewOrigin() / 100000.0);
	Common.MotionKmS = WindMs > 0.f ? FVector3f(WindMs / 1000.f, 0.f, 0.f) : Data.MotionKmS;
	Common.DriftKmS = WindMs > 0.f ? Common.MotionKmS : Data.DriftKmS;
	Common.TimeS = (float)FMath::Fmod(Time, 4096.0);
	Common.FrameIndex = FrameIndex++;
	Common.ViewMin = ViewRect.Min;
	Common.HalfSize = HalfSize;
	Common.FullSize = FullSize;
	Common.AmbientScale = FMath::Max(CVarAmbient.GetValueOnRenderThread(), 0.f);
	{
		static const int32 Steps[4] = { 96, 128, 192, 256 };
		static const int32 Light[4] = { 3, 4, 6, 8 };
		Common.TraceScale = TraceScale;
		// Traced pixel inside each block, cycling over the block (ordered so neighbours differ)
		static const int32 Order2[4][2] = { {0, 0}, {1, 1}, {1, 0}, {0, 1} };
		static const int32 Order4[16] = { 0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12 };
		const uint32 Phase = Common.FrameIndex;
		Common.JitterOffset = TraceScale == 2 ? FIntPoint(Order2[Phase % 4][0], Order2[Phase % 4][1])
			: FIntPoint(Order4[Phase % 16] % 4, Order4[Phase % 16] / 4);
		Common.MaxSteps = Steps[Quality];
		Common.LightSteps = Light[Quality];
	}
	Common.HazeScale = FMath::Clamp(CVarHaze.GetValueOnRenderThread(), 0.f, 1.f);

	// 0. Cloud shadow map for the sun's light function: a quarter of the rows each frame (a full refresh
	// every 4 frames without a GPU spike), and all of it at once when its rect moved (the material
	// already reads the new rect).
	FCloudDataParameters DataParams;
	DataParams.WeatherMap = RegisterOr(GraphBuilder, Data.WeatherMap, TEXT("AerosClouds.WeatherMap"), false);
	DataParams.ShapeMap = RegisterOr(GraphBuilder, Data.ShapeMap, TEXT("AerosClouds.ShapeMap"), false);
	DataParams.VolumePrev = RegisterOr(GraphBuilder, Data.VolumePrev, TEXT("AerosClouds.VolumePrev"), true);
	DataParams.VolumeCur = RegisterOr(GraphBuilder, Data.VolumeCur, TEXT("AerosClouds.VolumeCur"), true);
	DataParams.DataSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	DataParams.DomainCm = Data.DomainCm;
	DataParams.RectPrevCm = Data.RectPrevCm;
	DataParams.RectCurCm = Data.RectCurCm;
	DataParams.VolumeBlend = Data.VolumeBlend;
	DataParams.VolumeWeight = Data.VolumeWeight;
	DataParams.NoiseOffsetKm = Data.NoiseOffsetKm;
	DataParams.NumTornadoes = 0;
	for (const FAerosCloudTornado& Tw : Data.Tornadoes)
	{
		if (DataParams.NumTornadoes >= 4u || Tw.Fade <= 0.f) continue;
		const uint32 Ti = DataParams.NumTornadoes++;
		DataParams.TornadoA[Ti] = FVector4f(Tw.GroundKm, Tw.CloudBaseKm);
		DataParams.TornadoB[Ti] = Tw.AxisKm;
		DataParams.TornadoC[Ti] = FVector4f(Tw.TopKm.X, Tw.TopKm.Y, Tw.VmaxMs, Tw.CoreRadiusM * 0.001f);
		DataParams.TornadoD[Ti] = FVector4f(Tw.K, Tw.SpinPhase, Tw.RopeOut, float(Tw.NumSubVortices));
		DataParams.TornadoE[Ti] = FVector4f(Tw.DustTint, Tw.SubPhase);
		DataParams.TornadoF[Ti] = FVector4f(Tw.Fade, 0.f, 0.f, 0.f);
	}
	for (uint32 Ti = DataParams.NumTornadoes; Ti < 4u; ++Ti)
	{
		DataParams.TornadoA[Ti] = DataParams.TornadoB[Ti] = DataParams.TornadoC[Ti] = FVector4f(0.f, 0.f, 0.f, 0.f);
		DataParams.TornadoD[Ti] = DataParams.TornadoE[Ti] = DataParams.TornadoF[Ti] = FVector4f(0.f, 0.f, 0.f, 0.f);
	}
	DataParams.TornadoDebug = CVarTornadoDebug.GetValueOnRenderThread() > 0 ? 1u : 0u;
	FNoiseParameters NoiseParams;
	NoiseParams.ShapeNoise = GraphBuilder.RegisterExternalTexture(ShapeNoiseRT);
	NoiseParams.DetailNoise = GraphBuilder.RegisterExternalTexture(DetailNoiseRT);
	NoiseParams.NoiseSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

	FRHITexture* ShadowRHI = Data.ShadowTarget ? Data.ShadowTarget->GetRenderTargetTexture() : nullptr;
	if (ShadowRHI && EnumHasAnyFlags(ShadowRHI->GetFlags(), TexCreate_UAV) && Frame != LastShadowFrame)
	{
		const bool bFull = Data.ShadowRectCm != LastShadowRect;
		LastShadowFrame = Frame;
		LastShadowRect = Data.ShadowRectCm;
		const FIntPoint ShadowSize(ShadowRHI->GetSizeXYZ().X, ShadowRHI->GetSizeXYZ().Y);
		FRDGTextureRef ShadowTex = RegisterExternalTexture(GraphBuilder, ShadowRHI, TEXT("AerosClouds.ShadowMap"));
		FShadowMapCS::FParameters* P = GraphBuilder.AllocParameters<FShadowMapCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->Data = DataParams;
		P->Noise = NoiseParams;
		P->ShadowRectCm = Data.ShadowRectCm;
		P->ShadowSize = ShadowSize;
		P->ShadowRowStep = bFull ? 1u : 4u;
		P->ShadowRowPhase = bFull ? 0u : (Frame & 3u);
		P->OutShadow = GraphBuilder.CreateUAV(ShadowTex);
		GraphBuilder.SetTextureAccessFinal(ShadowTex, ERHIAccess::SRVMask);   // read by the sun's light function
		const FIntPoint Threads(ShadowSize.X, FMath::DivideAndRoundUp(ShadowSize.Y, int32(P->ShadowRowStep)));
		TShaderMapRef<FShadowMapCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ShadowMap %dx%d", Threads.X, Threads.Y), Shader, P,
			FComputeShaderUtils::GetGroupCount(Threads, 8));
	}

	// 1. Trace at half resolution.
	const FRDGTextureDesc ColorDesc = FRDGTextureDesc::Create2D(HalfSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	const FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(HalfSize, PF_G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef TraceColor = GraphBuilder.CreateTexture(ColorDesc, TEXT("AerosClouds.TraceColor"));
	FRDGTextureRef TraceDepth = GraphBuilder.CreateTexture(DepthDesc, TEXT("AerosClouds.TraceDepth"));
	{
		FTraceCS::FParameters* P = GraphBuilder.AllocParameters<FTraceCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->Common = Common;
		P->Data = DataParams;
		P->Noise = NoiseParams;
		P->SceneDepthTexture = SceneDepth;
		P->OutColor = GraphBuilder.CreateUAV(TraceColor);
		P->OutDepth = GraphBuilder.CreateUAV(TraceDepth);
		TShaderMapRef<FTraceCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Trace %dx%d", HalfSize.X, HalfSize.Y), Shader, P,
			FComputeShaderUtils::GetGroupCount(HalfSize, 8));
	}

	// 2. Temporal accumulation, reprojected along camera motion and storm motion.
	FRDGTextureRef HistColor = bHistoryValid ? GraphBuilder.RegisterExternalTexture(History.Color) : TraceColor;
	FRDGTextureRef HistDepth = bHistoryValid ? GraphBuilder.RegisterExternalTexture(History.Depth) : TraceDepth;
	const FRDGTextureDesc AccumColorDesc = FRDGTextureDesc::Create2D(FullSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	const FRDGTextureDesc AccumDepthDesc = FRDGTextureDesc::Create2D(FullSize, PF_G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef AccumColor = GraphBuilder.CreateTexture(AccumColorDesc, TEXT("AerosClouds.AccumColor"));
	FRDGTextureRef AccumDepth = GraphBuilder.CreateTexture(AccumDepthDesc, TEXT("AerosClouds.AccumDepth"));

	const FMatrix ViewProj = View.ViewMatrices.GetWorldToView() * View.ViewMatrices.ComputeProjectionNoAAMatrix();
	{
		const FMatrix TranslatedToPrevClip = FTranslationMatrix(-View.ViewMatrices.GetPreViewTranslation()) * (bHistoryValid ? History.ViewProj : ViewProj);

		FTemporalCS::FParameters* P = GraphBuilder.AllocParameters<FTemporalCS::FParameters>();
		P->View = View.ViewUniformBuffer;
		P->Common = Common;
		P->CurColor = TraceColor;
		P->CurDepth = TraceDepth;
		P->HistColor = HistColor;
		P->HistDepth = HistDepth;
		P->SceneDepthTexture = SceneDepth;
		P->LinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->TranslatedToPrevClip = FMatrix44f(TranslatedToPrevClip);
		P->DeltaTime = DeltaTime;
		P->NewWeight = FMath::Clamp(CVarBlend.GetValueOnRenderThread(), 0.02f, 1.f);
		P->BlendBoost = FMath::Abs(BlendStep) * 4.f;
		P->bHistoryValid = bHistoryValid ? 1u : 0u;
		P->OutColor = GraphBuilder.CreateUAV(AccumColor);
		P->OutDepth = GraphBuilder.CreateUAV(AccumDepth);
		TShaderMapRef<FTemporalCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Temporal %dx%d", FullSize.X, FullSize.Y), Shader, P,
			FComputeShaderUtils::GetGroupCount(FullSize, 8));
	}
	GraphBuilder.QueueTextureExtraction(AccumColor, &History.Color);
	GraphBuilder.QueueTextureExtraction(AccumDepth, &History.Depth);
	History.ViewProj = ViewProj;
	History.Time = Time;
	History.ViewOrigin = ViewOrigin;
	History.Blend = Data.VolumeBlend;

	// 3. Depth-aware upsample + composite over the scene, plus the fresh lightning term.
	FRDGTextureDesc OutDesc = SceneColor.Texture->Desc;
	OutDesc.Flags |= TexCreate_UAV | TexCreate_ShaderResource;
	OutDesc.Flags &= ~(TexCreate_FastVRAM);
	FRDGTextureRef Output = GraphBuilder.CreateTexture(OutDesc, TEXT("AerosClouds.SceneColor"));
	{
		FCompositeCS::FParameters* P = GraphBuilder.AllocParameters<FCompositeCS::FParameters>();
		uint32 NumFlashes = 0;
		const float FlashRate = CVarFlash.GetValueOnRenderThread();
		FVector3f TestKm(0.f);
		if (FlashRate > 0.f && TestFlash(Time, FlashRate, TestKm))
		{
			TestKm += FVector3f(Common.CameraKm.X, Common.CameraKm.Y, 0.f);   // test flash near the camera
			P->FlashPosKm[NumFlashes++] = FVector4f(TestKm, 1.f);
		}
		else if (FlashRate == 0.f)
		{
			for (const FAerosCloudFlash& Flash : Data.Flashes)
			{
				if (NumFlashes < 8u && Flash.Intensity > 0.f)
				{
					P->FlashPosKm[NumFlashes++] = FVector4f(Flash.PosKm, Flash.Intensity);
				}
			}
		}
		P->NumFlashes = NumFlashes;
		P->View = View.ViewUniformBuffer;
		P->Common = Common;
		P->SceneColorTexture = SceneColor.Texture;
		P->SceneDepthTexture = SceneDepth;
		P->CloudColor = AccumColor;
		P->CloudDepth = AccumDepth;
		P->OutSceneColor = GraphBuilder.CreateUAV(Output);
		TShaderMapRef<FCompositeCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Composite"), Shader, P,
			FComputeShaderUtils::GetGroupCount(FullSize, 8));
	}

	return FScreenPassTexture(Output, ViewRect);
}
