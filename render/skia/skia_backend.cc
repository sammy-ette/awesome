/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Skia/Vulkan renderer for AwesomeWM.
 *
 * This backend presents XCB windows directly through Vulkan swapchains wrapped
 * by Skia Ganesh surfaces. Its C ABI keeps Skia's C++ implementation details
 * out of Awesome's C core and Lua binding.
 */

#include "render/skia/skia_backend.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkColorType.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrBackendSemaphore.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkBackendSemaphore.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "include/gpu/ganesh/vk/GrVkTypes.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/vk/VulkanMutableTextureState.h"

/* Skia does not currently expose a public factory for its configured Vulkan
 * allocator. For this prototype we deliberately bind to the private factory.
 * Pinning the Skia revision is therefore required until Awesome provides its
 * own implementation of skgpu::VulkanMemoryAllocator.
 */
#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

void set_error(char *buffer, size_t size, const char *message)
{
    if (!buffer || size == 0)
        return;

    std::snprintf(buffer, size, "%s", message ? message : "unknown error");
}

const char *vk_result_name(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "unrecognised VkResult";
    }
}

void set_vk_error(char *buffer, size_t size, const char *operation, VkResult result)
{
    if (!buffer || size == 0)
        return;

    std::snprintf(buffer, size, "%s failed: %s (%d)", operation,
                  vk_result_name(result), static_cast<int>(result));
}

bool contains_extension(const std::vector<VkExtensionProperties>& extensions,
                        const char *name)
{
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    return false;
}

VkCompositeAlphaFlagBitsKHR choose_composite_alpha(VkCompositeAlphaFlagsKHR supported)
{
    /* Premultiplied alpha is the representation used by Skia. Some X11 WSI
     * implementations expose only INHERIT or OPAQUE, so retain a
     * conservative fallback order for the probe.
     */
    constexpr VkCompositeAlphaFlagBitsKHR preference[] = {
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };

    for (VkCompositeAlphaFlagBitsKHR candidate : preference)
        if (supported & candidate)
            return candidate;

    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

SkColor color_from_rgba(uint32_t rgba)
{
    return SkColorSetARGB(rgba & 0xff,
                          (rgba >> 24) & 0xff,
                          (rgba >> 16) & 0xff,
                          (rgba >> 8) & 0xff);
}

} // namespace

struct awesome_skia_renderer;

struct awesome_skia_frame
{
    awesome_skia_renderer *renderer = nullptr;
    SkCanvas *canvas = nullptr;
    SkSurface *surface = nullptr;
    uint32_t image_index = 0;
    size_t sync_index = 0;
};

struct frame_sync_t
{
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence recycle_fence = VK_NULL_HANDLE;
};

/* Vulkan and Ganesh contexts are process-wide GPU resources, not per-window
 * resources.  Every drawin gets its own XCB surface/swapchain below, while
 * this object owns the one device and Skia context shared by all of them.
 * Creating a device for each wibar, popup and wallpaper serializes driver
 * initialization and makes a normal Awesome configuration appear to have only
 * its first window.
 */
struct shared_gpu_t
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    VkPhysicalDeviceFeatures enabled_features = {};
    skgpu::VulkanExtensions skia_extensions;
    sk_sp<GrDirectContext> skia_context;

    ~shared_gpu_t()
    {
        if (device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(device);
        skia_context.reset();
        if (device != VK_NULL_HANDLE)
            vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE)
            vkDestroyInstance(instance, nullptr);
    }
};

std::weak_ptr<shared_gpu_t> global_shared_gpu;

struct awesome_skia_renderer
{
    xcb_connection_t *connection = nullptr;
    xcb_window_t window = XCB_NONE;
    uint32_t requested_width = 0;
    uint32_t requested_height = 0;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    VkSurfaceKHR xcb_surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    VkImageUsageFlags image_usage = 0;
    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    VkPhysicalDeviceFeatures enabled_features = {};
    skgpu::VulkanExtensions skia_extensions;
    sk_sp<GrDirectContext> skia_context;
    std::shared_ptr<shared_gpu_t> gpu;
    std::vector<VkImage> images;
    std::vector<sk_sp<SkSurface>> skia_surfaces;
    std::vector<frame_sync_t> frame_sync;
    size_t next_sync = 0;
    awesome_skia_frame *active_frame = nullptr;

    void destroy_frame_sync()
    {
        for (const frame_sync_t& sync : frame_sync)
        {
            if (sync.image_available != VK_NULL_HANDLE)
                vkDestroySemaphore(device, sync.image_available, nullptr);
            if (sync.render_finished != VK_NULL_HANDLE)
                vkDestroySemaphore(device, sync.render_finished, nullptr);
            if (sync.recycle_fence != VK_NULL_HANDLE)
                vkDestroyFence(device, sync.recycle_fence, nullptr);
        }
        frame_sync.clear();
        next_sync = 0;
    }

    bool create_frame_sync(char *error, size_t error_size)
    {
        const size_t count = std::min<size_t>(2, images.size());
        if (count == 0)
        {
            set_error(error, error_size, "Cannot create frame sync without swapchain images");
            return false;
        }

        VkSemaphoreCreateInfo semaphore_info = {};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        frame_sync.resize(count);
        for (frame_sync_t& sync : frame_sync)
        {
            VkResult result = vkCreateSemaphore(device, &semaphore_info, nullptr,
                                                &sync.image_available);
            if (result == VK_SUCCESS)
                result = vkCreateSemaphore(device, &semaphore_info, nullptr,
                                           &sync.render_finished);
            if (result == VK_SUCCESS)
                result = vkCreateFence(device, &fence_info, nullptr,
                                       &sync.recycle_fence);
            if (result != VK_SUCCESS)
            {
                set_vk_error(error, error_size, "creating Vulkan frame synchronization", result);
                destroy_frame_sync();
                return false;
            }
        }
        return true;
    }

    void adopt_shared_gpu(const std::shared_ptr<shared_gpu_t>& shared)
    {
        gpu = shared;
        instance = gpu->instance;
        physical_device = gpu->physical_device;
        device = gpu->device;
        queue = gpu->queue;
        queue_family = gpu->queue_family;
        skia_context = gpu->skia_context;
    }

    bool create_instance(char *error, size_t error_size)
    {
        const char *instance_extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_XCB_SURFACE_EXTENSION_NAME,
        };

        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "awesome-skia-probe";
        app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.pEngineName = "AwesomeWM";
        app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount =
            static_cast<uint32_t>(std::size(instance_extensions));
        create_info.ppEnabledExtensionNames = instance_extensions;

        VkResult result = vkCreateInstance(&create_info, nullptr, &gpu->instance);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkCreateInstance", result);
            return false;
        }

        instance = gpu->instance;
        return true;
    }

    bool create_xcb_surface(char *error, size_t error_size)
    {
        VkXcbSurfaceCreateInfoKHR surface_info = {};
        surface_info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        surface_info.connection = connection;
        surface_info.window = window;

        VkResult result = vkCreateXcbSurfaceKHR(instance, &surface_info, nullptr, &xcb_surface);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkCreateXcbSurfaceKHR", result);
            return false;
        }

        return true;
    }

    bool choose_device(char *error, size_t error_size)
    {
        uint32_t physical_device_count = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance,
                                                      &physical_device_count,
                                                      nullptr);
        if (result != VK_SUCCESS || physical_device_count == 0)
        {
            if (result == VK_SUCCESS)
                set_error(error, error_size, "No Vulkan physical device found");
            else
                set_vk_error(error, error_size, "vkEnumeratePhysicalDevices", result);
            return false;
        }

        std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
        result = vkEnumeratePhysicalDevices(instance, &physical_device_count,
                                             physical_devices.data());
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkEnumeratePhysicalDevices", result);
            return false;
        }

        for (VkPhysicalDevice candidate : physical_devices)
        {
            VkPhysicalDeviceProperties properties = {};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_1)
                continue;

            uint32_t extension_count = 0;
            result = vkEnumerateDeviceExtensionProperties(candidate, nullptr,
                                                          &extension_count, nullptr);
            if (result != VK_SUCCESS)
                continue;

            std::vector<VkExtensionProperties> extensions(extension_count);
            result = vkEnumerateDeviceExtensionProperties(candidate, nullptr,
                                                          &extension_count,
                                                          extensions.data());
            if (result != VK_SUCCESS ||
                !contains_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                continue;

            uint32_t queue_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count,
                                                      queues.data());

            for (uint32_t index = 0; index < queue_count; ++index)
            {
                VkBool32 present_supported = VK_FALSE;
                result = vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index,
                                                              xcb_surface,
                                                              &present_supported);
                if (result != VK_SUCCESS)
                    continue;

                if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    present_supported == VK_TRUE)
                {
                    physical_device = candidate;
                    queue_family = index;
                    break;
                }
            }

            if (physical_device != VK_NULL_HANDLE)
                break;
        }

        if (physical_device == VK_NULL_HANDLE)
        {
            set_error(error, error_size,
                      "No Vulkan device has a queue supporting graphics and XCB presentation");
            return false;
        }

        return true;
    }

    bool validate_shared_present_support(char *error, size_t error_size)
    {
        VkBool32 present_supported = VK_FALSE;
        VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(
            physical_device, queue_family, xcb_surface, &present_supported);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkGetPhysicalDeviceSurfaceSupportKHR", result);
            return false;
        }
        if (present_supported != VK_TRUE)
        {
            set_error(error, error_size,
                      "The shared Vulkan queue cannot present to this XCB window");
            return false;
        }
        return true;
    }

    bool create_device_and_skia(char *error, size_t error_size)
    {
        constexpr float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        const char *device_extensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };

        /* Start with no optional VkPhysicalDeviceFeatures enabled. This is the
         * least surprising contract for Skia and keeps the backend portable. */
        gpu->enabled_features = {};

        VkDeviceCreateInfo device_info = {};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount =
            static_cast<uint32_t>(std::size(device_extensions));
        device_info.ppEnabledExtensionNames = device_extensions;
        device_info.pEnabledFeatures = &gpu->enabled_features;

        VkResult result = vkCreateDevice(physical_device, &device_info, nullptr, &gpu->device);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkCreateDevice", result);
            return false;
        }

        device = gpu->device;
        vkGetDeviceQueue(device, queue_family, 0, &gpu->queue);
        queue = gpu->queue;

        skgpu::VulkanBackendContext backend;
        backend.fInstance = instance;
        backend.fPhysicalDevice = physical_device;
        backend.fDevice = device;
        backend.fQueue = queue;
        backend.fGraphicsQueueIndex = queue_family;
        backend.fMaxAPIVersion = VK_API_VERSION_1_1;
        backend.fDeviceFeatures = &gpu->enabled_features;
        backend.fGetProc = [](const char *name, VkInstance vk_instance,
                              VkDevice vk_device) -> PFN_vkVoidFunction {
            if (vk_device != VK_NULL_HANDLE)
                return vkGetDeviceProcAddr(vk_device, name);
            return vkGetInstanceProcAddr(vk_instance, name);
        };

        const char *instance_extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_XCB_SURFACE_EXTENSION_NAME,
        };
        const char *device_extensions_for_skia[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        gpu->skia_extensions.init(backend.fGetProc, instance, physical_device,
                                  static_cast<uint32_t>(std::size(instance_extensions)),
                                  instance_extensions,
                                  static_cast<uint32_t>(std::size(device_extensions_for_skia)),
                                  device_extensions_for_skia);
        backend.fVkExtensions = &gpu->skia_extensions;
        backend.fMemoryAllocator = skgpu::VulkanMemoryAllocators::Make(
            backend, skgpu::ThreadSafe::kNo);
        if (!backend.fMemoryAllocator)
        {
            set_error(error, error_size,
                      "Skia failed to create its Vulkan memory allocator");
            return false;
        }

        gpu->skia_context = GrDirectContexts::MakeVulkan(backend);
        if (!gpu->skia_context)
        {
            set_error(error, error_size,
                      "GrDirectContexts::MakeVulkan returned null");
            return false;
        }

        skia_context = gpu->skia_context;
        gpu->physical_device = physical_device;
        gpu->queue_family = queue_family;
        return true;
    }

    bool choose_surface_format(VkSurfaceFormatKHR *chosen,
                               SkColorType *color_type,
                               char *error,
                               size_t error_size)
    {
        uint32_t count = 0;
        VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physical_device, xcb_surface, &count, nullptr);
        if (result != VK_SUCCESS || count == 0)
        {
            if (result == VK_SUCCESS)
                set_error(error, error_size, "The XCB Vulkan surface has no formats");
            else
                set_vk_error(error, error_size,
                             "vkGetPhysicalDeviceSurfaceFormatsKHR", result);
            return false;
        }

        std::vector<VkSurfaceFormatKHR> formats(count);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physical_device, xcb_surface, &count, formats.data());
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size,
                         "vkGetPhysicalDeviceSurfaceFormatsKHR", result);
            return false;
        }

        if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        {
            chosen->format = VK_FORMAT_B8G8R8A8_UNORM;
            chosen->colorSpace = formats[0].colorSpace;
            *color_type = kBGRA_8888_SkColorType;
            return true;
        }

        constexpr VkFormat preference[] = {
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R8G8B8A8_SRGB,
        };

        for (VkFormat wanted : preference)
        {
            for (const auto& available : formats)
            {
                if (available.format != wanted)
                    continue;

                *chosen = available;
                *color_type = (wanted == VK_FORMAT_B8G8R8A8_UNORM ||
                               wanted == VK_FORMAT_B8G8R8A8_SRGB)
                                  ? kBGRA_8888_SkColorType
                                  : kRGBA_8888_SkColorType;
                return true;
            }
        }

        set_error(error, error_size,
                  "No BGRA8 or RGBA8 Vulkan surface format is available");
        return false;
    }

    bool create_swapchain(uint32_t width, uint32_t height,
                          char *error, size_t error_size)
    {
        if (width == 0 || height == 0)
        {
            set_error(error, error_size, "Cannot create a zero-sized swapchain");
            return false;
        }

        VkSurfaceCapabilitiesKHR capabilities = {};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physical_device, xcb_surface, &capabilities);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size,
                         "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
            return false;
        }

        VkSurfaceFormatKHR surface_format = {};
        SkColorType color_type = kUnknown_SkColorType;
        if (!choose_surface_format(&surface_format, &color_type, error, error_size))
            return false;

        VkExtent2D new_extent = capabilities.currentExtent;
        if (new_extent.width == UINT32_MAX)
        {
            new_extent.width = std::clamp(width,
                                         capabilities.minImageExtent.width,
                                         capabilities.maxImageExtent.width);
            new_extent.height = std::clamp(height,
                                          capabilities.minImageExtent.height,
                                          capabilities.maxImageExtent.height);
        }

        uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0)
            image_count = std::min(image_count, capabilities.maxImageCount);

        /* Ganesh requires transfer source and destination usage on wrapped
         * Vulkan render targets. This lets it resolve/copy internally when a
         * draw needs it, even though the probe itself only paints directly.
         */
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((capabilities.supportedUsageFlags & usage) != usage)
        {
            set_error(error, error_size,
                      "The Vulkan surface lacks the transfer usage required by Skia Ganesh");
            return false;
        }

        VkSwapchainCreateInfoKHR create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = xcb_surface;
        create_info.minImageCount = image_count;
        create_info.imageFormat = surface_format.format;
        create_info.imageColorSpace = surface_format.colorSpace;
        create_info.imageExtent = new_extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage = usage;
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.preTransform = capabilities.currentTransform;
        create_info.compositeAlpha =
            choose_composite_alpha(capabilities.supportedCompositeAlpha);
        create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        create_info.clipped = VK_TRUE;
        create_info.oldSwapchain = swapchain;

        VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
        result = vkCreateSwapchainKHR(device, &create_info, nullptr, &new_swapchain);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkCreateSwapchainKHR", result);
            return false;
        }

        /* A swapchain owns its synchronisation objects. Recreate only between
         * frames, then wait once before releasing the old resources. Normal
         * rendering never waits for the GPU on the CPU. */
        if (active_frame)
        {
            set_error(error, error_size, "Cannot resize while a Skia frame is active");
            vkDestroySwapchainKHR(device, new_swapchain, nullptr);
            return false;
        }
        vkDeviceWaitIdle(device);
        destroy_frame_sync();
        skia_surfaces.clear();
        images.clear();
        if (swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device, swapchain, nullptr);

        swapchain = new_swapchain;
        swapchain_format = surface_format.format;
        extent = new_extent;
        image_usage = usage;
        composite_alpha = create_info.compositeAlpha;
        requested_width = width;
        requested_height = height;

        uint32_t swapchain_image_count = 0;
        result = vkGetSwapchainImagesKHR(device, swapchain,
                                         &swapchain_image_count, nullptr);
        if (result != VK_SUCCESS || swapchain_image_count == 0)
        {
            if (result == VK_SUCCESS)
                set_error(error, error_size, "The Vulkan swapchain has no images");
            else
                set_vk_error(error, error_size, "vkGetSwapchainImagesKHR", result);
            return false;
        }

        images.resize(swapchain_image_count);
        result = vkGetSwapchainImagesKHR(device, swapchain,
                                         &swapchain_image_count, images.data());
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkGetSwapchainImagesKHR", result);
            return false;
        }

        skia_surfaces.reserve(images.size());
        sk_sp<SkColorSpace> color_space = SkColorSpace::MakeSRGB();
        for (VkImage image : images)
        {
            GrVkImageInfo image_info;
            image_info.fImage = image;
            /* A swapchain image is acquired from the presentation engine.
             * Its valid incoming layout is PRESENT_SRC, not UNDEFINED.  The
             * latter tells Ganesh that it may discard the image without the
             * transition required by X11 WSI, which results in black frames.
             * The mutable presentation state supplied at flush below keeps
             * this contract true for every subsequent acquire as well.
             */
            image_info.fImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            image_info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.fFormat = swapchain_format;
            image_info.fImageUsageFlags = image_usage;
            image_info.fSampleCount = 1;
            image_info.fLevelCount = 1;
            image_info.fCurrentQueueFamily = queue_family;
            image_info.fProtected = skgpu::Protected::kNo;
            image_info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;

            GrBackendRenderTarget target = GrBackendRenderTargets::MakeVk(
                static_cast<int>(extent.width),
                static_cast<int>(extent.height),
                image_info);
            sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(
                skia_context.get(), target, kTopLeft_GrSurfaceOrigin,
                color_type, color_space, nullptr);
            if (!surface)
            {
                set_error(error, error_size,
                          "Skia failed to wrap a Vulkan swapchain image");
                skia_surfaces.clear();
                return false;
            }
            skia_surfaces.push_back(std::move(surface));
        }

        return create_frame_sync(error, error_size);
    }

    awesome_skia_frame *begin_frame(char *error, size_t error_size)
    {
        if (!skia_context || swapchain == VK_NULL_HANDLE || skia_surfaces.empty())
        {
            set_error(error, error_size, "The Skia renderer is not initialized");
            return nullptr;
        }
        if (active_frame)
        {
            set_error(error, error_size, "A Skia frame is already active");
            return nullptr;
        }
        if (frame_sync.empty())
        {
            set_error(error, error_size, "The Skia renderer has no frame synchronization");
            return nullptr;
        }

        const size_t sync_index = next_sync;
        frame_sync_t& sync = frame_sync[sync_index];
        VkResult result = vkWaitForFences(device, 1, &sync.recycle_fence,
                                          VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkWaitForFences", result);
            return nullptr;
        }

        uint32_t image_index = 0;
        result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                       sync.image_available, VK_NULL_HANDLE,
                                       &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            if (!create_swapchain(requested_width, requested_height,
                                  error, error_size))
                return nullptr;
            return begin_frame(error, error_size);
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            set_vk_error(error, error_size, "vkAcquireNextImageKHR", result);
            return nullptr;
        }

        if (image_index >= skia_surfaces.size())
        {
            set_error(error, error_size, "Vulkan returned an invalid swapchain image index");
            return nullptr;
        }

        GrBackendSemaphore acquire_semaphore =
            GrBackendSemaphores::MakeVk(sync.image_available);
        if (!skia_context->wait(1, &acquire_semaphore, false))
        {
            set_error(error, error_size, "Skia could not wait for the acquired swapchain image");
            return nullptr;
        }

        std::unique_ptr<awesome_skia_frame> frame(new (std::nothrow) awesome_skia_frame());
        if (!frame)
        {
            set_error(error, error_size, "Out of memory creating a Skia frame");
            return nullptr;
        }
        frame->renderer = this;
        frame->canvas = skia_surfaces[image_index]->getCanvas();
        frame->surface = skia_surfaces[image_index].get();
        frame->image_index = image_index;
        frame->sync_index = sync_index;
        active_frame = frame.get();
        return frame.release();
    }

    bool end_frame(awesome_skia_frame *frame, char *error, size_t error_size)
    {
        std::unique_ptr<awesome_skia_frame> owned_frame(frame);
        if (!frame || frame != active_frame || frame->renderer != this)
        {
            set_error(error, error_size, "The Skia frame does not belong to this renderer");
            return false;
        }
        active_frame = nullptr;
        frame_sync_t& sync = frame_sync[frame->sync_index];

        GrBackendSemaphore render_semaphore =
            GrBackendSemaphores::MakeVk(sync.render_finished);
        GrFlushInfo flush_info = {};
        flush_info.fNumSemaphores = 1;
        flush_info.fSignalSemaphores = &render_semaphore;
        skgpu::MutableTextureState present_state =
            skgpu::MutableTextureStates::MakeVulkan(
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, queue_family);
        if (skia_context->flush(frame->surface, flush_info, &present_state) !=
            GrSemaphoresSubmitted::kYes)
        {
            set_error(error, error_size, "Skia failed to submit the render-complete semaphore");
            return false;
        }
        skia_context->submit(GrSyncCpu::kNo);

        VkPresentInfoKHR present_info = {};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &sync.render_finished;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &frame->image_index;

        VkResult result = vkQueuePresentKHR(queue, &present_info);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            return create_swapchain(requested_width, requested_height,
                                    error, error_size);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkQueuePresentKHR", result);
            return false;
        }

        /* Queue an empty operation after present. Its fence lets this frame's
         * binary semaphores be reused without a CPU wait in the steady-state
         * rendering path. */
        result = vkResetFences(device, 1, &sync.recycle_fence);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkResetFences", result);
            return false;
        }
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        result = vkQueueSubmit(queue, 1, &submit_info, sync.recycle_fence);
        if (result != VK_SUCCESS)
        {
            set_vk_error(error, error_size, "vkQueueSubmit", result);
            return false;
        }
        next_sync = (frame->sync_index + 1) % frame_sync.size();
        return true;
    }

    bool draw(float phase, char *error, size_t error_size)
    {
        awesome_skia_frame *frame = begin_frame(error, error_size);
        if (!frame)
            return false;

        SkCanvas *canvas = frame->canvas;
        const float width = static_cast<float>(extent.width);
        const float height = static_cast<float>(extent.height);
        const float angle = phase * 6.28318530718f;

        canvas->clear(SkColorSetARGB(220, 17, 20, 28));

        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(SkColorSetARGB(255, 88, 166, 255));
        canvas->drawRoundRect(
            SkRect::MakeXYWH(width * 0.08f, height * 0.12f,
                             width * 0.84f, height * 0.76f),
            28.0f, 28.0f, paint);

        paint.setColor(SkColorSetARGB(230, 255, 255, 255));
        canvas->drawCircle(width * (0.5f + 0.28f * std::cos(angle)),
                           height * (0.5f + 0.28f * std::sin(angle)),
                           std::max(12.0f, std::min(width, height) * 0.06f),
                           paint);

        SkPathBuilder path_builder;
        path_builder.moveTo(width * 0.24f, height * 0.65f);
        path_builder.cubicTo(width * 0.36f, height * 0.20f,
                             width * 0.64f, height * 0.90f,
                             width * 0.78f, height * 0.35f);
        SkPath path = path_builder.detach();
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(8.0f);
        paint.setStrokeCap(SkPaint::kRound_Cap);
        paint.setColor(SkColorSetARGB(255, 255, 190, 77));
        canvas->drawPath(path, paint);

        return end_frame(frame, error, error_size);
    }

    void destroy()
    {
        if (gpu && device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(device);

        if (active_frame)
        {
            delete active_frame;
            active_frame = nullptr;
        }
        destroy_frame_sync();

        skia_surfaces.clear();
        images.clear();
        skia_context.reset();

        if (swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;

        if (xcb_surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(instance, xcb_surface, nullptr);
        xcb_surface = VK_NULL_HANDLE;

        /* gpu owns the device, Skia context and instance. Releasing the last
         * renderer tears them down after every swapchain has gone away. */
        gpu.reset();
        device = VK_NULL_HANDLE;
        instance = VK_NULL_HANDLE;
    }
};

extern "C" awesome_skia_renderer_t *awesome_skia_renderer_create(
    xcb_connection_t *connection,
    xcb_window_t window,
    uint32_t width,
    uint32_t height,
    char *error,
    size_t error_size)
{
    if (!connection || window == XCB_NONE)
    {
        set_error(error, error_size, "An XCB connection and window are required");
        return nullptr;
    }

    std::unique_ptr<awesome_skia_renderer> renderer(
        new (std::nothrow) awesome_skia_renderer());
    if (!renderer)
    {
        set_error(error, error_size, "Out of memory creating the renderer");
        return nullptr;
    }

    renderer->connection = connection;
    renderer->window = window;
    renderer->requested_width = width;
    renderer->requested_height = height;

    if (std::shared_ptr<shared_gpu_t> shared = global_shared_gpu.lock())
    {
        renderer->adopt_shared_gpu(shared);
        if (!renderer->create_xcb_surface(error, error_size) ||
            !renderer->validate_shared_present_support(error, error_size) ||
            !renderer->create_swapchain(width, height, error, error_size))
        {
            renderer->destroy();
            return nullptr;
        }
        return renderer.release();
    }

    renderer->gpu = std::make_shared<shared_gpu_t>();
    if (!renderer->create_instance(error, error_size) ||
        !renderer->create_xcb_surface(error, error_size) ||
        !renderer->choose_device(error, error_size) ||
        !renderer->create_device_and_skia(error, error_size) ||
        !renderer->create_swapchain(width, height, error, error_size))
    {
        renderer->destroy();
        return nullptr;
    }

    global_shared_gpu = renderer->gpu;
    return renderer.release();
}

extern "C" void awesome_skia_renderer_destroy(awesome_skia_renderer_t *renderer)
{
    if (!renderer)
        return;
    renderer->destroy();
    delete renderer;
}

extern "C" bool awesome_skia_renderer_resize(
    awesome_skia_renderer_t *renderer,
    uint32_t width,
    uint32_t height,
    char *error,
    size_t error_size)
{
    if (!renderer)
    {
        set_error(error, error_size, "Renderer is null");
        return false;
    }
    return renderer->create_swapchain(width, height, error, error_size);
}

extern "C" awesome_skia_frame_t *awesome_skia_renderer_begin_frame(
    awesome_skia_renderer_t *renderer,
    char *error,
    size_t error_size)
{
    if (!renderer)
    {
        set_error(error, error_size, "Renderer is null");
        return nullptr;
    }
    return renderer->begin_frame(error, error_size);
}

extern "C" bool awesome_skia_renderer_end_frame(
    awesome_skia_frame_t *frame,
    char *error,
    size_t error_size)
{
    if (!frame || !frame->renderer)
    {
        set_error(error, error_size, "Frame is null");
        delete frame;
        return false;
    }
    return frame->renderer->end_frame(frame, error, error_size);
}

extern "C" void awesome_skia_frame_clear(awesome_skia_frame_t *frame, uint32_t rgba)
{
    if (frame && frame->canvas)
        frame->canvas->clear(color_from_rgba(rgba));
}

extern "C" void awesome_skia_frame_save(awesome_skia_frame_t *frame)
{
    if (frame && frame->canvas)
        frame->canvas->save();
}

extern "C" void awesome_skia_frame_restore(awesome_skia_frame_t *frame)
{
    if (frame && frame->canvas)
        frame->canvas->restore();
}

extern "C" void awesome_skia_frame_translate(awesome_skia_frame_t *frame,
                                               float x, float y)
{
    if (frame && frame->canvas)
        frame->canvas->translate(x, y);
}

extern "C" void awesome_skia_frame_scale(awesome_skia_frame_t *frame,
                                           float x, float y)
{
    if (frame && frame->canvas)
        frame->canvas->scale(x, y);
}

extern "C" void awesome_skia_frame_clip_rect(awesome_skia_frame_t *frame,
                                               float x, float y,
                                               float width, float height)
{
    if (frame && frame->canvas)
        frame->canvas->clipRect(SkRect::MakeXYWH(x, y, width, height));
}

extern "C" void awesome_skia_frame_draw_rect(awesome_skia_frame_t *frame,
                                               float x, float y,
                                               float width, float height,
                                               uint32_t rgba)
{
    if (!frame || !frame->canvas)
        return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color_from_rgba(rgba));
    frame->canvas->drawRect(SkRect::MakeXYWH(x, y, width, height), paint);
}

extern "C" void awesome_skia_frame_draw_round_rect(awesome_skia_frame_t *frame,
                                                     float x, float y,
                                                     float width, float height,
                                                     float radius_x, float radius_y,
                                                     uint32_t rgba)
{
    if (!frame || !frame->canvas)
        return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color_from_rgba(rgba));
    frame->canvas->drawRoundRect(SkRect::MakeXYWH(x, y, width, height),
                                 radius_x, radius_y, paint);
}

extern "C" void awesome_skia_frame_draw_circle(awesome_skia_frame_t *frame,
                                                 float x, float y, float radius,
                                                 uint32_t rgba)
{
    if (!frame || !frame->canvas)
        return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color_from_rgba(rgba));
    frame->canvas->drawCircle(x, y, radius, paint);
}

SkCanvas *awesome_skia_frame_canvas(awesome_skia_frame_t *frame)
{
    return frame ? frame->canvas : nullptr;
}

extern "C" bool awesome_skia_renderer_draw_demo(
    awesome_skia_renderer_t *renderer,
    float phase,
    char *error,
    size_t error_size)
{
    if (!renderer)
    {
        set_error(error, error_size, "Renderer is null");
        return false;
    }
    return renderer->draw(phase, error, error_size);
}
