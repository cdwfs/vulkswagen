// Must happen before any vulkan.h include, in order to get the platform-specific extensions included.
#if defined(_MSC_VER)
# define VK_USE_PLATFORM_WIN32_KHR 1
# define PLATFORM_SURFACE_EXTENSION_NAME VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#elif defined(unix) || defined(__unix__) || defined(__unix)
# define VK_USE_PLATFORM_XCB_KHR 1
# define PLATFORM_SURFACE_EXTENSION_NAME VK_KHR_XCB_SURFACE_EXTENSION_NAME
#elif defined(__ANDROID__)
# define VK_USE_PLATFORM_ANDROID_KHR 1
# define PLATFORM_SURFACE_EXTENSION_NAME VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#else
# error Unsupported platform
# define PLATFORM_SURFACE_EXTENSION_NAME "Unsupported platform",
#endif

#include "vk_application.h"
#include "vk_debug.h"
#include "vk_image.h"
#include "vk_utilities.h"
using namespace spokk;

#include "platform.h"

#include <cassert>
#include <cstdio>

#define CDSVK__CLAMP(x, xmin, xmax) ( ((x)<(xmin)) ? (xmin) : ( ((x)>(xmax)) ? (xmax) : (x) ) )

namespace {

template<typename T>
T my_min(T x, T y) {
  return (x < y) ? x : y;
}
template<typename T>
T my_max(T x, T y) {
  return (x > y) ? x : y;
}

void my_glfw_error_callback(int error, const char *description) {
  fprintf( stderr, "GLFW Error %d: %s\n", error, description);
}

VKAPI_ATTR VkBool32 VKAPI_CALL my_debug_report_callback(VkFlags msgFlags,
    VkDebugReportObjectTypeEXT /*objType*/, uint64_t /*srcObject*/, size_t /*location*/, int32_t msgCode,
    const char *pLayerPrefix, const char *pMsg, void * /*pUserData*/) {
  char *message = (char*)malloc(strlen(pMsg)+100);
  assert(message);
  if (msgFlags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
    sprintf(message, "ERROR: [%s] Code %d : %s", pLayerPrefix, msgCode, pMsg);
  } else if (msgFlags & VK_DEBUG_REPORT_WARNING_BIT_EXT) {
    sprintf(message, "WARNING: [%s] Code %d : %s", pLayerPrefix, msgCode, pMsg);
  } else if (msgFlags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) {
    sprintf(message, "INFO: [%s] Code %d : %s", pLayerPrefix, msgCode, pMsg);
  } else if (msgFlags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT) {
    sprintf(message, "PERFORMANCE WARNING: [%s] Code %d : %s", pLayerPrefix, msgCode, pMsg);
  } else {
    return VK_FALSE;
  }
#if 0//_WIN32
  MessageBoxA(NULL, message, "Alert", MB_OK);
#else
  printf("%s\n", message);
  fflush(stdout);
#endif
  free(message);
  if (msgFlags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
    return VK_TRUE; // bail out now if an error occurred
  } else {
    return VK_FALSE; // otherwise, try to soldier on.
  }
}

VkResult find_physical_device(const std::vector<Application::QueueFamilyRequest>& qf_reqs, VkInstance instance,
  VkSurfaceKHR present_surface, VkPhysicalDevice *out_physical_device, std::vector<uint32_t>* out_queue_families) {
  *out_physical_device = VK_NULL_HANDLE;
  uint32_t physical_device_count = 0;
  std::vector<VkPhysicalDevice> all_physical_devices;
  VkResult result = VK_INCOMPLETE;
  do {
    result = vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
    if (result == VK_SUCCESS && physical_device_count > 0) {
      all_physical_devices.resize(physical_device_count);
      result = vkEnumeratePhysicalDevices(instance, &physical_device_count, all_physical_devices.data());
    }
  } while (result == VK_INCOMPLETE);
  out_queue_families->clear();
  out_queue_families->resize(qf_reqs.size(), VK_QUEUE_FAMILY_IGNORED);
  for(auto physical_device : all_physical_devices) {
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> all_queue_family_properties(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, all_queue_family_properties.data());
    bool pd_meets_requirements = true;
    for(uint32_t iReq=0; iReq < qf_reqs.size(); ++iReq) {
      auto &req = qf_reqs[iReq];
      bool found_qf = false;
      // First search for an *exact* match for the requested queue flags, so that users who request e.g. a dedicated
      // transfer queue are more likely to get one.
      for(uint32_t iQF=0; (size_t)iQF < all_queue_family_properties.size(); ++iQF) {
        if (all_queue_family_properties[iQF].queueCount < req.queue_count) {
          continue;  // insufficient queue count
        } else if (all_queue_family_properties[iQF].queueFlags != req.flags) {
          continue;  // family doesn't the exact requested operations
        }
        VkBool32 supports_present = VK_FALSE;
        if (req.flags & VK_QUEUE_GRAPHICS_BIT && present_surface != VK_NULL_HANDLE) {
          result = vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, iQF, present_surface, &supports_present);
          if (result != VK_SUCCESS) {
            return result;
          } else if (!supports_present) {
            continue;  // Queue family can not present to the provided surface
          }
        }
        // This family meets all requirements. Hooray!
        (*out_queue_families)[iReq] = iQF;
        found_qf = true;
        break;
      }
      if (!found_qf) {
        // Search again; this time, accept any queue family that supports the requested flags, even if it supports
        // additional operations.
        for(uint32_t iQF=0; (size_t)iQF < all_queue_family_properties.size(); ++iQF) {
          if (all_queue_family_properties[iQF].queueCount < req.queue_count) {
            continue;  // insufficient queue count
          } else if ((all_queue_family_properties[iQF].queueFlags & req.flags) != req.flags) {
            continue;  // family doesn't support all required operations
          }
          VkBool32 supports_present = VK_FALSE;
          if (req.flags & VK_QUEUE_GRAPHICS_BIT && present_surface != VK_NULL_HANDLE) {
            result = vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, iQF, present_surface, &supports_present);
            if (result != VK_SUCCESS) {
              return result;
            } else if (!supports_present) {
              continue;  // Queue family can not present to the provided surface
            }
          }
          // This family meets all requirements. Hooray!
          (*out_queue_families)[iReq] = iQF;
          found_qf = true;
          break;
        }
      }
      if (!found_qf) {
        pd_meets_requirements = false;
        continue;
      }
    }
    if (pd_meets_requirements) {
      *out_physical_device = physical_device;
      return VK_SUCCESS;
    }
  }
  return VK_ERROR_INITIALIZATION_FAILED;
}

const uint32_t kWindowWidthDefault = 1280;
const uint32_t kWindowHeightDefault = 720;
}  // namespace

//
// InputState
//
void InputState::Update(void) {
  std::shared_ptr<GLFWwindow> w = window_.lock();
  assert(w != nullptr);
  GLFWwindow *pw = w.get();

  prev_ = current_;

  current_.digital[DIGITAL_LPAD_UP] = (GLFW_PRESS == glfwGetKey(pw, GLFW_KEY_W));
  current_.digital[DIGITAL_LPAD_LEFT] = (GLFW_PRESS == glfwGetKey(pw, GLFW_KEY_A));
  current_.digital[DIGITAL_LPAD_RIGHT] = (GLFW_PRESS == glfwGetKey(pw, GLFW_KEY_D));
  current_.digital[DIGITAL_LPAD_DOWN] = (GLFW_PRESS == glfwGetKey(pw, GLFW_KEY_S));

  double mx = 0, my = 0;
  glfwGetCursorPos(pw, &mx, &my);
  current_.analog[ANALOG_MOUSE_X] = (float)mx;
  current_.analog[ANALOG_MOUSE_Y] = (float)my;
}

//
// Application
//
Application::Application(const CreateInfo &ci) {
  if (ci.enable_graphics) {
    // Initialize GLFW
    glfwSetErrorCallback(my_glfw_error_callback);
    if( !glfwInit() ) {
      fprintf( stderr, "Failed to initialize GLFW\n" );
      return;
    }
    if (!glfwVulkanSupported()) {
      fprintf(stderr, "Vulkan is not available :(\n");
      return;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = std::shared_ptr<GLFWwindow>(
      glfwCreateWindow(kWindowWidthDefault, kWindowHeightDefault, ci.app_name.c_str(), NULL, NULL),
      [](GLFWwindow *w){ glfwDestroyWindow(w); });
    glfwSetInputMode(window_.get(), GLFW_STICKY_KEYS, 1);
    glfwPollEvents(); // dummy poll for first loop iteration

    input_state_.set_window(window_);
  }

  // Initialize Vulkan
  std::vector<const char*> required_instance_layer_names = {};
  if (ci.debug_report_flags != 0) {
    required_instance_layer_names.push_back("VK_LAYER_LUNARG_standard_validation");
  }
  std::vector<const char*> optional_instance_layer_names = {};
  std::vector<const char*> enabled_instance_layer_names = {};
  SPOKK_VK_CHECK(get_supported_instance_layers(
    required_instance_layer_names, optional_instance_layer_names,
    &instance_layers_, &enabled_instance_layer_names));

  std::vector<const char*> required_instance_extension_names = {
  };
  if (ci.enable_graphics) {
    required_instance_extension_names.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    required_instance_extension_names.push_back(PLATFORM_SURFACE_EXTENSION_NAME);
  }
  std::vector<const char*> optional_instance_extension_names = {};
  if (ci.debug_report_flags != 0) {
    optional_instance_extension_names.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
  }
  std::vector<const char*> enabled_instance_extension_names;
  SPOKK_VK_CHECK(get_supported_instance_extensions(instance_layers_,
    required_instance_extension_names, optional_instance_extension_names,
    &instance_extensions_, &enabled_instance_extension_names));

  VkApplicationInfo application_info = {};
  application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application_info.pApplicationName = ci.app_name.c_str();
  application_info.applicationVersion = 0x1000;
  application_info.pEngineName = "Spokk";
  application_info.engineVersion = 0x1001;
  application_info.apiVersion = VK_MAKE_VERSION(1,0,37);
  VkInstanceCreateInfo instance_ci = {};
  instance_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_ci.pApplicationInfo = &application_info;
  instance_ci.enabledLayerCount       = (uint32_t)enabled_instance_layer_names.size();
  instance_ci.ppEnabledLayerNames     = enabled_instance_layer_names.data();
  instance_ci.enabledExtensionCount   = (uint32_t)enabled_instance_extension_names.size();
  instance_ci.ppEnabledExtensionNames = enabled_instance_extension_names.data();
  SPOKK_VK_CHECK(vkCreateInstance(&instance_ci, host_allocator_, &instance_));

  if (is_instance_extension_enabled(VK_EXT_DEBUG_REPORT_EXTENSION_NAME)) {
    VkDebugReportCallbackCreateInfoEXT debug_report_callback_ci = {};
    debug_report_callback_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    debug_report_callback_ci.flags = ci.debug_report_flags;
    debug_report_callback_ci.pfnCallback = my_debug_report_callback;
    debug_report_callback_ci.pUserData = nullptr;
    auto create_debug_report_func = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance_,
      "vkCreateDebugReportCallbackEXT");
    SPOKK_VK_CHECK(create_debug_report_func(instance_, &debug_report_callback_ci, host_allocator_, &debug_report_callback_));
  }

  if (ci.enable_graphics) {
    SPOKK_VK_CHECK( glfwCreateWindowSurface(instance_, window_.get(), host_allocator_, &surface_) );
  }

  std::vector<uint32_t> queue_family_indices;
  SPOKK_VK_CHECK(find_physical_device(ci.queue_family_requests, instance_, surface_, &physical_device_, &queue_family_indices));
  std::vector<VkDeviceQueueCreateInfo> device_queue_cis = {};
  uint32_t total_queue_count = 0;
  for(uint32_t iQF=0; iQF<(uint32_t)ci.queue_family_requests.size(); ++iQF) {
    uint32_t queue_count = ci.queue_family_requests[iQF].queue_count;
    total_queue_count += queue_count;
  }
  std::vector<float> queue_priorities;
  queue_priorities.reserve(total_queue_count);
  for(uint32_t iQF=0; iQF<(uint32_t)ci.queue_family_requests.size(); ++iQF) {
    uint32_t queue_count = ci.queue_family_requests[iQF].queue_count;
    device_queue_cis.push_back({
      VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0,
      queue_family_indices[iQF], queue_count, queue_priorities.data()
    });
    queue_priorities.insert(queue_priorities.end(), queue_count, ci.queue_family_requests[iQF].priority);
  };
  assert(queue_priorities.size() == total_queue_count);

  std::vector<const char*> required_device_extension_names = {
  };
  if (ci.enable_graphics) {
    required_device_extension_names.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }
  const std::vector<const char*> optional_device_extension_names = {
#if !defined(NDEBUG) && defined(VK_EXT_debug_marker)
    VK_EXT_DEBUG_MARKER_EXTENSION_NAME, // will only be enabled if a layer supports it (currently, only RenderDoc's implicit layer)
#endif
  };
  std::vector<const char*> enabled_device_extension_names;
  SPOKK_VK_CHECK(get_supported_device_extensions(physical_device_, instance_layers_,
    required_device_extension_names, optional_device_extension_names,
    &device_extensions_, &enabled_device_extension_names));

  vkGetPhysicalDeviceFeatures(physical_device_, &physical_device_features_);

  VkDeviceCreateInfo device_ci = {};
  device_ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_ci.queueCreateInfoCount = (uint32_t)device_queue_cis.size();
  device_ci.pQueueCreateInfos = device_queue_cis.data();
  device_ci.enabledExtensionCount = (uint32_t)enabled_device_extension_names.size();
  device_ci.ppEnabledExtensionNames = enabled_device_extension_names.data();
  device_ci.pEnabledFeatures = &physical_device_features_;
  SPOKK_VK_CHECK(vkCreateDevice(physical_device_, &device_ci, host_allocator_, &device_));

  uint32_t total_queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &total_queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> all_queue_family_properties(total_queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &total_queue_family_count, all_queue_family_properties.data());
  queues_.reserve(total_queue_count);
  for(uint32_t iQFR=0; iQFR<(uint32_t)ci.queue_family_requests.size(); ++iQFR) {
    const QueueFamilyRequest &qfr = ci.queue_family_requests[iQFR];
    const VkDeviceQueueCreateInfo& qci = device_queue_cis[iQFR];
    const VkQueueFamilyProperties& qfp = all_queue_family_properties[qci.queueFamilyIndex];
    DeviceQueue qc = {
      VK_NULL_HANDLE,
      qci.queueFamilyIndex,
      0.0f,
      qfp.queueFlags,
      qfp.timestampValidBits,
      qfp.minImageTransferGranularity,
      (qfr.support_present && ((qfp.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)) ? surface_ : VK_NULL_HANDLE,
    };
    for(uint32_t iQ=0; iQ<total_queue_count; ++iQ) {
      vkGetDeviceQueue(device_, qci.queueFamilyIndex, iQ, &qc.handle);
      qc.priority = qci.pQueuePriorities[iQ];
      queues_.push_back(qc);
    }
  }
  assert(queues_.size() == total_queue_count);

  VkPipelineCacheCreateInfo pipeline_cache_ci = {};
  pipeline_cache_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  SPOKK_VK_CHECK(vkCreatePipelineCache(device_, &pipeline_cache_ci, host_allocator_, &pipeline_cache_));

  device_context_ = DeviceContext(device_, physical_device_, pipeline_cache_, queues_.data(),
    (uint32_t)queues_.size(), host_allocator_, device_allocator_);

  // Create VkSwapchain
  if (ci.enable_graphics && surface_ != VK_NULL_HANDLE) {
    VkSurfaceCapabilitiesKHR surface_caps = {};
    SPOKK_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &surface_caps));
    swapchain_extent_ = surface_caps.currentExtent;
    if ((int32_t)swapchain_extent_.width == -1) {
      assert( (int32_t)swapchain_extent_.height == -1 );
      swapchain_extent_.width =
        CDSVK__CLAMP(ci.window_width, surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width);
      swapchain_extent_.height =
        CDSVK__CLAMP(ci.window_height, surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height);
    }

    uint32_t device_surface_format_count = 0;
    std::vector<VkSurfaceFormatKHR> device_surface_formats;
    VkResult result = VK_INCOMPLETE;
    do {
      result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &device_surface_format_count, nullptr);
      if (result == VK_SUCCESS && device_surface_format_count > 0) {
        device_surface_formats.resize(device_surface_format_count);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &device_surface_format_count,
          device_surface_formats.data());
      }
    } while (result == VK_INCOMPLETE);
    if (device_surface_formats.size() == 1 && device_surface_formats[0].format == VK_FORMAT_UNDEFINED) {
      // No preferred format.
      swapchain_surface_format_.format = VK_FORMAT_B8G8R8A8_UNORM;
      swapchain_surface_format_.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else {
      assert(device_surface_formats.size() >= 1);
      swapchain_surface_format_ = device_surface_formats[0];
    }

    uint32_t device_present_mode_count = 0;
    std::vector<VkPresentModeKHR> device_present_modes;
    do {
      result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &device_present_mode_count, nullptr);
      if (result == VK_SUCCESS && device_present_mode_count > 0) {
        device_present_modes.resize(device_present_mode_count);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &device_present_mode_count,
          device_present_modes.data());
      }
    } while (result == VK_INCOMPLETE);
    VkPresentModeKHR present_mode;
    if (!ci.enable_vsync) {
      present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else {
      bool found_mailbox_mode = false;
      for(auto mode : device_present_modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
          found_mailbox_mode = true;
          break;
        }
      }
      present_mode = found_mailbox_mode ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    }

    uint32_t desired_swapchain_image_count = surface_caps.minImageCount+1;
    if (surface_caps.maxImageCount > 0 && desired_swapchain_image_count > surface_caps.maxImageCount) {
      desired_swapchain_image_count = surface_caps.maxImageCount;
    }

    VkSurfaceTransformFlagBitsKHR surface_transform = surface_caps.currentTransform;

    VkImageUsageFlags swapchain_image_usage = 0
      | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      ;
    assert( (surface_caps.supportedUsageFlags & swapchain_image_usage) == swapchain_image_usage );

    assert(surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    VkSwapchainKHR old_swapchain = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR swapchain_ci = {};
    swapchain_ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_ci.surface = surface_;
    swapchain_ci.minImageCount = desired_swapchain_image_count;
    swapchain_ci.imageFormat = swapchain_surface_format_.format;
    swapchain_ci.imageColorSpace = swapchain_surface_format_.colorSpace;
    swapchain_ci.imageExtent.width = swapchain_extent_.width;
    swapchain_ci.imageExtent.height = swapchain_extent_.height;
    swapchain_ci.imageArrayLayers = 1;
    swapchain_ci.imageUsage = swapchain_image_usage;
    swapchain_ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_ci.preTransform = surface_transform;
    swapchain_ci.compositeAlpha = composite_alpha;
    swapchain_ci.presentMode = present_mode;
    swapchain_ci.clipped = VK_TRUE;
    swapchain_ci.oldSwapchain = old_swapchain;
    SPOKK_VK_CHECK(vkCreateSwapchainKHR(device_, &swapchain_ci, host_allocator_, &swapchain_));
    if (old_swapchain != VK_NULL_HANDLE) {
      assert(0); // TODO(cort): handle this at some point
    }

    uint32_t swapchain_image_count = 0;
    do {
      result = vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count, nullptr);
      if (result == VK_SUCCESS && swapchain_image_count > 0) {
        swapchain_images_.resize(swapchain_image_count);
        result = vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count, swapchain_images_.data());
      }
    } while (result == VK_INCOMPLETE);
    VkImageViewCreateInfo image_view_ci = {};
    image_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_ci.image = VK_NULL_HANDLE; // filled in below
    image_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image_view_ci.format = swapchain_surface_format_.format;
    image_view_ci.components = {};
    image_view_ci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    image_view_ci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    image_view_ci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    image_view_ci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    image_view_ci.subresourceRange = {};
    image_view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_view_ci.subresourceRange.baseMipLevel = 0;
    image_view_ci.subresourceRange.levelCount = 1;
    image_view_ci.subresourceRange.baseArrayLayer = 0;
    image_view_ci.subresourceRange.layerCount = 1;
    swapchain_image_views_.reserve(swapchain_images_.size());
    for(auto image : swapchain_images_) {
      image_view_ci.image = image;
      VkImageView view = VK_NULL_HANDLE;
      SPOKK_VK_CHECK(vkCreateImageView(device_, &image_view_ci, host_allocator_, &view));
      swapchain_image_views_.push_back(view);
    }
  }

  graphics_and_present_queue_ = device_context_.find_queue(VK_QUEUE_GRAPHICS_BIT, surface_);

  // Allocate command buffers
  VkCommandPoolCreateInfo cpool_ci = {};
  cpool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpool_ci.queueFamilyIndex = graphics_and_present_queue_->family;
  SPOKK_VK_CHECK(vkCreateCommandPool(device_, &cpool_ci, host_allocator_, &primary_cpool_));
  VkCommandBufferAllocateInfo cb_allocate_info = {};
  cb_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_allocate_info.commandPool = primary_cpool_;
  cb_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_allocate_info.commandBufferCount = (uint32_t)primary_command_buffers_.size();
  SPOKK_VK_CHECK(vkAllocateCommandBuffers(device_, &cb_allocate_info, primary_command_buffers_.data()));

  // Create the semaphores used to synchronize access to swapchain images
  VkSemaphoreCreateInfo semaphore_ci = {};
  semaphore_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  SPOKK_VK_CHECK(vkCreateSemaphore(device_, &semaphore_ci, host_allocator_, &image_acquire_semaphore_));
  SPOKK_VK_CHECK(vkCreateSemaphore(device_, &semaphore_ci, host_allocator_, &submit_complete_semaphore_));

  // Create the fences used to wait for each swapchain image's command buffer to be submitted.
  // This prevents re-writing the command buffer contents before it's been submitted and processed.
  VkFenceCreateInfo fence_ci = {};
  fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for(auto &fence : submit_complete_fences_) {
    SPOKK_VK_CHECK(vkCreateFence(device_, &fence_ci, host_allocator_, &fence));
  }

  init_successful = true;
}
Application::~Application() {
  if (device_) {
    vkDeviceWaitIdle(device_);

    vkDestroySemaphore(device_, image_acquire_semaphore_, host_allocator_);
    vkDestroySemaphore(device_, submit_complete_semaphore_, host_allocator_);
    for(auto fence : submit_complete_fences_) {
      vkDestroyFence(device_, fence, host_allocator_);
    }
    vkDestroyCommandPool(device_, primary_cpool_, host_allocator_);

    vkDestroyPipelineCache(device_, pipeline_cache_, host_allocator_);

    if (swapchain_ != VK_NULL_HANDLE) {
      for(auto& view : swapchain_image_views_) {
        vkDestroyImageView(device_, view, host_allocator_);
        view = VK_NULL_HANDLE;
      }
      vkDestroySwapchainKHR(device_, swapchain_, host_allocator_);
      swapchain_ = VK_NULL_HANDLE;
    }
  }
  if (surface_ != VK_NULL_HANDLE) {
    window_.reset();
    glfwTerminate();
  }
  vkDestroyDevice(device_, host_allocator_);
  device_ = VK_NULL_HANDLE;
  if (debug_report_callback_ != VK_NULL_HANDLE) {
    auto destroy_debug_report_func = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(instance_, "vkDestroyDebugReportCallbackEXT");
    destroy_debug_report_func(instance_, debug_report_callback_, host_allocator_);
  }
  if (surface_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_, surface_, host_allocator_);
    surface_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, host_allocator_);
    instance_ = VK_NULL_HANDLE;
  }
}

int Application::run() {
  if (!init_successful) {
    return -1;
  }

  const uint64_t clock_start = zomboClockTicks();
  uint64_t ticks_prev = clock_start;
  frame_index_ = 0;
  vframe_index_ = 0;
  while(!force_exit_ && !glfwWindowShouldClose(window_.get())) {
    uint64_t ticks_now = zomboClockTicks();
    const double dt = (float)zomboTicksToSeconds(ticks_now - ticks_prev);
    ticks_prev = ticks_now;

    update(dt);
    if (force_exit_) {
      break;
    }

    // Wait for the command buffer previously used to generate this swapchain image to be submitted.
    // TODO(cort): this does not guarantee memory accesses from this submission will be visible on the host;
    // there'd need to be a memory barrier for that.
    vkWaitForFences(device_, 1, &submit_complete_fences_[vframe_index_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &submit_complete_fences_[vframe_index_]);

    // The host can now safely reset and rebuild this command buffer, even if the GPU hasn't finished presenting the
    // resulting frame yet.
    VkCommandBuffer cb = primary_command_buffers_[vframe_index_];

    // Retrieve the index of the next available swapchain index
    VkFence image_acquire_fence = VK_NULL_HANDLE; // currently unused, but if you want the CPU to wait for an image to be acquired...
    uint32_t swapchain_image_index = 0;
    VkResult acquire_result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
      image_acquire_semaphore_, image_acquire_fence, &swapchain_image_index);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
      assert(0); // TODO(cort): swapchain is out of date (e.g. resized window) and must be recreated.
    } else if (acquire_result == VK_SUBOPTIMAL_KHR) {
      // TODO(cort): swapchain is not as optimal as it could be, but it'll work. Just an FYI condition.
    } else {
      SPOKK_VK_CHECK(acquire_result);
    }

    VkCommandBufferBeginInfo cb_begin_info = {};
    cb_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SPOKK_VK_CHECK(vkBeginCommandBuffer(cb, &cb_begin_info) );

    // Applications-specific render code
    render(cb, swapchain_image_index);
    if (force_exit_) {
      break;
    }

    SPOKK_VK_CHECK( vkEndCommandBuffer(cb) );
    const VkPipelineStageFlags submit_wait_stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_acquire_semaphore_;
    submit_info.pWaitDstStageMask = &submit_wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cb;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &submit_complete_semaphore_;
    SPOKK_VK_CHECK( vkQueueSubmit(graphics_and_present_queue_->handle, 1,
      &submit_info, submit_complete_fences_[vframe_index_]) );
    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pNext = NULL;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &swapchain_image_index;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &submit_complete_semaphore_;
    VkResult present_result = vkQueuePresentKHR(graphics_and_present_queue_->handle, &present_info);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR) {
      assert(0); // TODO(cort): swapchain is out of date (e.g. resized window) and must be recreated.
    } else if (present_result == VK_SUBOPTIMAL_KHR) {
      // TODO(cort): swapchain is not as optimal as it could be, but it'll work. Just an FYI condition.
    } else {
      SPOKK_VK_CHECK(present_result);
    }

    glfwPollEvents();
    frame_index_ += 1;
    vframe_index_ += 1;
    if (vframe_index_ == VFRAME_COUNT) {
      vframe_index_ = 0;
    }
  }
  return 0;
}

void Application::update(double /*dt*/) {
  input_state_.Update();
}

bool Application::is_instance_layer_enabled(const std::string& layer_name) const {
  for(const auto &layer : instance_layers_) {
    if (layer_name == layer.layerName) {
      return true;
    }
  }
  return false;
}
bool Application::is_instance_extension_enabled(const std::string& extension_name) const {
  for(const auto &extension : instance_extensions_) {
    if (extension_name == extension.extensionName) {
      return true;
    }
  }
  return false;
}
bool Application::is_device_extension_enabled(const std::string& extension_name) const {
  for(const auto &extension : device_extensions_) {
    if (extension_name == extension.extensionName) {
      return true;
    }
  }
  return false;
}