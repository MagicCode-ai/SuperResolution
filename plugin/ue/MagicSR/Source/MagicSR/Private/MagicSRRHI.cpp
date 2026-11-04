#include "MagicSRRHI.h"

#include "RHI.h"
#include "TextureResource.h"

#if PLATFORM_WINDOWS || PLATFORM_LINUX
#include "IVulkanDynamicRHI.h"
#endif

namespace MagicSRRHI
{
EMagicSRRHIKind GetKind()
{
    if (GDynamicRHI == nullptr)
    {
        return EMagicSRRHIKind::Unknown;
    }
    const TCHAR* Name = GDynamicRHI->GetName();
    if (FCString::Stristr(Name, TEXT("Vulkan")))
    {
        return EMagicSRRHIKind::Vulkan;
    }
    if (FCString::Stristr(Name, TEXT("D3D11")))
    {
        return EMagicSRRHIKind::D3D11;
    }
    if (FCString::Stristr(Name, TEXT("D3D12")))
    {
        return EMagicSRRHIKind::D3D12;
    }
    if (FCString::Stristr(Name, TEXT("OpenGL")))
    {
        return EMagicSRRHIKind::OpenGL;
    }
    if (FCString::Stristr(Name, TEXT("Metal")))
    {
        return EMagicSRRHIKind::Metal;
    }
    return EMagicSRRHIKind::Unknown;
}

static uint32 MapPixelFormat(EPixelFormat Format, EMagicSRRHIKind Kind)
{
    // Backend-native enums. Values match Vulkan / DXGI common UNORM/FLOAT forms.
    switch (Kind)
    {
    case EMagicSRRHIKind::Vulkan:
        switch (Format)
        {
        case PF_R8G8B8A8:
        case PF_R8G8B8A8_UINT:
            return 37; // VK_FORMAT_R8G8B8A8_UNORM
        case PF_B8G8R8A8:
            return 44; // VK_FORMAT_B8G8R8A8_UNORM
        case PF_FloatRGBA:
            return 97; // VK_FORMAT_R16G16B16A16_SFLOAT
        case PF_G16R16F:
            return 83; // VK_FORMAT_R16G16_SFLOAT
        case PF_R16F:
            return 77; // VK_FORMAT_R16_SFLOAT
        case PF_R32_FLOAT:
            return 100; // VK_FORMAT_R32_SFLOAT
        case PF_DepthStencil:
        case PF_ShadowDepth:
            return 126; // VK_FORMAT_D32_SFLOAT
        case PF_X24_G8:
            return 127; // VK_FORMAT_D32_SFLOAT_S8_UINT (approx)
        default:
            return 0;
        }
    case EMagicSRRHIKind::D3D11:
    case EMagicSRRHIKind::D3D12:
        switch (Format)
        {
        case PF_R8G8B8A8:
            return 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        case PF_B8G8R8A8:
            return 87; // DXGI_FORMAT_B8G8R8A8_UNORM
        case PF_FloatRGBA:
            return 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
        case PF_G16R16F:
            return 34; // DXGI_FORMAT_R16G16_FLOAT
        case PF_R16F:
            return 54; // DXGI_FORMAT_R16_FLOAT
        case PF_R32_FLOAT:
            return 41; // DXGI_FORMAT_R32_FLOAT
        case PF_DepthStencil:
        case PF_ShadowDepth:
            return 40; // DXGI_FORMAT_D32_FLOAT (common device depth)
        default:
            return 0;
        }
    case EMagicSRRHIKind::OpenGL:
        switch (Format)
        {
        case PF_R8G8B8A8:
        case PF_B8G8R8A8:
            return 0x8058; // GL_RGBA8
        case PF_G16R16F:
            return 0x822F; // GL_RG16F
        case PF_R32_FLOAT:
            return 0x822E; // GL_R32F
        case PF_DepthStencil:
            return 0x8CAC; // GL_DEPTH_COMPONENT32F
        default:
            return 0;
        }
    default:
        return 0;
    }
}

bool ResolveTexture(FRHITexture* Texture, FMagicSRNativeResource& Out)
{
    Out = FMagicSRNativeResource();
    if (Texture == nullptr)
    {
        return false;
    }

    void* Native = Texture->GetNativeResource();
    if (Native == nullptr)
    {
        return false;
    }

    const EMagicSRRHIKind Kind = GetKind();
    Out.Format = MapPixelFormat(Texture->GetFormat(), Kind);
    Out.Layout = 0;

    switch (Kind)
    {
    case EMagicSRRHIKind::Vulkan:
#if PLATFORM_WINDOWS || PLATFORM_LINUX
        if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan)
        {
            IVulkanDynamicRHI* VulkanRHI = GetIVulkanDynamicRHI();
            const FVulkanRHIImageViewInfo ViewInfo = VulkanRHI->RHIGetImageViewInfo(Texture);
            Out.VkImage = reinterpret_cast<uint64>(ViewInfo.Image ? ViewInfo.Image : VulkanRHI->RHIGetVkImage(Texture));
            Out.VkImageView = reinterpret_cast<uint64>(ViewInfo.ImageView);
            Out.Format = static_cast<uint32>(ViewInfo.Format ? ViewInfo.Format : VulkanRHI->RHIGetViewVkFormat(Texture));
        }
        else
#endif
        {
            // UE returns VkImage cast to void*.
            Out.VkImage = reinterpret_cast<uint64>(Native);
        }
        // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL — validator accepts when non-zero.
        Out.Layout = 5;
        Out.bValid = Out.VkImage != 0;
        break;
    case EMagicSRRHIKind::OpenGL:
        Out.GlTexture = static_cast<uint32>(reinterpret_cast<uintptr_t>(Native));
        Out.bValid = Out.GlTexture != 0;
        break;
    case EMagicSRRHIKind::D3D11:
    case EMagicSRRHIKind::D3D12:
    case EMagicSRRHIKind::Metal:
    default:
        Out.Pointer = Native;
        Out.bValid = Out.Pointer != nullptr;
        break;
    }
    return Out.bValid;
}

FRHITexture* ResolveFromGameResources(FTextureRenderTargetResource* RTResource, FTextureResource* TextureResource)
{
    if (RTResource != nullptr)
    {
        return RTResource->GetRenderTargetTexture();
    }
    if (TextureResource != nullptr)
    {
        return TextureResource->TextureRHI;
    }
    return nullptr;
}

#if MAGIC_SR_SUPPORTED
void FillColorResource(const FMagicSRNativeResource& Native, ::magic_resource_t& Out)
{
    FMemory::Memzero(&Out, sizeof(Out));
    Out.format = Native.Format;
    Out.layout = Native.Layout;
    Out.mip_count = 1;
    if (Native.VkImage != 0)
    {
        Out.handle.vk_image = Native.VkImage;
    }
    else if (Native.GlTexture != 0)
    {
        Out.handle.gl_texture = Native.GlTexture;
    }
    else
    {
        Out.handle.pointer = Native.Pointer;
    }
}

void FillDataHandle(const FMagicSRNativeResource& Native, ::magic_data_e& Out)
{
    FMemory::Memzero(&Out, sizeof(Out));
    if (Native.VkImage != 0)
    {
        Out.vk_image = Native.VkImage;
    }
    else if (Native.GlTexture != 0)
    {
        Out.gl_texture = Native.GlTexture;
    }
    else
    {
        Out.pointer = Native.Pointer;
    }
}
#endif
} // namespace MagicSRRHI
