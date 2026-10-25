#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"

#if MAGIC_SR_SUPPORTED
extern "C" {
#include "mc_interface.h"
}
#endif

struct FMagicSRNativeResource
{
    uint64 VkImage = 0;
    uint64 VkImageView = 0;
    void* Pointer = nullptr;
    uint32 GlTexture = 0;
    uint32 Format = 0;
    uint32 Layout = 0;
    bool bValid = false;
};

enum class EMagicSRRHIKind : uint8
{
    Unknown,
    Vulkan,
    D3D11,
    D3D12,
    OpenGL,
    Metal
};

namespace MagicSRRHI
{
EMagicSRRHIKind GetKind();
bool ResolveTexture(FRHITexture* Texture, FMagicSRNativeResource& Out);
FRHITexture* ResolveFromGameResources(FTextureRenderTargetResource* RTResource, FTextureResource* TextureResource);

#if MAGIC_SR_SUPPORTED
void FillColorResource(const FMagicSRNativeResource& Native, ::magic_resource_t& Out);
void FillDataHandle(const FMagicSRNativeResource& Native, ::magic_data_e& Out);
#endif
}
