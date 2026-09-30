# MagicCode Super-Resolution (MagicSR) User Guide

Version: v2.2.0 (`mc_nscaler_version()`)
Applies to: Native SDK
Platforms: Android (Vulkan / OpenGLES), iOS / macOS (Metal), Windows (Vulkan / Direct3D 11 / OpenGL 4.3), CPU (x86 / NEON, spatial)

中文版：[`用户使用说明书.md`](用户使用说明书.md)

Header: `interface/mc_interface.h`

---

## 1. Product Overview

MagicSR upscales a caller-owned image and writes the result into a caller-owned output. Spatial modes take one color image per frame. Temporal modes also take depth, motion vectors, jitter, and camera parameters.

There is one public API:

| Call | Role |
|------|------|
| `mc_nscaler_control(..., MC_NSCALER_CMD_SET_PARAM, &ctrl, NULL)` | Create the handle from `ctrl_param_t` when `*handle` is NULL. Later calls update mutable settings. |
| `mc_nscaler_enable(&handle, &in_frame, &out_frame)` | Process one frame. |
| `mc_nscaler_control(..., MC_NSCALER_CMD_QUERY_STATUS, NULL, &status)` | Read sizes, mode, `gpu_time`, and `error_code`. |
| `mc_nscaler_disable(handle)` | Release the handle. |
| `mc_nscaler_version()` | Static version string, currently `v2.2.0`. |

Zero-initialize every struct, then set the fields you use. The caller owns every GPU texture and CPU buffer. The library does not allocate the output image and does not free the caller's resources.

---

## 2. Libraries

Link **one** of the static archive or the dynamic library for the target. Both export the same `mc_nscaler_*` symbols. Include `interface/mc_interface.h`.

| Platform | Static | Dynamic |
|----------|--------|---------|
| iOS (arm64 device, iOS 18.4+) | `lib/ios/libmagic_sr.a` | `lib/ios/libmagic_sr.dylib` |
| Android (arm64-v8a) | `lib/android/libmagic_sr.a` | `lib/android/libmagic_sr.so` |
| macOS Apple Silicon (macOS 14+) | `lib/mac_arm/libmagic_sr.a` | `lib/mac_arm/libmagic_sr.dylib` |
| Windows (x86-64) | `lib/windows/libmagic_sr.lib` | `lib/windows/libmagic_sr.dll` |
| Linux (x86-64, CPU spatial) | `lib/linux/libmagic_sr.a` | `lib/linux/libmagic_sr.so` |

Apple static archives still need the system frameworks at app link time: Foundation, Metal, QuartzCore, MetalFX, MetalKit, MetalPerformanceShaders. The iOS and macOS dylibs already link those frameworks. Android `.so` already links `liblog`, `libGLESv3`, `libEGL`, and `libvulkan`.

iOS and macOS dylib install name: `@rpath/libmagic_sr.dylib`. How to embed the iOS dylib is in §8.

---

## 3. Quick Start (spatial, Metal)

```c
#include "mc_interface.h"
#include <string.h>

static void *g_handle;

int sr_init(const char *model_path)
{
    ctrl_param_t ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.input_type = INPUT_TEXTURE_RGB8Unorm;
    strncpy(ctrl.model_path, model_path, sizeof(ctrl.model_path) - 1);
    ctrl.scaler_factor = 2.0f;
    ctrl.alg_mode = SPATIAL_BALANCED_MODE;
    ctrl.backend = MAGIC_BACKEND_METAL;
    ctrl.log_level = MAGIC_LOG_ERROR;
    ctrl.spatial_sharpen_level = 0; /* 0..5 */
    ctrl.enable_msaa = 0;
    /* ctrl.gpu_context.device = MTLDevice*; NULL uses the system default */

    g_handle = NULL;
    return mc_nscaler_control(&g_handle, MC_NSCALER_CMD_SET_PARAM, &ctrl, NULL);
}

int sr_process(void *in_tex, void *out_tex,
               unsigned in_w, unsigned in_h,
               unsigned out_w, unsigned out_h,
               uint32_t pixel_format)
{
    mc_nscaler_input_frame_t in;
    mc_nscaler_output_frame_t out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle.pointer = in_tex;     /* MTLTexture* */
    in.format = pixel_format;       /* MTLPixelFormatRGBA8Unorm */
    in.width = in_w;
    in.height = in_h;
    out.handle.pointer = out_tex;
    out.format = pixel_format;
    out.width = out_w;
    out.height = out_h;
    return mc_nscaler_enable(&g_handle, &in, &out);
}

void sr_shutdown(void)
{
    mc_nscaler_disable(g_handle);
    g_handle = NULL;
}
```

Other backends use the same calls. The live union member depends on the backend chosen at create time:

| Backend | Color handle | Notes |
|---------|--------------|-------|
| Metal, D3D11 | `handle.pointer` | `MTLTexture*` or `ID3D11Texture2D*` |
| Vulkan | `handle.vk_image` | Set `format` and `layout` |
| OpenGL / OpenGLES | `handle.gl_texture` | `GLuint`; `target` 0 means `GL_TEXTURE_2D` |
| x86 / NEON | `handle.pointer` | CPU buffer; `input_type` is `INPUT_BUFFER_R8` or `INPUT_BUFFER_RGB` |

The first `mc_nscaler_enable` must pass a non-zero `width` and `height` in `[64, 4032]`. A later call with `0, 0` keeps the current size. When both output dimensions are non-zero they must match the size produced from `scaler_factor` (each axis is `floor(value * scale + 0.5)`).

---

## 4. Lifetime

1. `SET_PARAM` with `*handle == NULL` creates the handle. `model_path`, `gpu_context`, `backend`, `depth_reversed`, `depth_infinite`, and `hdr_color` are fixed for that handle. A later `SET_PARAM` ignores changes to those fields.
2. `mc_nscaler_enable` reads the input and writes the output. Both images stay owned by the caller.
3. Repeating `enable` with the same size and scale reuses internal GPU resources.
4. `mc_nscaler_disable` frees the handle. Do not use it afterward.
5. Query status with `MC_NSCALER_CMD_QUERY_STATUS`. `output_status_params_t.error_code` is `0` on success. Negative `MC_ERROR_*` values are listed in `mc_interface.h`.

`SET_PARAM` before the first sized `enable` uses a placeholder input of 640×360 until that `enable` supplies the real size.

---

## 5. Scale, Mode, and Sharpen

`ctrl_param_t.scaler_factor`

| Mode | Range |
|------|-------|
| Spatial | `[1, 8]` |
| Temporal | `(1, 8]` (1.0 is rejected) |
| x86 / NEON | Implemented integer scales |

`ctrl_param_t.alg_mode`

| Value | Enum | Behavior |
|-------|------|----------|
| 0 | `SPATIAL_SPEED_MODE` | Spatial, throughput first |
| 1 | `SPATIAL_BALANCED_MODE` | Spatial, quality and cost balanced |
| 2 | `TEMPORAL_SPEED_MODE` | Temporal. See §9. |
| 3 | `TEMPORAL_BALANCED_MODE` | Temporal. See §9. |

`spatial_sharpen_level` is an integer in `[0, 5]`. `0` is off. Higher levels increase sharpening strength.

`enable_msaa` is independent. Non-zero runs an extra preprocess before the selected super-resolution path.

---

## 6. Textures and Size

GPU spatial input is `INPUT_TEXTURE_RGB8Unorm` (RGBA8). `INPUT_TEXTURE_R8Unorm` is the single-channel GPU path. CPU spatial uses `INPUT_BUFFER_R8` or `INPUT_BUFFER_RGB` (planar R, then G, then B).

`magic_resource_t` and the frame structs carry no automatic size query. Pass `width` and `height` on `mc_nscaler_input_frame_t`. Vulkan `format` is a `VkFormat`. `layout` 0 means undefined and fails temporal validation; set the layout the image actually has when `enable` runs.

GL / GLES do not make a context current. The calling thread must already have one on the first `enable`.

---

## 7. Model File

Set `ctrl_param_t.model_path` to an absolute path of a readable `.bin` before the creating `SET_PARAM`. The path is at most 255 characters plus a NUL. An empty path or a file that cannot be opened returns `MC_ERROR_INIT_MODEL_FILE_OPEN_FAILED` (`-100029`) or `MC_ERROR_INIT_LOAD_PARAMS_FAILED` (`-100012`).

The current combined model is `model/magic_sr_gpu_params.bin`. Spatial mode and sharpen select a segment inside that file. Pass that file's absolute path. The library does not search the working directory and does not read an environment variable for the model.

`tools/setup_models.sh` only copies `.bin` files. After it runs, the app still sets `model_path` to the absolute path of the file it will load.

---

## 8. Dynamic Libraries

### 8.1 iOS

`lib/ios/libmagic_sr.dylib` is arm64, minimum iOS 18.4, install name `@rpath/libmagic_sr.dylib`. It is a device library.

1. Add the dylib to the app target.
2. Set **Frameworks, Libraries, and Embedded Content** to **Embed & Sign**. The copy lands in `YourApp.app/Frameworks/` and is re-signed.
3. Add `@executable_path/Frameworks` to **Runpath Search Paths**.
4. Add the `interface` directory to **Header Search Paths**.
5. Set the app deployment target to iOS 18.4 or later. Build for iphoneos.

The dylib must be loaded from inside the app bundle.

### 8.2 Android

`lib/android/libmagic_sr.so` is arm64-v8a. Ship it in `jniLibs/arm64-v8a` and load it with `System.loadLibrary`, or pass its path to the native link line. The static archive is `lib/android/libmagic_sr.a` when the app links MagicSR into its own `.so`.

### 8.3 macOS Apple Silicon

`lib/mac_arm/libmagic_sr.dylib` is arm64, minimum macOS 14, install name `@rpath/libmagic_sr.dylib`. Embed it in the app `Frameworks` folder and set the runpath to `@executable_path/../Frameworks` for a bundled app. A tool can link it with `-rpath` pointing at `lib/mac_arm`.

### 8.4 Windows

`lib/windows/libmagic_sr.dll` is x86-64. Link the import library `lib/windows/libmagic_sr.dll.lib`, and ship `libmagic_sr.dll` next to the executable (or on `PATH`). The static archive `lib/windows/libmagic_sr.lib` is the alternative when MagicSR is linked into the app binary.

### 8.5 Linux

`lib/linux/libmagic_sr.so` is x86-64. This build is the CPU spatial library. Link with `-Llib/linux -lmagic_sr` and an rpath such as `$ORIGIN` if the `.so` sits beside the executable, or pass the full path of the `.so` to the linker. Load it at runtime with `dlopen` only from a path the process is allowed to read. The static archive is `lib/linux/libmagic_sr.a`.

---

## 9. Temporal Super-Resolution

Temporal modes accumulate history from color, depth, and motion. They are `TEMPORAL_SPEED_MODE` and `TEMPORAL_BALANCED_MODE` on the same handle API. CPU backends reject them with `MC_ERROR_INIT_BACKEND_UNAVAILABLE`.

Create with `SET_PARAM`. Each `mc_nscaler_enable` must set `in_frame.frame` to a `temporal_frame_t` whose `struct_size` is `sizeof(temporal_frame_t)`.

| Field | Requirement |
|-------|-------------|
| `depth`, `motion` | GPU resources at the input size |
| `jitter_offset_x/y` | Input pixels, origin top-left, +X right, +Y down |
| `frame_index`, `reset_history` | Consecutive frames should be last+1. Non-zero `reset_history` drops history on a cut or teleport |
| `camera_near`, `camera_far`, `camera_fov_y` | View-space distances and vertical FOV in radians |
| `in_frame.command_buffer` | Vulkan: a command buffer that is already recording. Metal: NULL (library submits) or a caller `MTLCommandBuffer`. D3D11 / GL / GLES: NULL |

`motion_vector_scale_x/y` of `(0, 0)` means UV motion vectors scaled by the input size. Pixel motion vectors use `(1, 1)`. `mv_jitter` `0` means jitter is only in `jitter_offset_*`.

Depth is GPU device depth in `[0, 1]`. Reversed-Z, infinite far, and HDR are create-stage fields on `ctrl_param_t` (`depth_reversed`, `depth_infinite`, `hdr_color`). Leave them 0 for conventional-Z, finite far, and LDR.

Optional masks: an explicit `reactive` or `transparency` texture wins that channel. When omitted, the library derives an approximate mask. That derived mask is not a semantic transparency mask.

Vulkan records into the caller's command buffer and does not submit it. Wait for that GPU work before `mc_nscaler_disable` or a size change.

```c
int temporal_vk_init(VkPhysicalDevice phys, VkDevice dev, const char *model)
{
    ctrl_param_t ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.input_type = INPUT_TEXTURE_RGB8Unorm;
    strncpy(ctrl.model_path, model, sizeof(ctrl.model_path) - 1);
    ctrl.scaler_factor = 2.0f; /* (1, 8] */
    ctrl.alg_mode = TEMPORAL_SPEED_MODE;
    ctrl.backend = MAGIC_BACKEND_VULKAN;
    ctrl.log_level = MAGIC_LOG_ERROR;
    ctrl.gpu_context.physical_device = phys;
    ctrl.gpu_context.device = dev;
    g_handle = NULL;
    return mc_nscaler_control(&g_handle, MC_NSCALER_CMD_SET_PARAM, &ctrl, NULL);
}
```

Fill `mc_nscaler_input_frame_t` / `mc_nscaler_output_frame_t` as in §3, set `in.frame` to the temporal descriptor, set `in.command_buffer` to the recording buffer, and call `mc_nscaler_enable`.

| Backend | `gpu_context` | Command buffer |
|---------|---------------|----------------|
| Metal | `device` may be NULL | Optional |
| Vulkan | `physical_device` and `device` required | Recording buffer required |
| D3D11 | `device` required; `device_context` NULL means immediate | NULL |
| OpenGL / GLES | Current context | NULL |

---

## 10. FAQ

**Q: `SET_PARAM` or `enable` returns a negative code?**
A: Read `output_status_params_t.error_code` and the `MC_ERROR_*` list in `mc_interface.h`. Common create failures: model path (`-100029`, `-100012`), size outside `[64, 4032]` (`-100002`, `-100003`), scale out of range (`-100004`), GPU device missing (`-100025`).

**Q: Which library do I link?**
A: One file from §2. Static `.a` / `.lib` or the matching dynamic library. The header is always `mc_interface.h`.

**Q: Who allocates the output texture?**
A: The caller. Pass it on `mc_nscaler_output_frame_t`. Release it with the graphics API that created it, after `mc_nscaler_disable` or after the GPU work that used it has finished.

**Q: Can several handles run at once?**
A: Yes. Each `void *` from `SET_PARAM` is an independent handle.

**Q: Where does the model path go?**
A: `ctrl_param_t.model_path`, an absolute path, on the creating `SET_PARAM`. See §7.

**Q: What does sharpen do?**
A: `spatial_sharpen_level` is 0 through 5. 0 turns sharpening off. Higher values increase strength.

---

## 11. Deliverables

| Item | Path |
|------|------|
| Header | `interface/mc_interface.h` |
| iOS static / dynamic | `lib/ios/libmagic_sr.a`, `lib/ios/libmagic_sr.dylib` |
| Android static / dynamic | `lib/android/libmagic_sr.a`, `lib/android/libmagic_sr.so` |
| macOS arm static / dynamic | `lib/mac_arm/libmagic_sr.a`, `lib/mac_arm/libmagic_sr.dylib` |
| Windows static / dynamic | `lib/windows/libmagic_sr.lib`, `lib/windows/libmagic_sr.dll` (import library `lib/windows/libmagic_sr.dll.lib`) |
| Linux static / dynamic | `lib/linux/libmagic_sr.a`, `lib/linux/libmagic_sr.so` |
| Combined model | `model/magic_sr_gpu_params.bin` |
| License | `doc/版本文档/MagicCode Super-Resolution Software End User License Agreement (EULA).pdf` |

---

## 12. Support

Contact MagicCode support with the platform, `mc_nscaler_version()` string, backend, model filename, the `MC_ERROR_*` code, and a minimal project when possible.
