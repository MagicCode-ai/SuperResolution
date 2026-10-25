#include "MagicSRShaders.h"

#include "CommonRenderResources.h"
#include "PixelShaderUtils.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "SystemTextures.h"

class FMagicSRDecodeVelocityPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FMagicSRDecodeVelocityPS);
	SHADER_USE_PARAMETER_STRUCT(FMagicSRDecodeVelocityPS, FGlobalShader);

	class FWriteReactive : SHADER_PERMUTATION_BOOL("MAGICSR_WRITE_REACTIVE");
	using FPermutationDomain = TShaderPermutationDomain<FWriteReactive>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, EncodedVelocityTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, EncodedVelocitySampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
		SHADER_PARAMETER(FVector4f, OutputUVMinMax)
		SHADER_PARAMETER(FVector2f, OutputSize)
		SHADER_PARAMETER(float, ReactiveStartPx)
		SHADER_PARAMETER(float, ReactiveSatPx)
		SHADER_PARAMETER(FVector4f, TexelToScreenScaleBias)
		SHADER_PARAMETER(FVector2f, ViewRectMin)
		SHADER_PARAMETER(FVector2f, DepthExtent)
		SHADER_PARAMETER(uint32, bHasDynamicVelocity)
		SHADER_PARAMETER(uint32, bStencilReactive)
		SHADER_PARAMETER(uint32, StencilReactiveValue)
		SHADER_PARAMETER(float, StencilReactiveStrength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencilTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMagicSRDecodeVelocityPS, "/Plugin/MagicSR/Private/MagicSRDecodeVelocity.usf", "MainPS",
						SF_Pixel);

class FMagicSRCopyDepthPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FMagicSRCopyDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FMagicSRCopyDepthPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
		SHADER_PARAMETER(FVector4f, DepthUVMinMax)
		SHADER_PARAMETER(FVector2f, OutputSize)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMagicSRCopyDepthPS, "/Plugin/MagicSR/Private/MagicSRCopyDepth.usf", "MainPS", SF_Pixel);

FRDGTextureRef AddMagicSRDecodeVelocityPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef EncodedVelocity,
	FRDGTextureRef SceneDepth,
	const FIntRect& ViewRect,
	const FIntPoint& OutputExtent,
	FRDGTextureRef ReactiveMask,
	float ReactiveStartPx,
	float ReactiveSatPx,
	const FMagicSRDecodeVelocityExtras& Extras)
{
	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		OutputExtent,
		PF_G16R16F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
	FRDGTextureRef Output = GraphBuilder.CreateTexture(Desc, TEXT("MagicSR.DecodedVelocity"));

	auto ClearOutputs = [&]()
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Output), FVector4(0.0, 0.0, 0.0, 0.0));
		if (ReactiveMask)
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ReactiveMask), FVector4(0, 0, 0, 0));
		}
	};

	// Camera MV needs raw device Z and the view uniform. Without those, keep a zero target.
	if (!SceneDepth || !View.ViewUniformBuffer || ViewRect.Width() <= 0 || ViewRect.Height() <= 0)
	{
		ClearOutputs();
		return Output;
	}

	// UE5 desktop velocity is PF_G16R16 (UNORM) or PF_A16B16G16R16 when depth is packed.
	// A 1x1 black (or unknown) texture is the "nothing is moving" binding: still draw,
	// and let every pixel take the camera path instead of clearing MV to zero.
	const EPixelFormat VelFmt = EncodedVelocity ? EncodedVelocity->Desc.Format : PF_Unknown;
	const bool bKnownFmt =
		VelFmt == PF_G16R16 || VelFmt == PF_A16B16G16R16 || VelFmt == PF_G16R16F ||
		VelFmt == PF_FloatRGBA || VelFmt == PF_G32R32F || VelFmt == PF_A32B32G32R32F;
	const bool bHasDynamicVelocity =
		EncodedVelocity != nullptr && EncodedVelocity->Desc.Extent.X > 2 &&
		EncodedVelocity->Desc.Extent.Y > 2 && bKnownFmt;
	if (!EncodedVelocity)
	{
		ClearOutputs();
		return Output;
	}

	const FIntPoint VelExt = EncodedVelocity->Desc.Extent;
	const FVector2f UvMin(
		float(ViewRect.Min.X) / float(VelExt.X), float(ViewRect.Min.Y) / float(VelExt.Y));
	const FVector2f UvMax(
		float(ViewRect.Max.X) / float(VelExt.X), float(ViewRect.Max.Y) / float(VelExt.Y));

	// Same texel→NDC change of basis TSR uses (TexelPosition → ScreenPosition).
	// Extent is unused on that path; the view rect is what places the pixel.
	const FIntPoint DepthExt = SceneDepth->Desc.Extent;
	const FScreenTransform TexelToScreen = FScreenTransform::ChangeTextureBasisFromTo(
		DepthExt, ViewRect, FScreenTransform::ETextureBasis::TexelPosition,
		FScreenTransform::ETextureBasis::ScreenPosition);

	FMagicSRDecodeVelocityPS::FParameters* Parameters =
		GraphBuilder.AllocParameters<FMagicSRDecodeVelocityPS::FParameters>();
	Parameters->View = View.ViewUniformBuffer;
	Parameters->EncodedVelocityTexture = EncodedVelocity;
	Parameters->EncodedVelocitySampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->SceneDepthTexture = SceneDepth;
	Parameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->OutputUVMinMax = FVector4f(UvMin.X, UvMin.Y, UvMax.X, UvMax.Y);
	Parameters->OutputSize = FVector2f(float(OutputExtent.X), float(OutputExtent.Y));
	Parameters->ReactiveStartPx = ReactiveStartPx;
	Parameters->ReactiveSatPx = ReactiveSatPx;
	Parameters->TexelToScreenScaleBias = FVector4f(
		TexelToScreen.Scale.X, TexelToScreen.Scale.Y, TexelToScreen.Bias.X, TexelToScreen.Bias.Y);
	Parameters->ViewRectMin = FVector2f(float(ViewRect.Min.X), float(ViewRect.Min.Y));
	Parameters->DepthExtent = FVector2f(float(DepthExt.X), float(DepthExt.Y));
	Parameters->bHasDynamicVelocity = bHasDynamicVelocity ? 1u : 0u;
	Parameters->bStencilReactive = (Extras.CustomStencil != nullptr && ReactiveMask != nullptr) ? 1u : 0u;
	Parameters->StencilReactiveValue = Extras.StencilValue;
	Parameters->StencilReactiveStrength = Extras.StencilStrength;
	Parameters->CustomStencilTexture = Extras.CustomStencil
		? Extras.CustomStencil
		: FRDGSystemTextures::Get(GraphBuilder).StencilDummySRV;
	Parameters->RenderTargets[0] = FRenderTargetBinding(Output, ERenderTargetLoadAction::ENoAction);
	if (ReactiveMask)
	{
		Parameters->RenderTargets[1] = FRenderTargetBinding(ReactiveMask, ERenderTargetLoadAction::ENoAction);
	}

	FMagicSRDecodeVelocityPS::FPermutationDomain Permutation;
	Permutation.Set<FMagicSRDecodeVelocityPS::FWriteReactive>(ReactiveMask != nullptr);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FMagicSRDecodeVelocityPS> PixelShader(ShaderMap, Permutation);

	// Tight pack: write into (0,0)-(W,H) regardless of ViewRect.Min.
	const FIntRect DestRect(FIntPoint::ZeroValue, OutputExtent);
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder, ShaderMap, RDG_EVENT_NAME("MagicSR DecodeVelocity"), PixelShader, Parameters,
		DestRect);

	return Output;
}

FRDGTextureRef AddMagicSRCopyDepthPass(
	FRDGBuilder& GraphBuilder,
	FScreenPassViewInfo ViewInfo,
	FRDGTextureRef SceneDepth,
	const FIntRect& ViewRect,
	const FIntPoint& OutputExtent)
{
	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
		OutputExtent,
		PF_R32_FLOAT,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
	FRDGTextureRef Output = GraphBuilder.CreateTexture(Desc, TEXT("MagicSR.DeviceDepthR32F"));

	const FIntPoint Ext = SceneDepth->Desc.Extent;
	const FVector2f UvMin(float(ViewRect.Min.X) / float(Ext.X), float(ViewRect.Min.Y) / float(Ext.Y));
	const FVector2f UvMax(float(ViewRect.Max.X) / float(Ext.X), float(ViewRect.Max.Y) / float(Ext.Y));

	FMagicSRCopyDepthPS::FParameters* Parameters =
		GraphBuilder.AllocParameters<FMagicSRCopyDepthPS::FParameters>();
	Parameters->SceneDepthTexture = SceneDepth;
	Parameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->DepthUVMinMax = FVector4f(UvMin.X, UvMin.Y, UvMax.X, UvMax.Y);
	Parameters->OutputSize = FVector2f(float(OutputExtent.X), float(OutputExtent.Y));
	Parameters->RenderTargets[0] = FRenderTargetBinding(Output, ERenderTargetLoadAction::ENoAction);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ViewInfo.FeatureLevel);
	TShaderMapRef<FMagicSRCopyDepthPS> PixelShader(ShaderMap);

	const FIntRect DestRect(FIntPoint::ZeroValue, OutputExtent);
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder, ShaderMap, RDG_EVENT_NAME("MagicSR CopyDepth"), PixelShader, Parameters, DestRect);

	return Output;
}
