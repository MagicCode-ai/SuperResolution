#include "MagicSRD3D11Bridge.h"

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace MagicSRD3D11Bridge
{
bool EnsureRgba8Unorm(
    ID3D11Device* Device,
    ID3D11Texture2D*& InOutTex,
    uint32 Width,
    uint32 Height,
    bool bRenderTarget)
{
    if (Device == nullptr || Width == 0 || Height == 0)
    {
        return false;
    }

    if (InOutTex)
    {
        D3D11_TEXTURE2D_DESC Desc = {};
        InOutTex->GetDesc(&Desc);
        if (Desc.Width == Width && Desc.Height == Height && Desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            return true;
        }
        InOutTex->Release();
        InOutTex = nullptr;
    }

    D3D11_TEXTURE2D_DESC Desc = {};
    Desc.Width = Width;
    Desc.Height = Height;
    Desc.MipLevels = 1;
    Desc.ArraySize = 1;
    Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.Usage = D3D11_USAGE_DEFAULT;
    Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (bRenderTarget ? D3D11_BIND_RENDER_TARGET : 0);
    const HRESULT Hr = Device->CreateTexture2D(&Desc, nullptr, &InOutTex);
    return SUCCEEDED(Hr) && InOutTex != nullptr;
}

bool CopyTexture(ID3D11DeviceContext* Context, ID3D11Texture2D* Src, ID3D11Texture2D* Dst)
{
    if (Context == nullptr || Src == nullptr || Dst == nullptr)
    {
        return false;
    }
    Context->CopyResource(Dst, Src);
    return true;
}

void ReleaseTexture(ID3D11Texture2D*& Tex)
{
    if (Tex)
    {
        Tex->Release();
        Tex = nullptr;
    }
}
} // namespace MagicSRD3D11Bridge

#endif
