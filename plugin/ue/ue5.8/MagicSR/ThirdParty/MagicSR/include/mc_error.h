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
 * @file        mc_error.h
 * @brief       Public MC_ERROR_* / MC_WARNING_* codes for the MagicCode nscaler API
 * @details     Appendix to mc_interface.h. Numeric values are public ABI and must
 *              not be renumbered. mc_interface.h includes this file, so callers
 *              that include only mc_interface.h keep compiling. Internal FG
 *              implementation codes (MC_ERROR_FG_* in src/magic_fg_common.h,
 *              -106001..) are not part of this header.
 * @author      MagicCode  Team
 * @date        2026-10-24
 * @version     V3.0.0
 * @copyright   Copyright (C) 2024-2026 MagicCode Technology Co., Ltd. All rights reserved.
 * @website     https://www.magiccode-ai.com
 ************************************************************************************/

#ifndef MC_ERROR_H
#define MC_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Appendix: public error and warning codes belonging to the nscaler API.
 *
 * Canonical location: interface/mc_error.h
 * Included by mc_interface.h (same-directory quoted include).
 *
 * GEN public FG semantics live here with the rest of the public codes:
 *   MC_ERROR_INIT_ALG_MODE_GEN_NOT_AVAILABLE (-100050)
 *   MC_ERROR_INIT_FG_WEIGHTS                 (-100051)
 *   MC_ERROR_INIT_FG_CREATE_FAILED           (-100052)
 *   MC_ERROR_CONTROL_ALG_MODE_GEN_NOT_AVAILABLE (-102013)
 *   MC_ERROR_CONTROL_FG_WEIGHTS               (-102014)
 *   MC_ERROR_CONTROL_FG_CREATE_FAILED         (-102015)
 */

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
#define MC_ERROR_INIT_ALG_MODE_GEN_NOT_AVAILABLE (-100050) /* GEN_* is not available (non-Vulkan or no FG in this build). */
#define MC_ERROR_INIT_FG_WEIGHTS                  (-100051) /* GEN_BALANCED: FG weight file missing or invalid. */
#define MC_ERROR_INIT_FG_CREATE_FAILED            (-100052) /* Vulkan FG handle/pipeline create failed. */
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
#define MC_ERROR_CONTROL_ALG_MODE_GEN_NOT_AVAILABLE (-102013) /* GEN_* is not available (non-Vulkan or no FG in this build). */
#define MC_ERROR_CONTROL_FG_WEIGHTS               (-102014) /* GEN_BALANCED: FG weight file missing or invalid (SET_PARAM). */
#define MC_ERROR_CONTROL_FG_CREATE_FAILED         (-102015) /* Vulkan FG handle/pipeline create failed during SET_PARAM. */
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
