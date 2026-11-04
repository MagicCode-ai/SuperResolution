#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace MagicSRD3D11Bridge
{
/** Ensure a DXGI_FORMAT_R8G8B8A8_UNORM texture of WxH; recreate if needed. */
bool EnsureRgba8Unorm(
    ID3D11Device* Device,
    ID3D11Texture2D*& InOutTex,
    uint32 Width,
    uint32 Height,
    bool bRenderTarget);

/** Copy Src (any compatible 2D) into Dst UNORM texture. */
bool CopyTexture(ID3D11DeviceContext* Context, ID3D11Texture2D* Src, ID3D11Texture2D* Dst);

void ReleaseTexture(ID3D11Texture2D*& Tex);
}

#endif
