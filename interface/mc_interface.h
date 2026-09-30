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
 * @date        2026-08-28
 * @version     V2.0.0
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
    SPATIAL_SPEED_MODE = 0,
    SPATIAL_BALANCED_MODE = 1,
    TEMPORAL_SPEED_MODE = 2, /* temporal upscale in (1, 8]; spatial preprocess off.
                              * Default: derive missing reactive (and, on the
                              * Metal/Vulkan/GLES Speed path, missing
                              * transparency) and apply depth+MV history
                              * rejection. Explicit caller masks win per
                              * channel. FSR-family Speed (D3D11 / desktop GL)
                              * does not derive transparency from color.a. */
    TEMPORAL_BALANCED_MODE = 3, /* temporal upscale in (1, 8]; spatial preprocess
                                 * off. Canonical FSR-family path. Derives
                                 * missing reactive and applies depth+MV
                                 * history rejection. Explicit caller masks
                                 * win per channel. Missing transparency binds
                                 * an internal zero texture; it is not a
                                 * semantic mask. */
    MAX_ALG_MODE
} alg_mode_e;

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
 *          HDR: color is HDR (not LDR [0,1]); FSR3 uses the HDR path.
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
 *            // mask; FSR-family paths (Balanced on every backend; Speed on
 *            // D3D11 and desktop GL) bind an internal zero transparency
 *            // when omitted. Diagnostic kill switches:
 *            // MAGICSR_TAAU_AUTO_MASK=0 and MAGICSR_TAAU_HIST_REJECT=0.
 *
 *          Fixed input contract (all backends; same idea as FSR/DLSS/MetalFX):
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
 *          (TEMPORAL_SPEED_MODE, TEMPORAL_BALANCED_MODE) require
 *          frame != NULL. command_buffer is one-shot per frame
 *          (VkCommandBuffer / MTLCommandBuffer). NULL is valid where the
 *          backend submits internally (CPU, spatial GPU, Metal default,
 *          D3D11, GL/GLES). Vulkan temporal requires a recording buffer.
 *          Never cached as handle state.
 *
 *          width/height are the input pixel size [64, 4032]. First
 *          mc_nscaler_enable must supply a valid size; later calls use these
 *          to resize. 0,0 on a subsequent enable keeps the handle size.
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
    char model_path[256];          // File path of the pre-trained model (max 255 characters + null terminator)
    float scaler_factor;           // Requested super-resolution scaling factor. Spatial: [1, 8]. Temporal: (1, 8]. x86/neon accept implemented integer scales only.
    alg_mode_e alg_mode;         // 0 = SPATIAL_SPEED_MODE, 1 = SPATIAL_BALANCED_MODE, 2 = TEMPORAL_SPEED_MODE, 3 = TEMPORAL_BALANCED_MODE.
    log_level_e log_level;
    magic_backend_e backend;       // Runtime backend selector: x86/neon/metal/opengl/opengles/vulkan.
    unsigned int spatial_sharpen_level; // Spatial sharpen grade [0, 5]. 0 = off, 5 = strongest.
                                   // RCAS attenuation = (5-level)*0.2 when RCAS is used; 0 skips RCAS.
                                   // SPATIAL_SPEED (1,2): 2dir LUT (segment 1+level) then bilinear.
                                   // SPATIAL_SPEED exact-2x / (2,8]: combined-bin segment 1+level.
                                   // SPATIAL_BALANCED (1,2): 2dir LUT segment 1+level then lanczos2HV.
                                   //   Sharpen picks the segment. No FSR1 RCAS on this band.
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
     *   MC_HDR_COLOR_HDR for HDR color (FSR3 HDR path).
     */
    uint32_t depth_reversed;       /* mc_depth_reversed_e */
    uint32_t depth_infinite;       /* mc_depth_infinite_e */
    uint32_t hdr_color;            /* mc_hdr_color_e */
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
    double gpu_time;
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
 *        using in_frame width/height and default ctrl (empty model_path,
 *        scaler 2, SPATIAL_SPEED). Prefer SET_PARAM create when ctrl is needed.
 *        *handle != NULL validates integrity then processes.
 *        in_frame width/height [64, 4032] resize the handle when both are
 *        non-zero; 0,0 keeps the current size.
 * @param in_frame Input color. NULL with a live handle skips process.
 *        Create requires a non-NULL in_frame with a valid size.
 *        Spatial may leave frame NULL; temporal requires frame != NULL.
 * @param out_frame Output color. width/height 0,0 use the handle output size.
 *        Process runs only when both in and out native handles are present.
 * @return 0 on success; negative MC_ERROR_* on failure.
 */
int mc_nscaler_enable(void **handle, mc_nscaler_input_frame_t *in_frame,
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
 */
char *mc_nscaler_version(void);

/* Public error codes returned by MC_* APIs or exposed through output_status_params_t.error_code. */
#define MC_ERROR_INIT_NULL_PARAM                  (-100001) /* Create received a NULL ctrl_param_t pointer. */
#define MC_ERROR_INIT_WIDTH_OUT_OF_RANGE          (-100002) /* Initialization width is outside the supported range. */
#define MC_ERROR_INIT_HEIGHT_OUT_OF_RANGE         (-100003) /* Initialization height is outside the supported range. */
#define MC_ERROR_INIT_SCALER_OUT_OF_RANGE         (-100004) /* Initialization scaler factor is outside the supported range. */
#define MC_ERROR_INIT_ALG_MODE_OUT_OF_RANGE       (-100005) /* Initialization algorithm mode is invalid. */
#define MC_ERROR_INIT_INPUT_TYPE_OUT_OF_RANGE     (-100006) /* Initialization input type is invalid. */
#define MC_ERROR_INIT_BACKEND_OUT_OF_RANGE        (-100007) /* Initialization backend enum value is invalid. */
#define MC_ERROR_INIT_BACKEND_UNAVAILABLE         (-100008) /* Requested backend is not available in this build. */
#define MC_ERROR_INIT_CPU_BACKEND_INPUT_TYPE      (-100009) /* CPU backend was requested with non-buffer input. */
#define MC_ERROR_INIT_GPU_BACKEND_INPUT_TYPE      (-100010) /* GPU backend was requested with buffer input. */
#define MC_ERROR_INIT_MODEL_PATH_TOO_LONG         (-100011) /* Model path exceeds the supported length. */
#define MC_ERROR_INIT_LOAD_PARAMS_FAILED          (-100012) /* Model parameters could not be loaded. */
#define MC_ERROR_INIT_TYPE_SIZE_MISMATCH          (-100013) /* Model primary data type size does not match the backend. */
#define MC_ERROR_INIT_SPEED_TYPE_MISMATCH    (-100014) /* Speed model secondary data type size is invalid. */
#define MC_ERROR_INIT_BALANCED_TYPE_MISMATCH         (-100015) /* Balanced model secondary data type size is invalid. */
#define MC_ERROR_INIT_QUANT_SCALER_INVALID        (-100016) /* Quantization scaler is incompatible with the current build. */
#define MC_ERROR_INIT_STORE_MODE_INVALID          (-100017) /* Model store mode is incompatible with data type sizes. */
#define MC_ERROR_INIT_NEON_QUANT_SHIFT_MISMATCH   (-100018) /* NEON speed mode quantization shift is unsupported. */
#define MC_ERROR_INIT_CPU_FEATURE_UNSUPPORTED     (-100019) /* Required CPU feature is not available at runtime. */
#define MC_ERROR_INIT_FUNC_UNSUPPORTED_SCALER     (-100020) /* No processing function exists for the selected scaler. */
#define MC_ERROR_INIT_FUNC_ASSIGN_FAILED          (-100021) /* Processing function assignment failed. */
#define MC_ERROR_INIT_THREAD_START_SEM_FAILED     (-100022) /* Worker thread start semaphore initialization failed. */
#define MC_ERROR_INIT_THREAD_DONE_SEM_FAILED      (-100023) /* Worker thread completion semaphore initialization failed. */
#define MC_ERROR_INIT_THREAD_CREATE_FAILED        (-100024) /* Worker thread creation failed. */
#define MC_ERROR_INIT_GPU_CREATE_FAILED           (-100025) /* GPU backend context creation failed. */
#define MC_ERROR_INIT_REPORT_THREAD_LOCK_FAILED   (-100026) /* Report thread mutex initialization failed. */
#define MC_ERROR_INIT_REPORT_THREAD_SEM_FAILED    (-100027) /* Report thread semaphore initialization failed. */
#define MC_ERROR_INIT_REPORT_THREAD_CREATE_FAILED (-100028) /* Report thread creation failed. */
#define MC_ERROR_INIT_MODEL_FILE_OPEN_FAILED      (-100029) /* Model file could not be opened. */
#define MC_ERROR_INIT_MODEL_READ_TAG_FAILED       (-100030) /* Failed to read model backend tag. */
#define MC_ERROR_INIT_MODEL_READ_TYPE_SIZE_FAILED (-100031) /* Failed to read model primary data type size. */
#define MC_ERROR_INIT_MODEL_READ_TYPE2_SIZE_FAILED (-100032) /* Failed to read model secondary data type size. */
#define MC_ERROR_INIT_MODEL_READ_QUANT_FAILED     (-100033) /* Failed to read model quantization factor. */
#define MC_ERROR_INIT_MODEL_READ_DIM_FAILED       (-100034) /* Failed to read model LUT dimension. */
#define MC_ERROR_INIT_MODEL_READ_LUT_NUMS_FAILED  (-100035) /* Failed to read model LUT count. */
#define MC_ERROR_INIT_MODEL_READ_LUT_LEN_FAILED   (-100036) /* Failed to read model primary LUT length. */
#define MC_ERROR_INIT_MODEL_READ_LUT2_LEN_FAILED  (-100037) /* Failed to read model secondary LUT length. */
#define MC_ERROR_INIT_MODEL_READ_SCALER_FAILED    (-100038) /* Failed to read model scaler factor. */
#define MC_ERROR_INIT_MODEL_READ_STORE_MODE_FAILED (-100039) /* Failed to read model store mode. */
#define MC_ERROR_INIT_MODEL_READ_TAB_SIZE_FAILED  (-100040) /* Failed to read fast x2 model table size. */
#define MC_ERROR_INIT_MODEL_TAB_SIZE_INVALID      (-100041) /* Fast x2 model table size is invalid. */
#define MC_ERROR_INIT_MODEL_TAG_INVALID           (-100042) /* Model backend tag is invalid. */
#define MC_ERROR_INIT_MODEL_LUT_NUMS_INVALID      (-100043) /* Model LUT count is unsupported. */
#define MC_ERROR_INIT_MODEL_SCALER_MISMATCH       (-100044) /* Model scaler factor does not match requested scaler. */
#define MC_ERROR_INIT_MODEL_STORE_MODE_INVALID    (-100045) /* Model store mode value is unsupported. */
#define MC_ERROR_INIT_MODEL_DATA_SHORT_READ       (-100046) /* Model payload is shorter than expected. */
#define MC_ERROR_INIT_MODEL_TAIL_INVALID          (-100047) /* Model tail marker is invalid. */
#define MC_ERROR_INIT_SHARPEN_LEVEL_OUT_OF_RANGE  (-100048) /* spatial_sharpen_level is outside [0, 5]. */
#define MC_ERROR_INIT_STRUCT_SIZE                 (-100049) /* Reserved. */
#define MC_ERROR_MEMORY_INIT_PARAMS_ALLOC_FAILED  (-200001) /* Memory allocation failed while parsing model parameters. */
#define MC_ERROR_MEMORY_INIT_REINIT_ALLOC_FAILED  (-200002) /* Memory allocation failed during reinitialization. */
#define MC_ERROR_MEMORY_INIT_THREAD_ALLOC_FAILED  (-200003) /* Memory allocation failed during thread pool initialization. */
#define MC_ERROR_MEMORY_INIT_FAST_X2_MODEL_ALLOC_FAILED (-200004) /* Memory allocation failed for fast x2 model data. */
#define MC_ERROR_MEMORY_INIT_MODEL_DATA_ALLOC_FAILED (-200005) /* Memory allocation failed for model payload data. */

#define MC_ERROR_PROCESS_NULL_HANDLE              (-101001) /* mc_nscaler_enable received a NULL handle. */
#define MC_ERROR_PROCESS_HANDLE_CORRUPTED         (-101002) /* mc_nscaler_enable detected an invalid handle guard value. */
#define MC_ERROR_PROCESS_NULL_IMAGE               (-101003) /* mc_nscaler_enable received a NULL input or output image. */
#define MC_ERROR_PROCESS_CPU_FUNC_MISSING         (-101004) /* CPU processing function pointers are not initialized. */
#define MC_ERROR_PROCESS_TEXTURE_TYPE_INVALID     (-101005) /* Texture input or output pointer is invalid. */
#define MC_ERROR_PROCESS_TEXTURE_TYPE_CONFLICT    (-101006) /* Texture type conflicts with the configured input type. */
#define MC_ERROR_PROCESS_GPU_SR_FAILED            (-101007) /* GPU super-resolution stage failed. */
#define MC_ERROR_PROCESS_GPU_RESIZE_FAILED        (-101008) /* GPU resize/post-filter stage failed. */
#define MC_ERROR_PROCESS_GPU_POST_FAILED          (-101009) /* GPU post/upscale stage failed. */

#define MC_ERROR_ENABLE_NULL_HANDLE               MC_ERROR_PROCESS_NULL_HANDLE
#define MC_ERROR_ENABLE_HANDLE_CORRUPTED          MC_ERROR_PROCESS_HANDLE_CORRUPTED
#define MC_ERROR_ENABLE_NULL_IMAGE                MC_ERROR_PROCESS_NULL_IMAGE
#define MC_ERROR_ENABLE_CPU_FUNC_MISSING          MC_ERROR_PROCESS_CPU_FUNC_MISSING
#define MC_ERROR_ENABLE_TEXTURE_TYPE_INVALID      MC_ERROR_PROCESS_TEXTURE_TYPE_INVALID
#define MC_ERROR_ENABLE_TEXTURE_TYPE_CONFLICT     MC_ERROR_PROCESS_TEXTURE_TYPE_CONFLICT
#define MC_ERROR_ENABLE_GPU_SR_FAILED             MC_ERROR_PROCESS_GPU_SR_FAILED
#define MC_ERROR_ENABLE_GPU_RESIZE_FAILED         MC_ERROR_PROCESS_GPU_RESIZE_FAILED
#define MC_ERROR_ENABLE_GPU_POST_FAILED           MC_ERROR_PROCESS_GPU_POST_FAILED

#define MC_ERROR_TEMPORAL_NULL_FRAME              (-105001) /* Nested temporal_frame_t pointer is NULL. */
#define MC_ERROR_TEMPORAL_STRUCT_SIZE             (-105002) /* temporal_frame_t.struct_size is below the required prefix. */
#define MC_ERROR_TEMPORAL_MISSING_COLOR           (-105003) /* Color (image_in) native handle is missing. */
#define MC_ERROR_TEMPORAL_MISSING_OUTPUT          (-105004) /* Output (image_out) native handle is missing. */
#define MC_ERROR_TEMPORAL_MISSING_DEPTH           (-105005) /* Depth native handle is missing. */
#define MC_ERROR_TEMPORAL_MISSING_MOTION          (-105006) /* Motion-vector native handle is missing. */
#define MC_ERROR_TEMPORAL_NONFINITE               (-105007) /* jitter, scale, near/far, pre_exposure, or frame time is NaN/Inf. */
#define MC_ERROR_TEMPORAL_CAMERA_NEAR_FAR         (-105008) /* camera_near / camera_far relationship is invalid. */
#define MC_ERROR_TEMPORAL_MV_DIRECTION            (-105009) /* Reserved; motion is always current→previous. */
#define MC_ERROR_TEMPORAL_MV_UNITS                (-105010) /* Reserved; scale is the only MV numeric conversion. */
#define MC_ERROR_TEMPORAL_MV_SCALE                (-105011) /* motion_vector_scale is mixed-zero or otherwise invalid. */
#define MC_ERROR_TEMPORAL_COORD_SPACE             (-105012) /* Reserved; coordinates are top-left, +X right, +Y down. */
#define MC_ERROR_TEMPORAL_DEPTH_ENCODING          (-105013) /* Reserved; depth is GPU device depth [0,1]. */
#define MC_ERROR_TEMPORAL_METADATA_CONFLICT       (-105014) /* Tail semantics disagree with temporal flags. */
#define MC_ERROR_TEMPORAL_FORMAT                  (-105015) /* Caller-provided format is not accepted for this slot. */
#define MC_ERROR_TEMPORAL_COMMAND_BUFFER          (-105017) /* Vulkan temporal requires a recording command buffer. */

#define MC_WARNING_TEMPORAL_FRAME_INDEX           (105001) /* frame_index is not last+1; history may be stale. */

#define MC_ERROR_CONTROL_NULL_HANDLE              (-102001) /* mc_nscaler_control received a NULL handle. */
#define MC_ERROR_CONTROL_HANDLE_CORRUPTED         (-102002) /* mc_nscaler_control detected an invalid handle guard value. */
#define MC_ERROR_CONTROL_CMD_OUT_OF_RANGE         (-102003) /* Control command is outside the supported range. */
#define MC_ERROR_CONTROL_NULL_PARAMS              (-102004) /* SET_PARAM received a NULL ctrl_param_t pointer. */
#define MC_ERROR_CONTROL_WIDTH_OUT_OF_RANGE       (-102005) /* Control width is outside the supported range. */
#define MC_ERROR_CONTROL_HEIGHT_OUT_OF_RANGE      (-102006) /* Control height is outside the supported range. */
#define MC_ERROR_CONTROL_SCALER_OUT_OF_RANGE      (-102007) /* Control scaler factor is outside the supported range. */
#define MC_ERROR_CONTROL_ALG_MODE_OUT_OF_RANGE    (-102008) /* Control algorithm mode is invalid. */
#define MC_ERROR_CONTROL_MODEL_PATH_TOO_LONG      (-102009) /* Control model path exceeds the supported length. */
#define MC_ERROR_CONTROL_INIT_FUNCS_FAILED        (-102010) /* Processing function re-assignment failed during control. */
#define MC_ERROR_CONTROL_REINIT_FAILED            (-102011) /* Full reinitialization failed during control. */
#define MC_ERROR_CONTROL_NULL_OUTPUT              (-102012) /* QUERY_STATUS command received a NULL output pointer. */
#define MC_ERROR_MEMORY_CONTROL_ALLOC_FAILED      (-202001) /* Memory allocation failed during control. */

#define MC_ERROR_UNINIT_NULL_HANDLE               (-103001) /* mc_nscaler_disable received a NULL handle. */
#define MC_ERROR_UNINIT_HANDLE_CORRUPTED          (-103002) /* mc_nscaler_disable detected an invalid handle guard value. */

#define MC_ERROR_DISABLE_NULL_HANDLE              MC_ERROR_UNINIT_NULL_HANDLE
#define MC_ERROR_DISABLE_HANDLE_CORRUPTED         MC_ERROR_UNINIT_HANDLE_CORRUPTED
#define MC_ERROR_MEMORY_UNINIT_DOUBLE_FREE        (-203001) /* Memory release count indicates a double free. */

#define MC_ERROR_REPORT_INVALID_PARAM                (-104001) /* Report packet arguments are invalid. */
#define MC_ERROR_REPORT_INVALID_SERVER_IP            (-104002) /* Report server IP address is invalid. */
#define MC_ERROR_REPORT_SOCKET_ENV_INIT_FAILED       (-104003) /* Socket environment initialization failed. */
#define MC_ERROR_REPORT_SOCKET_CREATE_FAILED         (-104004) /* Report socket creation failed. */
#define MC_ERROR_REPORT_SOCKET_REUSEADDR_FAILED      (-104005) /* Setting SO_REUSEADDR on the report socket failed. */
#define MC_ERROR_REPORT_SOCKET_SEND_TIMEOUT_FAILED   (-104006) /* Setting report socket send timeout failed. */
#define MC_ERROR_REPORT_SOCKET_RECV_TIMEOUT_FAILED   (-104007) /* Setting report socket receive timeout failed. */
#define MC_ERROR_REPORT_CONNECT_FAILED               (-104008) /* Connecting to the report server failed. */
#define MC_ERROR_REPORT_SEND_FAILED                  (-104009) /* Sending the report packet failed. */
#define MC_ERROR_REPORT_PARTIAL_SEND                 (-104010) /* Report packet was only partially sent. */

#define MC_WARNING_MEMORY_LEAK_ON_FREE            (100001) /* Memory leak detected while freeing an internal handle. */
#define MC_WARNING_MEMORY_LEAK_ON_UNINIT          (100002) /* Memory leak detected while uninitializing the API handle. */


#ifdef __cplusplus
}
#endif
#endif
