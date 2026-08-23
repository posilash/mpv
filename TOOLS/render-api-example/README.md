# libmpv render API test client

A minimal headless client for `mpv_render_context_render()`. It creates an
EGL/OpenGL context with no window, renders into an FBO, and reads the result
back, so it can check that a render backend actually produces pixels. mpv has no
in-tree render API example, which makes backend changes awkward to verify:
building successfully says nothing about whether a renderer works.

## Build

Against an in-tree build:

    gcc -O1 -g -o render_test render_test.c \
        -I../../include -L../../build -lmpv -lEGL -lGL \
        -Wl,-rpath,$(realpath ../../build)

## Use

    ./render_test <api-type> <file> [frames]

`api-type` is `opengl` for the legacy gl_video renderer, or `opengl-next` for
the libplacebo (vo=gpu-next) renderer.

    ./render_test opengl-next test.mkv 10

It exits non-zero if no frames render, if every frame comes back blank, or if
the target is mostly purple, which is how vo_gpu_next signals a failed frame. On
success it prints the mean luma of the last frame, which is a crude but useful
way to compare two renderers on the same input.

Environment:

- `TEST_HWDEC` sets `--hwdec` (e.g. `auto`, `nvdec`).
- `TEST_OPT` / `TEST_OPT_VAL` set one arbitrary mpv option, which is handy for
  checking that renderer-specific options take effect, e.g.
  `TEST_OPT=tone-mapping TEST_OPT_VAL=reinhard`.

## Vulkan

`render_test_vk.c` is the same test for `MPV_RENDER_API_TYPE_VULKAN`. It creates
its own instance and device, hands them to mpv, renders into a VkImage it owns
and copies that image back to check the result.

    gcc -O1 -g -o render_test_vk render_test_vk.c \
        -I../../include -L../../build -lmpv -lvulkan \
        -Wl,-rpath,$(realpath ../../build)

    ./render_test_vk <file> [frames]

Note what the device needs, because getting it wrong fails at
`mpv_render_context_create()` rather than at render time: the features in
libplacebo's `pl_vulkan_required_features` must be enabled (`hostQueryReset`
among them), and `mpv_vulkan_fbo.signal_semaphore` must be set so the
application can tell when rendering has finished. mpv logs which feature is
missing when the import fails.
