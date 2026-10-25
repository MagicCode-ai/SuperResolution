/************************************************************************************
 * Copyright (C) 2024-2026 MagicCode Technology Co., Ltd. All rights reserved.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file        mc_interface.h
 * @brief       API Interface Header File for MagicCode Super-Resolution (SR) Algorithm
 * @details     This file defines the core data structures and function interfaces
 *              for the MagicCode image super-resolution algorithm, including
 *              initialization, image processing, parameter control, resource release,
 *              and version query.
 * @author      MagicCode  Team
 * @date        2026-10-20
 * @version     V3.0.0
 * @copyright   Copyright (C) 2024-2026 MagicCode Technology Co., Ltd. All rights reserved.
 * @website     https://www.magiccode-ai.com
 ************************************************************************************/

#ifndef MC_INTERFACE_H
#define MC_INTERFACE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum input_type_e {
    INPUT_BUFFER = 0,            /* legacy CPU R8 buffer */
    INPUT_BUFFER_R8 = 0,         /* CPU R8 (alias of INPUT_BUFFER) */
    INPUT_TEXTURE_RGB8Unorm = 1,
    INPUT_TEXTURE_R8Unorm = 2,
    INPUT_BUFFER_RGB = 3,        /* CPU planar RGB (RRR...GGG...BBB...) */
    MAX_INPUT_TYPE
} input_type_e;

typedef enum alg_mode_e {
    SR_SPEED_MODE_FOR_IMAGE = 0,
    SR_BALANCED_MODE_FOR_IMAGE = 1,
    SR_SPEED_MODE_FOR_GRAPHIC = 2, /* temporal upscale in (1, 8]; spatial preprocess off.
                              * Default: derive missing reactive (and, on the
                              * Metal/Vulkan/GLES Speed path, missing
                              * transparency) and apply depth+MV history
                              * rejection. Explicit caller masks win per
                              * channel. Magic-temporal-family Speed (D3D11 / desktop GL)
                              * does not derive transparency from color.a. */
    SR_BALANCED_MODE_FOR_GRAPHIC = 3, /* temporal upscale in (1, 8]; spatial preprocess
                                 * off. Canonical Magic-temporal-family path. Derives
                                 * missing reactive and applies depth+MV
                                 * history rejection. Explicit caller masks
                                 * win per channel. Missing transparency binds
                                 * an internal zero texture; it is not a
                                 * semantic mask. */
    GEN_SPEED_MODE_FOR_GRAPHIC = 1 << 2, /* =4. FG-Fast (analytic, no weights).
                              * Vulkan + MAGIC_SR_HAS_FG only. Alone this equals
                              * SR_SPEED_MODE_FOR_IMAGE|GEN_SPEED_MODE_FOR_GRAPHIC
                              * because SR_SPEED_MODE_FOR_IMAGE is 0. */
    GEN_BALANCED_MODE_FOR_GRAPHIC = 1 << 3, /* =8. FG-CoreV3 (model_path = FG
                              * weight file; empty = default search).
                              * Vulkan + MAGIC_SR_HAS_FG only. */
    /* Explicit 16: without it the next implicit value would be 9, so
     * (alg_mode_e)MAX_ALG_MODE would look like a legal
     * SR_BALANCED_MODE_FOR_IMAGE|GEN_SPEED_MODE_FOR_GRAPHIC combo. */
    MAX_ALG_MODE = 16
} alg_mode_e;

#define MC_ALG_SR_MASK  0x3
#define MC_ALG_GEN_MASK 0xC
#define MC_ALG_SR_OF(m)  ((m) & MC_ALG_SR_MASK)
#define MC_ALG_GEN_OF(m) ((m) & MC_ALG_GEN_MASK)
#define MC_ALG_HAS_GEN(m) (MC_ALG_GEN_OF(m) != 0)

/*
 * Backend-neutral I/O resource. Color, output, depth, motion, reactive, and
 * transparency all use this type.
 *
 * handle is a union: exactly one member is live at a time. Which member is
 * interpreted is fixed by the create-time backend (and that backend's process
 * path) — never by inspecting unused bytes. Zero-initialize the resource so
 * unused union bytes are 0, then write the live member:
 *   CPU / Metal / D3D11 → handle.pointer
 *   Vulkan              → handle.vk_image
 *   GL / GLES           → handle.gl_texture
 * Do not store a GPU object and CPU float pixels in the same union. Resource
 * ownership stays with the caller; the library does not retain or free it.
 *
 * This struct has no width/height. After handle initialization, color/depth/
 * motion/reactive/transparency are expected to match the handle input size
 * and image_out the computed output size. Matching those sizes is a caller
 * contract: the library validates handle/format/layout/semantic metadata
 * and does not query or prove GPU image extents. Vulkan cannot query a
 * VkImage extent from the handle.
 *
 * format is the backend-native enum (VkFormat / MTLPixelFormat /
 * DXGI_FORMAT / GLenum). Zero means "not provided": the validator will
 * not guess a format from the GPU object on the per-frame path (no GPU
 * readback, no unsafe pointer dereference).
 *
 * v2 pre-release ABI break: the former parallel buffer/image/texture/
 * texture_name fields are not preserved.
 */
typedef union magic_data_e {
    void *pointer;        /* CPU buffer / MTLTexture* / ID3D11Texture2D* */
    uint64_t vk_image;    /* VkImage */
    uint32_t gl_texture;  /* GLuint */
} magic_data_e;

typedef struct magic_resource_t {
    magic_data_e handle;
    uint32_t format;        /* backend-native format */
    uint32_t layout;        /* VkImageLayout; zero for other backends */
    uint32_t target;        /* GLenum; zero means GL_TEXTURE_2D */
    uint32_t mip_count;     /* 0 is treated as 1 */
} magic_resource_t;

typedef struct mc_nscaler_input_frame_t mc_nscaler_input_frame_t;
typedef struct mc_nscaler_output_frame_t mc_nscaler_output_frame_t;

typedef enum magic_backend_e {
    MAGIC_BACKEND_DEFAULT = 0,
    MAGIC_BACKEND_X86 = 1,
    MAGIC_BACKEND_NEON = 2,
    MAGIC_BACKEND_METAL = 3,
    MAGIC_BACKEND_OPENGL = 4,
    MAGIC_BACKEND_OPENGLES = 5,
    MAGIC_BACKEND_VULKAN = 6,
    MAGIC_BACKEND_D3D11 = 7,
} magic_backend_e;

typedef enum log_level_e {
    MAGIC_LOG_NONE = 0,
    MAGIC_LOG_ERROR,
    MAGIC_LOG_WARNING,
    MAGIC_LOG_INFO,
    MAGIC_LOG_DEBUG
} log_level_e;

/*
 * Per-frame MV jitter is mc_mv_jitter_e on temporal_frame_t. There is no
 * public flags bitmask. Reversed-Z, infinite far, and HDR are create-stage
 * fields on ctrl_param_t. Depth is always GPU device depth
 * in [0, 1]. Sharpening is enable_sharpening plus sharpness. Internal
 * debug / A-B bits are not part of this header.
 */

/*
 * Stable GPU import state for ctrl_param_t. Copied by value into the handle and
 * immutable for the handle lifetime.
 * All-zero = library-owned / default / current-context behavior.
 *
 * Vulkan temporal has no library-owned VkDevice: physical_device + device
 * are required; get_device_proc_addr may be NULL (private dispatch loads
 * vulkan-1.dll). This struct is a frozen 5-pointer ABI: do not
 * append fields. Vulkan reuses the otherwise-unused D3D11/GL slots:
 *   native_context  = VkInstance (optional; needed for instance-level
 *                     queries on a host loader such as volk / Streamline)
 *   device_context  = PFN_vkGetInstanceProcAddr (optional host gipa;
 *                     do not mix a host gdpa with the system-loader gipa)
 * D3D11 temporal: device required; device_context may be NULL (immediate).
 * Metal: device may be NULL (MTLCreateSystemDefaultDevice).
 * GL / GLES: native_context is optional identity; the library never makes a
 * context current. First mc_nscaler_enable still needs a current GL context.
 * Spatial GPU: imported device is used where the backend already supports
 * zero-copy import (Metal). Vulkan/D3D11/GL spatial keep internally owned
 * or current-context devices when this struct is all-zero.
 *
 * A Metal/Vulkan command buffer is one-shot per frame and lives on
 * mc_nscaler_input_frame_t.command_buffer, not here.
 */
typedef struct magic_device_context_t {
    void *physical_device;      /* VkPhysicalDevice */
    void *device;               /* VkDevice / ID3D11Device* / MTLDevice* */
    void *device_context;       /* ID3D11DeviceContext* ; Vulkan: PFN_vkGetInstanceProcAddr */
    void *native_context;       /* HGLRC / EGLContext ; Vulkan: VkInstance */
    void *get_device_proc_addr; /* PFN_vkGetDeviceProcAddr */
} magic_device_context_t;


/**
 * @brief Whether stored MVs already include the current-frame jitter.
 * @details EXCLUDED (recommended): pass camera jitter in jitter_offset_*
 *          only. INCLUDED: stored MVs already include the current-frame
 *          jitter; the library applies jitter cancel.
 *
 *          0 (unspecified) is the handle canonical default: excluded.
 *          Only an explicit INCLUDED value means MVs include jitter.
 */
typedef enum mc_mv_jitter_e {
    MC_MV_JITTER_UNSPECIFIED = 0,
    MC_MV_JITTER_EXCLUDED = 1,
    MC_MV_JITTER_INCLUDED = 2
} mc_mv_jitter_e;

/**
 * @brief Reversed-Z vs conventional device depth.
 * @details camera_near / camera_far are physical view-space distances.
 *          Reversed-Z inverts the device-depth mapping only; it does not
 *          mean near > far.
 *          CONVENTIONAL: device depth 0 = near, 1 = far.
 *          REVERSED: device depth 1 = near, 0 = far.
 *          Finite conventional and finite reversed both require
 *          0 < camera_near < camera_far. Infinite far requires
 *          camera_near > 0 (camera_far == 0 is allowed).
 *
 *          Set on ctrl_param_t. 0 (unspecified) is the
 *          handle canonical default: conventional-Z. Immutable for the
 *          handle lifetime.
 */
typedef enum mc_depth_reversed_e {
    MC_DEPTH_REVERSED_UNSPECIFIED = 0,
    MC_DEPTH_REVERSED_CONVENTIONAL = 1,
    MC_DEPTH_REVERSED_YES = 2
} mc_depth_reversed_e;

/**
 * @brief Finite vs infinite far plane.
 * @details Infinite far requires camera_near > 0; camera_far == 0 is
 *          allowed. Finite still requires 0 < camera_near < camera_far.
 *          Set on ctrl_param_t. 0 (unspecified) is the
 *          handle canonical default: finite far. Immutable for the
 *          handle lifetime.
 */
typedef enum mc_depth_infinite_e {
    MC_DEPTH_INFINITE_UNSPECIFIED = 0,
    MC_DEPTH_INFINITE_FINITE = 1,
    MC_DEPTH_INFINITE_YES = 2
} mc_depth_infinite_e;

/**
 * @brief LDR vs HDR input color (create-stage, ctrl_param_t.hdr_color).
 * @details LDR: color is in a display-referred [0,1] range.
 *          HDR: color is HDR (not LDR [0,1]); MCTSR uses the HDR path.
 *          0 (unspecified) is the handle canonical default: LDR.
 *          Immutable for the handle lifetime.
 */
typedef enum mc_hdr_color_e {
    MC_HDR_COLOR_UNSPECIFIED = 0,
    MC_HDR_COLOR_LDR = 1,
    MC_HDR_COLOR_HDR = 2
} mc_hdr_color_e;

/**
 * @brief Backend-neutral extras for temporal processing in (1, 8].
 * @details Flattened per-frame extras then auxiliary resources. Color and
 *          output live on mc_nscaler_input_frame_t / mc_nscaler_output_frame_t.
 *          Stable GPU device state lives on ctrl_param_t.gpu_context (copied at
 *          create). Per-frame command_buffer lives on mc_nscaler_input_frame_t.
 *          Depth/motion/reactive/transparency use the handle's input
 *          width/height. The caller owns all handles.
 *
 *          ABI: fields through transparency are the current prefix. Tail
 *          fields after transparency are read only when struct_size covers
 *          them (see MC_TEMPORAL_FRAME_HAS_FIELD). Never read tail fields
 *          from a smaller prefix. 0 in a tail enum means "unspecified":
 *          mv_jitter 0 is excluded. Reversed-Z, infinite far, and HDR come
 *          from ctrl_param_t, not from this struct. Default
 *          motion_vector_scale (0,0) is the handle's current input size
 *          and follows resize. Per-frame explicit scale overrides.
 *
 *          Minimum per-frame usage:
 *            temporal_frame_t tf = {0};
 *            tf.struct_size = sizeof(tf);
 *            // bind depth/motion (and color/output on nscaler in/out frames)
 *            // set jitter / frame_index / camera as needed
 *            // optional: enable_sharpening, exposure_texture, pre_exposure
 *            // optional: reactive / transparency. Empty handles are valid.
 *            // Both temporal modes default to deriving a missing reactive
 *            // channel and applying depth+MV history rejection. The Speed
 *            // Metal/Vulkan/GLES path may also derive a missing
 *            // transparency approximation from motion-compensated color,
 *            // local contrast, MV divergence, short persistence, and
 *            // color.a / opaque-only evidence. An explicit texture wins
 *            // that channel. A derived transparency is not a semantic
 *            // mask; Magic-temporal-family paths (Balanced on every backend; Speed on
 *            // D3D11 and desktop GL) bind an internal zero transparency
 *            // when omitted. Diagnostic kill switches:
 *            // MAGICSR_TAAU_AUTO_MASK=0 and MAGICSR_TAAU_HIST_REJECT=0.
 *
 *          Fixed input contract (all backends; same idea as MagicSR/DLSS/MetalFX):
 *            - Motion is current → previous: current pixel + decoded MV
 *              lands on the previous frame. The library never negates MVs.
 *            - Jitter and output coordinates are top-left, +X right, +Y down.
 *              Jitter is in input pixels.
 *            - Depth must be GPU device depth in [0, 1]. Linear-view depth
 *              is not accepted; the library never converts it.
 *            - motion_vector_scale_x/y is the only MV numeric conversion.
 *              stored_mv * scale = input-pixel displacement in the contract
 *              above (current→previous, +Y down). (0,0) defaults to
 *              (input_width, input_height) for UV-stored MVs. Mixed zero
 *              (one axis 0) is rejected. Any other finite non-zero pair is
 *              allowed, including negatives (negative Y converts +Y-up
 *              storage to +Y-down). Pixel MVs use (1,1). UV MVs typically
 *              use (W,H).
 *
 *          Descriptor validation (internal, every mc_nscaler_enable temporal
 *          frame) checks native handles, format/layout when provided,
 *          and semantic metadata. It does not query GPU image sizes.
 *          Optional exposure_texture is caller-owned 1x1 R32F; 1x1 is a
 *          call contract because magic_resource_t has no width/height.
 */
typedef struct temporal_frame_t {
    uint32_t struct_size;          /* set to sizeof(temporal_frame_t) */
    float frame_time_delta_ms;
    uint32_t enable_sharpening;    /* non-zero: run the output sharpening pass */
    float sharpness;               /* [0,1] output sharpening strength; used when enable_sharpening != 0 */
    float jitter_offset_x;         /* subpixel jitter in input pixels, +X right */
    float jitter_offset_y;         /* +Y down */
    float motion_vector_scale_x;   /* stored MV * scale = input-pixel displacement; (0,0) → (W,H) */
    float motion_vector_scale_y;
    float pre_exposure;            /* current-frame pre-exposure; 0 means 1.0 */
    unsigned int frame_index;
    int reset_history;             /* non-zero: discard temporal history (load/teleport) */
    float camera_near;             /* physical view-space near (>0; GZDoom default 5) */
    float camera_far;              /* physical view-space far (>near if finite; GZDoom 65536) */
    float camera_fov_y;            /* vertical FOV in radians */
    float view_to_meters;          /* view-space unit to meters (default 1) */
    float cam_vel_px;              /* half-res camera MV (px), log only */
    magic_resource_t depth;
    magic_resource_t motion;
    magic_resource_t reactive;
    magic_resource_t transparency;
    /* --- tail: ignored unless struct_size covers each field --- */
    uint32_t mv_jitter;            /* mc_mv_jitter_e */
    /*
     * Optional caller-owned 1x1 R32F exposure texture. Empty native handle
     * uses the library's internal frame_info auto-exposure. When a native
     * handle is set, format must be the backend R32F
     * (VK_FORMAT_R32_SFLOAT / MTLPixelFormatR32Float / DXGI_FORMAT_R32_FLOAT
     * / GL_R32F); format 0 is rejected (the library does not guess).
     * 1x1 is a call contract: magic_resource_t has no width/height, so
     * size is not validated. The caller owns the resource for the frame.
     */
    magic_resource_t exposure_texture;
    /*
     * Optional: frames since the camera last moved (still-camera hint for TAAU).
     * 0 when unknown or when struct_size does not cover this field.
     */
    float frames_since_camera_still;
    /*
     * Optional unjittered ViewProjection for FG-Fast (column-major 4x4).
     * Clip space matches Fast pre.comp: NDC xy in [-1,1], z in [-1,1]
     * (OpenGL; near = -1), UV = clip.xy/w * 0.5 + 0.5. Depth bound on this
     * frame is GPU device depth in [0,1] with 0 = near (same temporal_frame_t
     * contract; CopyDepth 1-Z on UE reversed-Z).
     *
     * I0 fills vp0, I1 fills vp1. Either frame may carry the pair. GEN reads
     * with MC_TEMPORAL_FRAME_HAS_FIELD; have_view_projection != 0 means the
     * matrices on that frame are valid. Missing field or 0 keeps Fast
     * have_matrices=0 (MV-only, old behavior). Not a control command.
     */
    float vp0[16];
    float vp1[16];
    uint32_t have_view_projection;
} temporal_frame_t;

/** Byte size of the current prefix (fields through transparency). */
#define MC_TEMPORAL_FRAME_ABI_V2_PREFIX_SIZE \
    ((uint32_t)offsetof(temporal_frame_t, mv_jitter))
#define MC_TEMPORAL_FRAME_ABI_PREFIX_SIZE MC_TEMPORAL_FRAME_ABI_V2_PREFIX_SIZE

/** Non-zero if struct_size covers this field; never read a tail field otherwise. */
#define MC_TEMPORAL_FRAME_HAS_FIELD(frame, field) \
    ((frame) != NULL && (frame)->struct_size >= \
     (uint32_t)(offsetof(temporal_frame_t, field) + sizeof((frame)->field)))

/**
 * @brief Per-frame input color (and temporal extras).
 * @details Spatial modes may leave frame NULL. Temporal modes
 *          (SR_SPEED_MODE_FOR_GRAPHIC, SR_BALANCED_MODE_FOR_GRAPHIC) require
 *          frame != NULL. command_buffer is one-shot per frame
 *          (VkCommandBuffer / MTLCommandBuffer). NULL is valid where the
 *          backend submits internally (CPU, spatial GPU, Metal default,
 *          D3D11, GL/GLES). Vulkan temporal requires a recording buffer.
 *          Never cached as handle state.
 *
 *          width/height are the input pixel size [64, 4032]. First
 *          mc_nscaler_enable must supply a valid size; later calls use these
 *          to resize. 0,0 on a subsequent enable keeps the handle size.
 *
 *          GEN (Vulkan): in_frame[0]/[1] are I0/I1 hudless color at OUTPUT
 *          resolution. handle/format/layout/width/height are the Vulkan image
 *          (VkImage, VkFormat, entry VkImageLayout, extent). command_buffer
 *          on in_frame[0] is the recording VkCommandBuffer (same contract as
 *          Vulkan Temporal: no submit, no wait). Depth/motion stay on
 *          temporal_frame_t at render resolution. reset_history,
 *          frames_since_camera_still, and motion_vector_scale map to the
 *          internal FG path; callers do not pass FG-only structs.
 */
struct mc_nscaler_input_frame_t {
    magic_data_e handle;
    uint32_t format;        /* backend-native format */
    uint32_t layout;        /* VkImageLayout; zero for other backends */
    uint32_t target;        /* GLenum; zero means GL_TEXTURE_2D */
    uint32_t mip_count;     /* 0 is treated as 1 */
    unsigned int width;     /* input width in pixels; 0 keeps handle size */
    unsigned int height;    /* input height in pixels; 0 keeps handle size */
    temporal_frame_t *frame; /* NULL for spatial; required for temporal */
    void *command_buffer; /* VkCommandBuffer / MTLCommandBuffer; optional where backend can submit internally */
};

/**
 * @brief Per-frame output color.
 * @details width/height are the destination pixel size. 0,0 means the
 *          library output size from scaler_factor. When both are set they
 *          must match the handle output size after scaling.
 *          GEN (Vulkan): handle/format/layout/width/height are the generated
 *          It image at OUTPUT resolution (VkImage, VkFormat, entry layout).
 */
struct mc_nscaler_output_frame_t {
    magic_data_e handle;
    uint32_t format;        /* backend-native format */
    uint32_t layout;        /* VkImageLayout; zero for other backends */
    uint32_t target;        /* GLenum; zero means GL_TEXTURE_2D */
    uint32_t mip_count;     /* 0 is treated as 1 */
    unsigned int width;     /* output width in pixels; 0 uses handle output */
    unsigned int height;    /* output height in pixels; 0 uses handle output */
};

/**
 * @brief Control parameter structure for algorithm initialization and configuration
 * @details Runtime settings for mc_nscaler_control(SET_PARAM) and
 *          enable-as-create defaults. Image size lives on
 *          mc_nscaler_input_frame_t / mc_nscaler_output_frame_t.
 *          Zero-initialize then set named fields.
 *          After create, model_path, gpu_context, backend, depth_reversed,
 *          depth_infinite, and hdr_color are immutable: SET_PARAM ignores
 *          changes to those fields.
 */
#define MC_CTRL_PARAM_ABI_VERSION 2u

typedef struct ctrl_param_t {
    input_type_e input_type; //0 = buffer, 1 = r8_texture
    char model_path[256];          // SR: pre-trained model path (max 255 + NUL).
                                   // GEN: FG weight file; empty = MAGIC_FG_WEIGHT_PATH /
                                   // magic_fg_core.bin / model/magic_fg_core.bin.
    float scaler_factor;           // Requested super-resolution scaling factor. Spatial: [1, 8]. Temporal: (1, 8]. x86/neon accept implemented integer scales only.
    alg_mode_e alg_mode;         // SR bits 0..3; optional GEN_SPEED=4 or GEN_BALANCED=8 (not both). MAX_ALG_MODE=16.
    log_level_e log_level;
    magic_backend_e backend;       // Runtime backend selector: x86/neon/metal/opengl/opengles/vulkan.
    unsigned int spatial_sharpen_level; // Spatial sharpen grade [0, 5]. 0 = off, 5 = strongest.
                                   // RCAS attenuation = (5-level)*0.2 when RCAS is used; 0 skips RCAS.
                                   // SPATIAL_SPEED (1,2): 2dir LUT (segment 1+level) then lanczos2HV.
                                   // SPATIAL_SPEED exact-2x / (2,8]: combined-bin segment 1+level.
                                   // SPATIAL_BALANCED (1,2): MCSP1 EASU+RCAS (level drives RCAS; no LUT segment).
                                   // SPATIAL_BALANCED exact-2x / (2,8]: lut_boost (8th combined-bin segment).
                                   // RCAS-G when level>0 (level sets RCAS attenuation; no extra LUT slot).
                                   // Exact 2x (all backends): lut_boost then RCAS-G at 2x
                                   // (4dir flat skip on 1x G). (2,8]: RCAS-G at 1x before lut_boost.
    unsigned int enable_msaa;      // 0 = off, non-zero = on. Independent of LUT and sharpen.
                                   // When on: EASU2x → Lanczos3HV 1x, then the selected SR path
                                   // (e.g. SPEED 1.5x → 2dir 2x → lanczos2HV; BALANCED 2x → RCAS + lut_boost).
    magic_device_context_t gpu_context; /* all-zero = library-owned/default */
    /*
     * Create-stage temporal depth / HDR. Ignored for spatial modes.
     * Immutable for the handle lifetime. Leave all three as 0 for the
     * canonical defaults: conventional-Z, finite far, LDR color. Depth
     * is always GPU device depth in [0,1]; linear-view depth is not a
     * selectable encoding.
     *
     * depth_reversed:
     *   MC_DEPTH_REVERSED_CONVENTIONAL means 0=near and 1=far.
     *   MC_DEPTH_REVERSED_YES means reversed-Z: 1=near and 0=far.
     * depth_infinite:
     *   MC_DEPTH_INFINITE_FINITE requires 0 < camera_near < camera_far.
     *   MC_DEPTH_INFINITE_YES permits camera_far=0 and requires camera_near>0.
     * hdr_color:
     *   MC_HDR_COLOR_LDR for display-referred [0,1] color.
     *   MC_HDR_COLOR_HDR for HDR color (MCTSR HDR path).
     */
    uint32_t depth_reversed;       /* mc_depth_reversed_e */
    uint32_t depth_infinite;       /* mc_depth_infinite_e */
    uint32_t hdr_color;            /* mc_hdr_color_e */
    /* Optional create-time input size for SET_PARAM handle creation; 0 = library default. */
    unsigned int width;
    unsigned int height;
} ctrl_param_t;

/**
 * @brief Output status parameter structure for algorithm query
 * @details Stores the returned status and parameters when querying the algorithm,
 * including input/output image dimensions, runtime settings, and error code
 */
typedef struct output_status_params_t {
    unsigned int width;            // Width of the original input image (pixel units)
    unsigned int height;           // Height of the original input image (pixel units)
    unsigned int output_width;     // Width of the super-resolved output image (pixel units)
    unsigned int output_height;    // Height of the super-resolved output image (pixel units)
    float scaler_factor;           // Current requested super-resolution scaling factor in use
    alg_mode_e alg_mode;         // Current algorithm runtime mode
    input_type_e input_type;
    magic_backend_e backend;
    unsigned int num_threads;      // Current number of CPU threads in use
    unsigned int spatial_sharpen_level; // Spatial sharpen grade [0, 5]. 0 = off, 5 = strongest.
    double gpu_time;               /* last GPU time (seconds); GEN uses SR+FG where measured */
    unsigned int error_code;       // Algorithm error code: 0 = No error, non-zero = specific error (refer to error code specification)
    /*
     * Last successful temporal mc_nscaler_enable snapshot.
     * These are not a substitute for temporal_frame_t input. They record
     * the last frame that passed validation and backend encode/process
     * (same moment as frame_index accept). GPU resources, command_buffer,
     * and pointers are never stored. Values are the effective contract:
     * motion_vector_scale 0,0 stores the handle input size (W,H);
     * temporal_mv_jitter is the effective mc_mv_jitter_e (unspecified
     * input stores excluded). temporal_status_valid is 0 until the first
     * successful temporal process, and after handle creation or
     * resize/reinit. Spatial enable does not update these fields.
     * Validation or backend failure leaves the previous successful
     * snapshot unchanged. When valid is 0, per-frame temporal_* fields
     * below are 0. Create-stage depth_reversed / depth_infinite /
     * hdr_color are filled from the handle even when valid is 0, so
     * status_info can read the immutable contract.
     */
    uint32_t temporal_status_valid;
    uint32_t temporal_mv_jitter; /* effective mc_mv_jitter_e; 0 when valid==0 */
    float temporal_frame_time_delta_ms;
    float temporal_jitter_offset_x;
    float temporal_jitter_offset_y;
    float temporal_motion_vector_scale_x; /* effective; scale 0,0 → (W,H) */
    float temporal_motion_vector_scale_y;
    float temporal_pre_exposure;         /* effective; 0 input stored as 1.0 */
    unsigned int temporal_frame_index;
    int temporal_reset_history;
    float temporal_camera_near;
    float temporal_camera_far;
    float temporal_camera_fov_y;
    float temporal_view_to_meters;
    float temporal_cam_vel_px;
    uint32_t temporal_enable_sharpening; /* last-frame enable_sharpening */
    float temporal_sharpness;            /* last-frame sharpness */
    uint32_t temporal_depth_reversed;    /* create-stage effective */
    uint32_t temporal_depth_infinite;    /* create-stage effective */
    uint32_t temporal_hdr_color;         /* create-stage effective */
} output_status_params_t;

typedef enum mc_cmd_e {
    MC_NSCALER_CMD_SET_PARAM = 1,
    MC_NSCALER_CMD_QUERY_STATUS = 2
} mc_cmd_e;

/**
 * @brief Create (if *handle == NULL) and/or process one frame.
 * @param handle Address of the algorithm handle. *handle == NULL is create
 *        using in_frame[0] width/height and default ctrl (empty model_path,
 *        scaler 2, SR_SPEED_MODE_FOR_IMAGE). Prefer SET_PARAM create when
 *        ctrl is needed. *handle != NULL validates integrity then processes.
 *        in_frame[0] width/height [64, 4032] resize the handle when both are
 *        non-zero; 0,0 keeps the current size.
 * @param in_frame Pointer to two input-frame pointers.
 *        in_frame[0] is the color frame before the interpolate position
 *        (and the only input in non-GEN modes). NULL in_frame, or
 *        in_frame[0] == NULL, matches the old in_frame == NULL contract
 *        (MC_ERROR_PROCESS_NULL_IMAGE on create; skip process on a live handle).
 *        Create requires in_frame[0] with a valid size.
 *        Spatial may leave frame NULL; temporal requires frame != NULL.
 *        in_frame[1] is the color frame after the interpolate position.
 *        It is read only when a GEN_* alg_mode is selected; other modes
 *        ignore it (must not be read; may be NULL or a garbage pointer).
 *
 *        GEN (Vulkan only, library built with MAGIC_SR_HAS_FG):
 *          in_frame[1] == NULL → existing SR path (in_frame[0] is the input).
 *          in_frame[1] != NULL → interpolate t=0.5 between [0] and [1] into
 *            out_frame. Color of [0]/[1]/out is OUTPUT resolution (post-SR).
 *            Depth and motion stay at render (input) resolution; FG samples
 *            them with normalized UV so the library does not copy/stretch.
 *          GEN_SPEED → FG-Fast (analytic, no weights). GEN_BALANCED →
 *            FG-CoreV3 (ctrl_param_t.model_path is the FG weight file;
 *            empty uses MAGIC_FG_WEIGHT_PATH / magic_fg_core.bin /
 *            model/magic_fg_core.bin).
 *          Vulkan GEN records into in_frame[0].command_buffer, same contract
 *            as Vulkan Temporal (no submit, no wait). NULL command_buffer
 *            returns MC_ERROR_TEMPORAL_COMMAND_BUFFER.
 *          Other backends / builds without FG still return
 *            MC_ERROR_INIT_ALG_MODE_GEN_NOT_AVAILABLE (-100050) at create.
 * @param out_frame Output color. width/height 0,0 use the handle output size.
 *        Process runs only when both in and out native handles are present.
 * @return 0 on success; negative MC_ERROR_* on failure.
 */
int mc_nscaler_enable(void **handle, mc_nscaler_input_frame_t *in_frame[2],
                      mc_nscaler_output_frame_t *out_frame);

/**
 * @brief SET_PARAM or QUERY_STATUS.
 * @param handle Address of the algorithm handle.
 *        SET_PARAM with *handle == NULL creates the handle from ctrl
 *        (default input size 640x360 until the first enable supplies
 *        in_frame width/height). QUERY_STATUS never creates.
 * @param cmd MC_NSCALER_CMD_SET_PARAM or MC_NSCALER_CMD_QUERY_STATUS.
 * @param ctrl Required for SET_PARAM; ignored for QUERY_STATUS.
 *        SET_PARAM on a live handle ignores immutable fields
 *        (model_path, gpu_context, backend, depth_reversed,
 *        depth_infinite, hdr_color).
 * @param status_info Required for QUERY_STATUS; optional for SET_PARAM.
 * @return 0 on success; negative MC_ERROR_* on failure.
 */
int mc_nscaler_control(void **handle, mc_cmd_e cmd, ctrl_param_t *ctrl,
                       output_status_params_t *status_info);

/**
 * @brief Release all resources allocated by the algorithm.
 * @param handle Algorithm handle (NULL returns MC_ERROR_UNINIT_NULL_HANDLE).
 * @return 0 on success; negative error code on failure.
 */
int mc_nscaler_disable(void *handle);

/**
 * @brief Get the version string of the algorithm library.
 * @return Pointer to a static NUL-terminated version string; never NULL.
 *         The bytes live in this module; copy them before FreeLibrary/dlclose.
 */
char *mc_nscaler_version(void);

/* Public error/warning codes: appendix in mc_error.h (included here so callers that only include mc_interface.h keep compiling). */
#include "mc_error.h"

#ifdef __cplusplus
}
#endif
#endif
