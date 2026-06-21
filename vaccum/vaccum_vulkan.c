#include "vaccum_priv.h"
#include <stdbool.h>

void vaccum_vulkan_fini(struct vaccum_screen_private *vaccum_priv)
{
    vkDestroyCommandPool(vaccum_priv->device, vaccum_priv->command_pool, NULL);
    vkDestroyDevice(vaccum_priv->device, NULL);
    vkDestroyInstance(vaccum_priv->instance, NULL);
    free(vaccum_priv->phys_devices);
    free(vaccum_priv->queue_families);
}

Bool vaccum_vulkan_init(struct vaccum_screen_private *vaccum_priv)
{
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "X.org server";
    appInfo.pEngineName = "X.org server";
    appInfo.apiVersion = 0;//apiVersion;

    VkInstanceCreateInfo instanceCreateInfo = {};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pNext = NULL;
    instanceCreateInfo.pApplicationInfo = &appInfo;

    VkResult result = vkCreateInstance(&instanceCreateInfo, NULL, &vaccum_priv->instance);
    if (result != VK_SUCCESS)
        return FALSE;

    uint32_t gpu_count;
    result = vkEnumeratePhysicalDevices(vaccum_priv->instance, &gpu_count, NULL);
    if (gpu_count == 0) {
        LogMessage(X_WARNING,
                   "vaccum: failed to find any vulkan devices\n");
        return FALSE;
    }

    vaccum_priv->phys_devices = malloc(sizeof(VkPhysicalDevice) * gpu_count);
    if (!vaccum_priv->phys_devices)
        return FALSE;

    result = vkEnumeratePhysicalDevices(vaccum_priv->instance, &gpu_count, vaccum_priv->phys_devices);
    if (result != VK_SUCCESS) {
        LogMessage(X_WARNING,
                   "vaccum: failed to find any vulkan devices\n");
        return FALSE;
    }

    vaccum_priv->phys_device = vaccum_priv->phys_devices[0];
    vkGetPhysicalDeviceProperties(vaccum_priv->phys_device, &vaccum_priv->dev_properties);
    vkGetPhysicalDeviceFeatures(vaccum_priv->phys_device, &vaccum_priv->dev_features);
    vkGetPhysicalDeviceMemoryProperties(vaccum_priv->phys_device, &vaccum_priv->dev_mem_properties);

    uint32_t queue_family_count;
    vkGetPhysicalDeviceQueueFamilyProperties(vaccum_priv->phys_device, &queue_family_count, NULL);
    if (queue_family_count == 0)
        return FALSE;
    vaccum_priv->queue_families = malloc(sizeof(VkQueueFamilyProperties) * queue_family_count);
    if (!vaccum_priv->queue_families)
        return FALSE;

    vkGetPhysicalDeviceQueueFamilyProperties(vaccum_priv->phys_device, &queue_family_count, vaccum_priv->queue_families);

    /* for vaccum we just want queue family 0 */
    const float default_queue_priority = 0.0;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &default_queue_priority;

    VkPhysicalDeviceFeatures features = { .robustBufferAccess = true,
                                          .dualSrcBlend = vaccum_priv->dev_features.dualSrcBlend,
                                          .logicOp = vaccum_priv->dev_features.logicOp,
    };

    VkDeviceCreateInfo device_create_info = {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_info;
    device_create_info.pEnabledFeatures = &features;

    result = vkCreateDevice(vaccum_priv->phys_device, &device_create_info, NULL, &vaccum_priv->device);
    if (result != VK_SUCCESS) {
        LogMessage(X_WARNING,
                   "vaccum: failed to create vulkan devices\n");
        return FALSE;
    }

    vkGetDeviceQueue(vaccum_priv->device, 0, 0, &vaccum_priv->queue);

    VkCommandPoolCreateInfo cmd_pool_create_info = {};
    cmd_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    /* queue index */
    result = vkCreateCommandPool(vaccum_priv->device, &cmd_pool_create_info, NULL, &vaccum_priv->command_pool);
    return TRUE;
}

static Bool
vaccum_add_format(struct vaccum_screen_private *vaccum_priv, int depth, CARD32 render_format,
                  VkFormat vk_format)
{
    struct vaccum_format *f = &vaccum_priv->formats[depth];
    VkFormatFeatureFlags req_feats = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;

    vkGetPhysicalDeviceFormatProperties(vaccum_priv->phys_device, vk_format, &f->format_props);

    if ((f->format_props.optimalTilingFeatures & req_feats) != req_feats)
        return FALSE;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkResult result = vkGetPhysicalDeviceImageFormatProperties(vaccum_priv->phys_device, vk_format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, usage, 0, &f->linear_format_props);

    if (result != VK_SUCCESS)
        return FALSE;
    result = vkGetPhysicalDeviceImageFormatProperties(vaccum_priv->phys_device, vk_format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &f->tiled_format_props);
    if (result != VK_SUCCESS)
        return FALSE;

    f->depth = depth;
    f->format = vk_format;
    f->render_format = render_format;
    return TRUE;
}

void vaccum_setup_formats(struct vaccum_screen_private *vaccum_priv)
{
    Bool ret;

    ret = vaccum_add_format(vaccum_priv, 8, PICT_a8, VK_FORMAT_R8_UNORM);
    ret = vaccum_add_format(vaccum_priv, 15, PICT_x1r5g5b5, VK_FORMAT_A1R5G5B5_UNORM_PACK16);

    ret = vaccum_add_format(vaccum_priv, 16, PICT_r5g6b5, VK_FORMAT_R5G6B5_UNORM_PACK16);

    ret = vaccum_add_format(vaccum_priv, 24, PICT_x8b8g8r8, VK_FORMAT_R8G8B8A8_UNORM);
    ret = vaccum_add_format(vaccum_priv, 32, PICT_a8b8g8r8, VK_FORMAT_R8G8B8A8_UNORM);

    ret = vaccum_add_format(vaccum_priv, 30, PICT_x2r10g10b10, VK_FORMAT_A2R10G10B10_UNORM_PACK32);
}

void vaccum_alloc_cmd_buffer(struct vaccum_screen_private *screen_priv)
{

    VkResult result;
    VkCommandBufferAllocateInfo allocate_info = {};

    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = screen_priv->command_pool;
    allocate_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(screen_priv->device, &allocate_info, &screen_priv->current_cmd);

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(screen_priv->current_cmd, &begin_info);
}

void vaccum_flush_cmds(struct vaccum_screen_private *screen_priv)
{
    VkFence fence;

    VkFenceCreateInfo fence_create_info = {};
    fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(screen_priv->device, &fence_create_info, NULL, &fence);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &screen_priv->current_cmd;
    vkQueueSubmit(screen_priv->queue, 1, &submit_info, fence);

    vaccum_alloc_cmd_buffer(screen_priv);
}
