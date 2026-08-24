// Minimal headless libmpv render-API client.
//
// Creates a surfaceless EGL/GL context, drives mpv_render_context_render() into
// an FBO, and reads the result back. Used to check that a render backend
// actually produces pixels -- "it compiles" says nothing about a renderer.
//
// usage: rendertest <api-type> <file> [frames]
//   api-type: "opengl" (legacy vo_gpu renderer) or "opengl-next" (libplacebo)

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#define W 640
#define H 360

static void *get_proc(void *ctx, const char *name)
{
    (void) ctx;
    return (void *) eglGetProcAddress(name);
}

static void die(const char *msg)
{
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        die("usage: rendertest <api-type> <file> [frames]");
    const char *api  = argv[1];
    const char *file = argv[2];
    int want_frames  = argc > 3 ? atoi(argv[3]) : 10;

    // ---- surfaceless EGL ----
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY)
        die("eglGetDisplay failed");
    if (!eglInitialize(dpy, NULL, NULL))
        die("eglInitialize failed");
    if (!eglBindAPI(EGL_OPENGL_API))
        die("eglBindAPI(OpenGL) failed");

    EGLConfig cfg;
    EGLint n = 0;
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1)
        die("eglChooseConfig failed");

    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE
    };
    EGLContext egl_ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (egl_ctx == EGL_NO_CONTEXT)
        die("eglCreateContext failed");

    static const EGLint pb_attr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (surf == EGL_NO_SURFACE)
        die("eglCreatePbufferSurface failed");
    if (!eglMakeCurrent(dpy, surf, surf, egl_ctx))
        die("eglMakeCurrent failed");

    printf("GL_RENDERER: %s\n", (const char *) glGetString(GL_RENDERER));

    // ---- render target ----
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        die("FBO incomplete");
    // Start from opaque black so "did anything get drawn" is unambiguous.
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // ---- mpv ----
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        die("mpv_create failed");
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "terminal", "yes");
    mpv_set_option_string(mpv, "msg-level", "all=info,vo=v,libmpv=v");
    mpv_set_option_string(mpv, "audio", "no");
    const char *o1 = getenv("TEST_OPT"), *o2 = getenv("TEST_OPT_VAL");
    if (o1 && o2) mpv_set_option_string(mpv, o1, o2);
    const char *hw = getenv("TEST_HWDEC");
    if (hw) mpv_set_option_string(mpv, "hwdec", hw);
    mpv_set_option_string(mpv, "vo", "libmpv");
    if (mpv_initialize(mpv) < 0)
        die("mpv_initialize failed");

    mpv_opengl_init_params gl_init = { .get_proc_address = get_proc };
    int advanced = atoi(getenv("TEST_ADVANCED") ?: "1");
    mpv_render_param cparams[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *) api },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced },
        { 0 }
    };
    mpv_render_context *rctx = NULL;
    int err = mpv_render_context_create(&rctx, mpv, cparams);
    if (err < 0) {
        fprintf(stderr, "FATAL: mpv_render_context_create(%s): %s\n",
                api, mpv_error_string(err));
        return 1;
    }
    printf("render context created with api-type=%s advanced_control=%d\n",
           api, advanced);

    const char *cmd[] = { "loadfile", file, NULL };
    mpv_command(mpv, cmd);

    // ---- render loop ----
    int rendered = 0, nonblack = 0;
    bool eof = false;
    unsigned char *px = malloc(W * H * 4);
    double render_ns_total = 0;
    struct timespec t0, t1, wall0;
    clock_gettime(CLOCK_MONOTONIC, &wall0);

    for (int iter = 0; iter < 2000 && rendered < want_frames && !eof; iter++) {
        while (1) {
            mpv_event *ev = mpv_wait_event(mpv, 0);
            if (ev->event_id == MPV_EVENT_NONE)
                break;
            if (ev->event_id == MPV_EVENT_END_FILE)
                eof = true;
            if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
                mpv_event_log_message *m = ev->data;
                fputs(m->text, stderr);
            }
        }

        uint64_t flags = mpv_render_context_update(rctx);
        if (!(flags & MPV_RENDER_UPDATE_FRAME)) {
            struct timespec ts = { .tv_nsec = 2 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }

        mpv_opengl_fbo target = { .fbo = (int) fbo, .w = W, .h = H,
                                  .internal_format = GL_RGBA8 };
        int flip = 0;
        mpv_render_param rparams[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO, &target },
            { MPV_RENDER_PARAM_FLIP_Y, &flip },
            { 0 }
        };
        clock_gettime(CLOCK_MONOTONIC, &t0);
        err = mpv_render_context_render(rctx, rparams);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        render_ns_total += (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        if (err < 0) {
            fprintf(stderr, "FATAL: render: %s\n", mpv_error_string(err));
            return 1;
        }
        rendered++;

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        long sum = 0;
        int purple = 0;
        for (int i = 0; i < W * H; i++) {
            sum += px[i*4] + px[i*4+1] + px[i*4+2];
            // vo_gpu_next clears to purple (0.5, 0, 1) when a frame fails
            if (px[i*4] > 100 && px[i*4] < 160 && px[i*4+1] < 20 && px[i*4+2] > 240)
                purple++;
        }
        if (sum > 0)
            nonblack++;
        if (rendered == want_frames) {
            printf("frame %d: mean luma %.1f, purple pixels %d/%d\n",
                   rendered, (double) sum / (W * H * 3), purple, W * H);
            if (purple > (W * H) / 2)
                die("target is mostly purple: renderer signalled frame failure");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall_s = (t1.tv_sec - wall0.tv_sec) + (t1.tv_nsec - wall0.tv_nsec) / 1e9;
    printf("TIMING: mean render() %.2f ms, wall %.2f s for %d frames (%.1f fps)\n",
           rendered ? render_ns_total / rendered / 1e6 : 0, wall_s, rendered,
           wall_s > 0 ? rendered / wall_s : 0);
    printf("RESULT: rendered=%d nonblack=%d (wanted %d)\n",
           rendered, nonblack, want_frames);

    mpv_render_context_free(rctx);
    mpv_terminate_destroy(mpv);

    if (rendered == 0)
        die("no frames rendered");
    if (nonblack == 0)
        die("all rendered frames were blank");
    printf("OK: %s produced %d non-blank frames\n", api, nonblack);
    return 0;
}
