# MagicSR Unreal Engine Plugin User Guide

## 1) Overview

The MagicSR Unreal Engine plugin is a bridge layer that exposes the MagicSR native super-resolution API to Unreal projects (Blueprint + C++). Native SDK **v2.0.0** adds temporal super-resolution through `MC_Init` / `MC_Process`; this plugin’s public Enable path remains **spatial** (one color texture, no per-frame depth/motion/jitter).

The plugin supports:

- Android (primary runtime backends: Vulkan and OpenGLES)
- iOS (Metal backend)

Core native libraries used by this plugin:

- Android: `lib/android/libmagic_sr_enable.a` (core + `MC_Enable`)
- iOS: `lib/ios/libmagic_sr_enable.a` (core + `MC_Enable`)

## 2) Deliverables

Main plugin deliverables:

- Plugin descriptor: `plugin/ue/MagicSR/MagicSR.uplugin`
- Module build + source: `plugin/ue/MagicSR/Source/MagicSR/`
- Blueprint API header: `plugin/ue/MagicSR/Source/MagicSR/Public/MagicSRBlueprintLibrary.h`

Sample project deliverables (smoke / QA only — not part of the published plugin):

- Sample project: `plugin/ue/Samples/AndroidSmokeTest/`
- Smoke runners live in the sample game module `Source/MagicSRUESmoke/` (not in `plugin/ue/MagicSR`)

Verified snapshot deliverables (by engine version/platform):

- `plugin/ue/verified/UE_<version>/Android/MagicSR`
- `plugin/ue/verified/UE_<version>/Android/Logs`
- `plugin/ue/verified/UE_<version>/IOS/...` (for iOS-verified versions)

## 3) Exposed Blueprint / C++ API

### One-call integration (recommended)

Generic GPU texture path only (camera / viewport / custom — same API):

```cpp
// C++
int64 OutTex = UMagicSRBlueprintLibrary::Enable(InputNativeTexture, /*Scale=*/1.5f);
// ... display / sample OutTex ...
UMagicSRBlueprintLibrary::Disable();  // only valid release for OutTex
```

Blueprint: **MagicSR → Easy → Enable** / **Disable**

`Scale` is float in `[1, 8]`; `<=0` defaults to `2.0`. Session is reused when scale/size are unchanged.

**Input format for Enable:** RGBA8Unorm (Metal / GLES) or RGB8Unorm (Vulkan).

For camera / SceneColor: resolve the color to a native GPU texture handle, then call the same `Enable(InputNativeTexture, Scale)`.

**Models:** put `.bin` files under project `Content/MagicSRModels/` for packaging, then at runtime copy to a readable absolute path and call:

- `SetModelDir(Dir)` = directory containing the bins, or
- `SetModelPath(Path)` = full path to one `.bin`

Android example that the SDK already searches: `/storage/emulated/0/Documents/MagicSRModels/`.
`Content/` is **not** auto-resolved by `Enable` at runtime. See User Guide §4.

User manual: [`doc/User_Guide.md`](../../doc/User_Guide.md) (English) · [`doc/用户使用说明书.md`](../../doc/用户使用说明书.md) (中文)

### Advanced API

**Preferred path (GPU texture pipelines):**

1. `Enable(InputNativeTexture, Scale)` — wraps `MC_Enable` (returns output texture)
   - also: `Enable_3params` / `Enable_4params`
   - `SetModelPath(Path)` / `SetModelDir(Dir)` — set model before Enable (preferred)   - `SetInputSizeHint(Width, Height)` — **required** on Android OpenGLES and Android Vulkan
2. `Disable()` — wraps `MC_Disable`
   - or `CreateSessionEx` + `ProcessUTexture` for explicit control
3. `ProcessUTexture(SessionId, InputUTexture, OutputUTexture)` — **recommended for game content**
   - or `ProcessNativeTexture(...)` when you already hold native GPU handles

> **Deprecated:** `EnableNative(Scale, InputNativeTexture)` uses the old **scale-first** argument order. Prefer `Enable(InputNativeTexture, Scale)`. `DisableNative()` → use `Disable()`.

`InputType` / `Backend` values (match `mc_interface.h`):

| InputType | Meaning |
|-----------|---------|
| 0 | Buffer (CPU) |
| 1 | TextureRgb8Unorm |
| 2 | TextureR8Unorm |

| Backend | Meaning |
|---------|---------|
| 2 | Neon (CPU) |
| 3 | Metal |
| 5 | OpenGLES |
| 6 | Vulkan |

`ProcessUTexture` resolves `UTexture` / `UTextureRenderTarget2D` RHI native handles on the render thread:

- **OpenGLES / Metal**: passes `GetNativeResource()` directly to `MC_Process`
- **Vulkan**: wraps UE’s `VkImage`-as-void* into a local `VkImage` storage so MagicSR’s `*(VkImage*)` convention works

Native texture handle conventions for `ProcessNativeTexture`:

- **OpenGLES**: pass `GLuint` texture id cast to `int64`
- **Metal**: pass `id<MTLTexture>` pointer cast to `int64`
- **Vulkan**: pass pointer-to-`VkImage` (or `VulkanTexture*`) cast to `int64`

**Fallback path (CPU buffer / Y8):**

1. `CreateSession(...)`  (Neon + Buffer)
2. `ProcessY8(SessionId, InputY, OutputY)`

All Blueprint-callable functions:

- `GetVersion()`
- `CreateSessionEx(...)` — **preferred for GPU**
- `CreateSession(...)` — buffer/Y8 helper
- `ProcessUTexture(...)` — **preferred high-level GPU path**
- `ProcessNativeTexture(...)` — low-level native handles
- `ProcessY8(...)` — buffer/Y8 only
- `SetParam(SessionId, ModelPath, Width, Height, Scale, AlgMode)`
- `QueryStatus(SessionId, OutStatus)`
- `DestroySession(SessionId)`

Status struct fields include output dimensions, backend, GPU time, and error code.

Mismatch / resolve guards:

- Calling `ProcessY8` on a texture session returns `-3004`
- Calling `ProcessNativeTexture` / `ProcessUTexture` on a buffer session returns `-3006`
- `ProcessUTexture` null textures `-3007`; RHI resolve fail `-3008`; unsupported RHI `-3009`

## 4) Integration Steps (UE Project)

1. Copy plugin folder into your UE project:
   - Target location: `<YourUEProject>/Plugins/MagicSR`
2. Ensure native libraries exist:
   - `lib/android/libmagic_sr_enable.a`
   - `lib/ios/libmagic_sr_enable.a`
3. Enable plugin in your `.uproject` if needed.
4. Re-generate project files / reopen editor.
5. Use Blueprint nodes from `MagicSR` category or call from C++.

## 5) Runtime Model Files

For Android runtime validation, put required model files into app-accessible storage:

- `magic_speed_gpu_params.bin` (Vulkan path)
- `magic_gles_speed_gpu_params.bin` (OpenGLES path)

For iOS runtime tests, package/push model files into app sandbox `Documents` path.

## 6) Runtime Validation (Recommended)

Validate on a physical target device and collect:

- runtime logs
- input/output images
- backend status and error codes

Use your normal UE package/install workflow for Android or iOS.

## 7) Visual Correctness Checklist

For each backend under test:

1. Confirm input image is saved (PGM/PNG).
2. Confirm output image is saved.
3. Confirm output size is expected (for x2 model, usually 128x128 from 64x64 input).
4. Confirm output is not all-zero (non-zero pixel count > 0).
5. Confirm log summary reports `failures=0`.

## 8) Troubleshooting

- **Missing library error**
  - Verify current libraries exist in `lib/android` and `lib/ios` (`libmagic_sr_enable.a`).
- **Android install signature mismatch**
  - Uninstall existing package `com.magicsr.uesmoke` and reinstall.
- **UE5.0 legacy Android build issues**
  - Use NDK r21 (`21.4.7075529`) for best compatibility.

