#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <climits>

extern "C" {
#include "mc_interface.h"
}

#define LOG_TAG "MagicMagnifierJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Demo JNI wrapper codes (distinct from MC_ERROR_* which are <= -100001). */
enum {
    JNI_ERR_NO_HANDLE = -1,
    JNI_ERR_NULL_ARGS = -2,
    JNI_ERR_CHANNEL = -3,
    JNI_ERR_BACKEND = -4,
    JNI_ERR_DIM = -5,
    JNI_ERR_READBACK = -6,
    JNI_ERR_INPUT_LEN = -7,
    JNI_ERR_OUTPUT_LEN = -8,
    JNI_ERR_SESSION_QUERY = -9,
    JNI_ERR_SESSION_INPUT = -10,
    JNI_ERR_SESSION_OUTPUT = -11,
    JNI_ERR_EGL = -12
};

static void *g_sr_handle = nullptr;
static magic_backend_e g_backend = MAGIC_BACKEND_OPENGLES;
static float g_scale = 2.0f;
static alg_mode_e g_mode = SPATIAL_SPEED_MODE;
static unsigned int g_session_in_w = 0;
static unsigned int g_session_in_h = 0;
static unsigned int g_session_out_w = 0;
static unsigned int g_session_out_h = 0;
static bool g_logged_first_process = false;

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static EGLSurface g_surface = EGL_NO_SURFACE;

static GLuint g_input_tex = 0;
static GLuint g_output_tex = 0;
static int g_input_w = 0;
static int g_input_h = 0;
static int g_output_w = 0;
static int g_output_h = 0;

/* Same rounding as product src/magic_backend.h scaled_dimension. */
static uint32_t scaled_dimension(uint32_t value, float scaler) {
    double scaled = (double)value * (double)scaler;
    if (scaled < 1.0) return 1;
    if (scaled > (double)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)floor(scaled + 0.5);
}

static int rgba_need_bytes(jint w, jint h, size_t *out_bytes) {
    if (w <= 0 || h <= 0 || !out_bytes) return -1;
    const size_t sw = (size_t)w;
    const size_t sh = (size_t)h;
    if (sw > SIZE_MAX / 4u) return -1;
    const size_t row = sw * 4u;
    if (sh > SIZE_MAX / row) return -1;
    *out_bytes = row * sh;
    return 0;
}

static int java_array_covers(JNIEnv *env, jbyteArray arr, size_t need) {
    if (!env || !arr) return 0;
    const jsize n = env->GetArrayLength(arr);
    if (n < 0) return 0;
    /* Java arrays cannot exceed Integer.MAX_VALUE bytes. */
    if (need > (size_t)INT32_MAX) return 0;
    return (size_t)n >= need;
}

static bool make_gles_current() {
    if (g_display == EGL_NO_DISPLAY || g_context == EGL_NO_CONTEXT || g_surface == EGL_NO_SURFACE) {
        return false;
    }
    if (eglMakeCurrent(g_display, g_surface, g_surface, g_context) != EGL_TRUE) {
        LOGE("eglMakeCurrent failed err=0x%x", (unsigned)eglGetError());
        return false;
    }
    return true;
}

static void destroy_egl() {
    if (g_display != EGL_NO_DISPLAY) {
        if (eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
            LOGE("eglMakeCurrent(NO_SURFACE) failed err=0x%x during EGL teardown",
                 (unsigned)eglGetError());
        }
        if (g_surface != EGL_NO_SURFACE) {
            eglDestroySurface(g_display, g_surface);
            g_surface = EGL_NO_SURFACE;
        }
        if (g_context != EGL_NO_CONTEXT) {
            eglDestroyContext(g_display, g_context);
            g_context = EGL_NO_CONTEXT;
        }
        eglTerminate(g_display);
        g_display = EGL_NO_DISPLAY;
    }
    g_context = EGL_NO_CONTEXT;
    g_surface = EGL_NO_SURFACE;
}

static bool create_gles_context() {
    eglBindAPI(EGL_OPENGL_ES_API);
    if (g_display != EGL_NO_DISPLAY || g_context != EGL_NO_CONTEXT || g_surface != EGL_NO_SURFACE) {
        destroy_egl();
    }

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }
    if (!eglInitialize(g_display, nullptr, nullptr)) {
        LOGE("eglInitialize failed err=0x%x", (unsigned)eglGetError());
        g_display = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint config_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint num = 0;
    if (!eglChooseConfig(g_display, config_attrs, &config, 1, &num) || num <= 0) {
        LOGE("eglChooseConfig failed");
        destroy_egl();
        return false;
    }

    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (g_context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        destroy_egl();
        return false;
    }

    const EGLint surf_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    g_surface = eglCreatePbufferSurface(g_display, config, surf_attrs);
    if (g_surface == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed");
        destroy_egl();
        return false;
    }

    if (!make_gles_current()) {
        destroy_egl();
        return false;
    }
    return true;
}

static GLuint gles_create_texture(int width, int height) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) return 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static void gles_destroy_texture(GLuint tex) {
    if (tex != 0) glDeleteTextures(1, &tex);
}

static void clear_gles_textures() {
    if (g_input_tex != 0) {
        gles_destroy_texture(g_input_tex);
        g_input_tex = 0;
    }
    if (g_output_tex != 0) {
        gles_destroy_texture(g_output_tex);
        g_output_tex = 0;
    }
    g_input_w = g_input_h = 0;
    g_output_w = g_output_h = 0;
}

static void forget_session_size() {
    g_session_in_w = g_session_in_h = 0;
    g_session_out_w = g_session_out_h = 0;
}

/* Order: mc_nscaler_disable, delete textures (current context), then destroy EGL. */
static void release_session() {
    if (g_sr_handle) {
        if (!make_gles_current()) {
            LOGE("eglMakeCurrent failed before mc_nscaler_disable; calling disable anyway");
        }
        const int ur = mc_nscaler_disable(g_sr_handle);
        if (ur != 0) {
            LOGE("mc_nscaler_disable failed ret=%d", ur);
        }
        g_sr_handle = nullptr;
    }
    if (g_display != EGL_NO_DISPLAY && make_gles_current()) {
        clear_gles_textures();
    } else {
        g_input_tex = g_output_tex = 0;
        g_input_w = g_input_h = g_output_w = g_output_h = 0;
    }
    destroy_egl();
    forget_session_size();
    g_logged_first_process = false;
}

static bool ensure_gles_io_textures(int in_w, int in_h, int out_w, int out_h) {
    if (!make_gles_current()) return false;
    if (g_input_tex != 0 && g_output_tex != 0 &&
        g_input_w == in_w && g_input_h == in_h &&
        g_output_w == out_w && g_output_h == out_h) {
        return true;
    }
    clear_gles_textures();
    g_input_tex = gles_create_texture(in_w, in_h);
    g_output_tex = gles_create_texture(out_w, out_h);
    if (g_input_tex == 0 || g_output_tex == 0) {
        clear_gles_textures();
        return false;
    }
    g_input_w = in_w;
    g_input_h = in_h;
    g_output_w = out_w;
    g_output_h = out_h;
    return true;
}

static void gles_upload_rgba(GLuint tex, int width, int height, const uint8_t *data) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static uint8_t *gles_readback_rgba(GLuint tex, int width, int height) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (fbo == 0) return nullptr;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return nullptr;
    }
    size_t bytes = 0;
    if (rgba_need_bytes(width, height, &bytes) != 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return nullptr;
    }
    uint8_t *out = (uint8_t *)malloc(bytes);
    if (!out) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return nullptr;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out);
    GLenum err = glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    if (err != GL_NO_ERROR) {
        free(out);
        return nullptr;
    }
    return out;
}

static int cache_session_size_from_handle() {
    output_status_params_t st;
    memset(&st, 0, sizeof(st));
    if (mc_nscaler_control(&g_sr_handle, MC_NSCALER_CMD_QUERY_STATUS, nullptr, &st) != 0) {
        return -1;
    }
    g_session_in_w = st.width;
    g_session_in_h = st.height;
    g_session_out_w = st.output_width;
    g_session_out_h = st.output_height;
    g_scale = st.scaler_factor;
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_superresolution_natives_SuperResolutionLib_initSuperResolution(
        JNIEnv *env,
        jclass,
        jint width,
        jint height,
        jfloat scaler_factor,
        jint sr_mode,
        jint sr_on_gpu,
        jint backend,
        jstring model_path) {
    (void)sr_on_gpu;
    if (width < 64 || height < 64 || scaler_factor < 1.0f) {
        LOGE("invalid init args w=%d h=%d scale=%f", width, height, scaler_factor);
        return 0;
    }

    release_session();

    g_backend = (magic_backend_e)backend;
    if (g_backend == MAGIC_BACKEND_DEFAULT) g_backend = MAGIC_BACKEND_OPENGLES;
    if (g_backend != MAGIC_BACKEND_OPENGLES) {
        LOGE("magnifier demo only supports OpenGLES session path, backend=%d", (int)g_backend);
        return 0;
    }
    g_scale = scaler_factor;
    g_mode = (sr_mode == (jint)SPATIAL_BALANCED_MODE) ? SPATIAL_BALANCED_MODE : SPATIAL_SPEED_MODE;

    if (!create_gles_context()) {
        LOGE("gles context create failed");
        return 0;
    }

    const char *model_path_cstr = model_path ? env->GetStringUTFChars(model_path, nullptr) : "";
    ctrl_param_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.input_type = INPUT_TEXTURE_RGB8Unorm;
    ctrl.scaler_factor = scaler_factor;
    ctrl.alg_mode = g_mode;
    ctrl.log_level = MAGIC_LOG_INFO;
    ctrl.backend = MAGIC_BACKEND_OPENGLES;
    ctrl.spatial_sharpen_level = 0;
    ctrl.gpu_context.native_context = (void *)g_context;
    if (model_path_cstr && model_path_cstr[0] != '\0') {
        strncpy(ctrl.model_path, model_path_cstr, sizeof(ctrl.model_path) - 1);
    }
    if (model_path) env->ReleaseStringUTFChars(model_path, model_path_cstr);

    g_sr_handle = nullptr;
    int init_rc = mc_nscaler_control(&g_sr_handle, MC_NSCALER_CMD_SET_PARAM, &ctrl, nullptr);
    if (init_rc != 0 || !g_sr_handle) {
        LOGE("mc_nscaler_control failed rc=%d scale=%.3f mode=%d %dx%d", init_rc, g_scale, (int)g_mode, width, height);
        release_session();
        return 0;
    }
    g_session_in_w = (unsigned)width;
    g_session_in_h = (unsigned)height;
    g_session_out_w = scaled_dimension((uint32_t)width, scaler_factor);
    g_session_out_h = scaled_dimension((uint32_t)height, scaler_factor);
    LOGI("mc_nscaler_control ok version=%s scale=%.3f mode=%d %ux%u -> %ux%u",
         mc_nscaler_version(), g_scale, (int)g_mode,
         g_session_in_w, g_session_in_h, g_session_out_w, g_session_out_h);
    return (jlong)(uintptr_t)g_sr_handle;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_superresolution_natives_SuperResolutionLib_processImage(
        JNIEnv *env,
        jclass,
        jbyteArray input,
        jbyteArray output,
        jint channel_num,
        jint input_width,
        jint input_height,
        jint output_width,
        jint output_height) {
    if (!g_sr_handle) return JNI_ERR_NO_HANDLE;
    if (!env || !input || !output) return JNI_ERR_NULL_ARGS;
    if (channel_num != 4) return JNI_ERR_CHANNEL;
    if (g_backend != MAGIC_BACKEND_OPENGLES) return JNI_ERR_BACKEND;
    if (input_width <= 0 || input_height <= 0 || output_width <= 0 || output_height <= 0) {
        return JNI_ERR_DIM;
    }

    size_t in_need = 0;
    size_t out_need = 0;
    if (rgba_need_bytes(input_width, input_height, &in_need) != 0) return JNI_ERR_INPUT_LEN;
    if (rgba_need_bytes(output_width, output_height, &out_need) != 0) return JNI_ERR_OUTPUT_LEN;
    if (!java_array_covers(env, input, in_need)) return JNI_ERR_INPUT_LEN;
    if (!java_array_covers(env, output, out_need)) return JNI_ERR_OUTPUT_LEN;

    if (g_session_in_w != (unsigned)input_width || g_session_in_h != (unsigned)input_height) {
        LOGE("process input %dx%d != session %ux%u",
             input_width, input_height, g_session_in_w, g_session_in_h);
        return JNI_ERR_SESSION_INPUT;
    }
    if (g_session_out_w != (unsigned)output_width || g_session_out_h != (unsigned)output_height) {
        LOGE("process output %dx%d != session %ux%u",
             output_width, output_height, g_session_out_w, g_session_out_h);
        return JNI_ERR_SESSION_OUTPUT;
    }

    jbyte *in_bytes = env->GetByteArrayElements(input, nullptr);
    jbyte *out_bytes = env->GetByteArrayElements(output, nullptr);
    if (!in_bytes || !out_bytes) {
        if (in_bytes) env->ReleaseByteArrayElements(input, in_bytes, JNI_ABORT);
        if (out_bytes) env->ReleaseByteArrayElements(output, out_bytes, JNI_ABORT);
        return JNI_ERR_NULL_ARGS;
    }

    if (!ensure_gles_io_textures(input_width, input_height, output_width, output_height)) {
        env->ReleaseByteArrayElements(input, in_bytes, JNI_ABORT);
        env->ReleaseByteArrayElements(output, out_bytes, JNI_ABORT);
        return JNI_ERR_EGL;
    }

    gles_upload_rgba(g_input_tex, input_width, input_height, (const uint8_t *)in_bytes);

    mc_nscaler_input_frame_t in_frame;
    mc_nscaler_output_frame_t out_frame;
    memset(&in_frame, 0, sizeof(in_frame));
    memset(&out_frame, 0, sizeof(out_frame));
    in_frame.handle.gl_texture = g_input_tex;
    in_frame.format = (uint32_t)GL_RGBA8;
    in_frame.target = (uint32_t)GL_TEXTURE_2D;
    in_frame.mip_count = 1;
    in_frame.width = (unsigned)input_width;
    in_frame.height = (unsigned)input_height;
    out_frame.handle.gl_texture = g_output_tex;
    out_frame.format = (uint32_t)GL_RGBA8;
    out_frame.target = (uint32_t)GL_TEXTURE_2D;
    out_frame.mip_count = 1;
    out_frame.width = (unsigned)output_width;
    out_frame.height = (unsigned)output_height;

    int ret = mc_nscaler_enable(&g_sr_handle, &in_frame, &out_frame);
    if (ret != 0) {
        LOGE("mc_nscaler_enable failed ret=%d", ret);
        env->ReleaseByteArrayElements(input, in_bytes, JNI_ABORT);
        env->ReleaseByteArrayElements(output, out_bytes, JNI_ABORT);
        return ret;
    }
    if (!g_logged_first_process) {
        g_logged_first_process = true;
        LOGI("mc_nscaler_enable first ok version=%s ret=0 in=%dx%d out=%dx%d scale=%.3f mode=%d",
             mc_nscaler_version(), input_width, input_height, output_width, output_height,
             g_scale, (int)g_mode);
    }
    glFinish();

    uint8_t *readback = gles_readback_rgba(g_output_tex, output_width, output_height);
    if (!readback) {
        ret = JNI_ERR_READBACK;
    } else {
        memcpy(out_bytes, readback, out_need);
        free(readback);
    }

    env->ReleaseByteArrayElements(input, in_bytes, JNI_ABORT);
    env->ReleaseByteArrayElements(output, out_bytes, ret == 0 ? 0 : JNI_ABORT);
    return ret;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_superresolution_natives_SuperResolutionLib_uninitSuperResolution(
        JNIEnv *,
        jclass) {
    release_session();
}
