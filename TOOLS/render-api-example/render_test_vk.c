// Headless Vulkan client for the libmpv render API.
//
// Creates its own VkInstance/VkDevice, hands them to mpv via
// MPV_RENDER_API_TYPE_VULKAN, renders into a VkImage it owns, then copies that
// image to a host-visible buffer to check that pixels actually arrived.
//
// usage: vktest <file> [frames]

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include <vulkan/vulkan.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_vk.h>

#define W 640
#define H 360
#define FMT VK_FORMAT_R8G8B8A8_UNORM

static void die(const char *m) { fprintf(stderr, "FATAL: %s\n", m); exit(1); }
#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "FATAL: %s -> %d\n", #x, _r); exit(1); } } while (0)

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkImage image;
static VkDeviceMemory image_mem;
static VkBuffer readback;
static VkDeviceMemory readback_mem;
static VkCommandPool pool;
static VkSemaphore done_sem;
static const VkPhysicalDeviceFeatures2 *vk_features;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    die("no suitable memory type");
    return 0;
}

static void vk_setup(void)
{
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
    VKC(vkCreateInstance(&ici, NULL, &inst));

    uint32_t n = 0;
    VKC(vkEnumeratePhysicalDevices(inst, &n, NULL));
    if (!n) die("no Vulkan devices");
    VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
    VKC(vkEnumeratePhysicalDevices(inst, &n, devs));
    phys = devs[0];
    free(devs);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("VK device: %s\n", props.deviceName);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, NULL);
    VkQueueFamilyProperties *qp = calloc(qn, sizeof(*qp));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qp);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { qfam = i; break; }
    }
    free(qp);
    if (qfam == UINT32_MAX) die("no graphics+compute queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qfam, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    // mpv (via libplacebo) requires these; creating the device without them
    // makes mpv_render_context_create() fail with MPV_ERROR_UNSUPPORTED.
    static VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
        .maintenance4 = VK_TRUE,
    };
    static VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &f13,
        .hostQueryReset = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
        .descriptorIndexing = VK_TRUE,
        .uniformBufferStandardLayout = VK_TRUE,
        .shaderSubgroupExtendedTypes = VK_TRUE,
        .vulkanMemoryModel = VK_TRUE,
        .vulkanMemoryModelDeviceScope = VK_TRUE,
    };
    static VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &f12,
        .samplerYcbcrConversion = VK_TRUE,
        .storageBuffer16BitAccess = VK_TRUE,
    };
    static VkPhysicalDeviceFeatures2 feats = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &f11,
        .features = { .shaderImageGatherExtended = VK_TRUE,
                      .shaderStorageImageReadWithoutFormat = VK_TRUE,
                      .shaderStorageImageWriteWithoutFormat = VK_TRUE },
    };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .pNext = &feats,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VKC(vkCreateDevice(phys, &dci, NULL, &dev));
    vk_features = &feats;
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    // target image
    VkImageCreateInfo img = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = FMT,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VKC(vkCreateImage(dev, &img, NULL, &image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, image, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VKC(vkAllocateMemory(dev, &ai, NULL, &image_mem));
    VKC(vkBindImageMemory(dev, image, image_mem, 0));

    // readback buffer
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize) W * H * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VKC(vkCreateBuffer(dev, &bci, NULL, &readback));
    vkGetBufferMemoryRequirements(dev, readback, &mr);
    VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_mem(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VKC(vkAllocateMemory(dev, &bai, NULL, &readback_mem));
    VKC(vkBindBufferMemory(dev, readback, readback_mem, 0));

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfam,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VKC(vkCreateCommandPool(dev, &pci, NULL, &pool));

    // mpv signals this when it has finished rendering; we must wait on it
    // before touching the image.
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VKC(vkCreateSemaphore(dev, &sci, NULL, &done_sem));
}

// Copy the rendered image into the host-visible buffer. `layout` is whatever
// mpv left the image in.
static void copy_out(VkImageLayout layout, unsigned char *out)
{
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VKC(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKC(vkBeginCommandBuffer(cb, &bi));

    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback, 1, &region);
    VKC(vkEndCommandBuffer(cb));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem,
                        .pWaitDstStageMask = &wait_stage,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    VKC(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cb);

    void *map = NULL;
    VKC(vkMapMemory(dev, readback_mem, 0, (VkDeviceSize) W * H * 4, 0, &map));
    memcpy(out, map, (size_t) W * H * 4);
    vkUnmapMemory(dev, readback_mem);
}

int main(int argc, char **argv)
{
    if (argc < 2) die("usage: vktest <file> [frames]");
    const char *file = argv[1];
    int want = argc > 2 ? atoi(argv[2]) : 10;

    vk_setup();

    mpv_handle *mpv = mpv_create();
    if (!mpv) die("mpv_create");
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "terminal", "yes");
    mpv_set_option_string(mpv, "msg-level", getenv("TEST_MSG") ?: "all=info");
    mpv_set_option_string(mpv, "audio", "no");
    mpv_set_option_string(mpv, "vo", "libmpv");
    const char *hw = getenv("TEST_HWDEC");
    if (hw) mpv_set_option_string(mpv, "hwdec", hw);
    if (mpv_initialize(mpv) < 0) die("mpv_initialize");

    mpv_vulkan_init_params vkp = {
        .instance = inst, .phys_device = phys, .device = dev,
        .get_proc_addr = vkGetInstanceProcAddr,
        .queue_graphics = { qfam, 1 },
        .queue_compute  = { qfam, 1 },
        .queue_transfer = { qfam, 1 },
        .features = vk_features,
    };
    mpv_render_param cp[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *) MPV_RENDER_API_TYPE_VULKAN },
        { MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vkp },
        { 0 }
    };
    mpv_render_context *rctx = NULL;
    int err = mpv_render_context_create(&rctx, mpv, cp);
    if (err < 0) {
        fprintf(stderr, "FATAL: mpv_render_context_create(vulkan): %s\n",
                mpv_error_string(err));
        return 1;
    }
    printf("render context created with api-type=vulkan\n");

    const char *cmd[] = { "loadfile", file, NULL };
    mpv_command(mpv, cmd);

    unsigned char *px = malloc((size_t) W * H * 4);
    int rendered = 0, nonblack = 0;
    bool eof = false;

    for (int i = 0; i < 3000 && rendered < want && !eof; i++) {
        while (1) {
            mpv_event *ev = mpv_wait_event(mpv, 0);
            if (ev->event_id == MPV_EVENT_NONE) break;
            if (ev->event_id == MPV_EVENT_END_FILE) eof = true;
        }
        if (!(mpv_render_context_update(rctx) & MPV_RENDER_UPDATE_FRAME)) {
            nanosleep(&(struct timespec){ .tv_nsec = 2000000 }, NULL);
            continue;
        }

        mpv_vulkan_fbo fbo = {
            .image = image, .format = FMT, .w = W, .h = H,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT,
            .layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .out_layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .signal_semaphore = done_sem,
        };
        mpv_render_param rp[] = {
            { MPV_RENDER_PARAM_VULKAN_FBO, &fbo },
            { 0 }
        };
        err = mpv_render_context_render(rctx, rp);
        if (err < 0) { fprintf(stderr, "FATAL: render: %s\n", mpv_error_string(err)); return 1; }
        rendered++;

        copy_out(fbo.out_layout, px);
        long sum = 0; int purple = 0;
        for (int k = 0; k < W * H; k++) {
            sum += px[k*4] + px[k*4+1] + px[k*4+2];
            if (px[k*4] > 100 && px[k*4] < 160 && px[k*4+1] < 20 && px[k*4+2] > 240)
                purple++;
        }
        if (sum > 0) nonblack++;
        if (rendered == want) {
            printf("frame %d: mean luma %.1f, purple %d/%d, out_layout=%d\n",
                   rendered, (double) sum / (W * H * 3), purple, W * H,
                   (int) fbo.out_layout);
            if (purple > (W * H) / 2) die("mostly purple: renderer signalled failure");
        }
    }

    printf("RESULT: rendered=%d nonblack=%d (wanted %d)\n", rendered, nonblack, want);
    mpv_render_context_free(rctx);
    mpv_terminate_destroy(mpv);
    if (!rendered) die("no frames rendered");
    if (!nonblack) die("all frames blank");
    printf("OK: vulkan produced %d non-blank frames\n", nonblack);
    return 0;
}
