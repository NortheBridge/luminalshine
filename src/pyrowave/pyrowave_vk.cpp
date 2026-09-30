/**
 * @file src/pyrowave/pyrowave_vk.cpp
 * @brief Implementation of the dedicated Vulkan context for PyroWave.
 *
 * Compiles against the vendored pyrowave library's public headers; built only
 * when SUNSHINE_ENABLE_PYROWAVE is ON.
 */
#include "pyrowave_vk.h"

#include <algorithm>
#include <cstring>
#include <vector>



// VMA is provided by the vendored pyrowave library.
#include <vk_mem_alloc.h>

#include "src/logging.h"

namespace pyrowave_vk {

  namespace {
    std::mutex context_mutex;

    /// Pick a physical device, preferring one whose LUID matches the capture GPU.
    int select_physical_device(const std::vector<vk::raii::PhysicalDevice> &devices, const uint8_t *prefer_luid) {
      int fallback = -1;
      for (int i = 0; i < (int) devices.size(); ++i) {
        auto props = devices[i].getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
        const auto &p = props.get<vk::PhysicalDeviceProperties2>().properties;
        const auto &id = props.get<vk::PhysicalDeviceIDProperties>();

        if (prefer_luid && id.deviceLUIDValid && std::memcmp(id.deviceLUID.data(), prefer_luid, VK_LUID_SIZE) == 0) {
          return i;
        }
        if (fallback < 0 || p.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
          fallback = i;
        }
      }
      return prefer_luid ? -1 : fallback;
    }

    /// Find a queue family that supports compute. Prefer a compute-only
    /// (async compute) family so the encode overlaps the game's rendering on
    /// the graphics queue instead of contending with it; fall back to the
    /// first compute-capable family.
    uint32_t find_compute_family(vk::raii::PhysicalDevice &dev) {
      auto families = dev.getQueueFamilyProperties();
      uint32_t fallback = UINT32_MAX;
      for (uint32_t i = 0; i < families.size(); ++i) {
        if (!(families[i].queueFlags & vk::QueueFlagBits::eCompute)) {
          continue;
        }
        if (!(families[i].queueFlags & vk::QueueFlagBits::eGraphics)) {
          return i;  // dedicated/async compute family
        }
        if (fallback == UINT32_MAX) {
          fallback = i;
        }
      }
      return fallback;
    }

  }  // namespace

  context::context(): lifetime_lock(context_mutex, std::try_to_lock) {}

  std::unique_ptr<context> context::create(const uint8_t *prefer_luid) {
    try {
      auto self = std::unique_ptr<context>(new context());
      if (!self->lifetime_lock.owns_lock()) return nullptr;

      // --- Instance --------------------------------------------------------
      // Use the highest version the loader offers (capped at 1.3): the codec
      // queries VkPhysicalDeviceVulkan13Properties for subgroup-size info, which
      // the driver only fills when the instance is >= 1.3. A Vulkan 1.3
      // physical device is required below.
      uint32_t loader_version = self->ctx.enumerateInstanceVersion();
      uint32_t api_version = std::min(loader_version, (uint32_t) VK_API_VERSION_1_3);
      vk::ApplicationInfo app_info {
        .pApplicationName = "sunshine-pyrowave",
        .apiVersion = api_version,
      };
      vk::InstanceCreateInfo inst_info {
        .pApplicationInfo = &app_info,
      };
      self->inst = vk::raii::Instance(self->ctx, inst_info);

      // --- Physical device --------------------------------------------------
      vk::raii::PhysicalDevices phys_devices(self->inst);
      if (phys_devices.empty()) {
        BOOST_LOG(error) << "pyrowave: no Vulkan physical devices";
        return nullptr;
      }
      int idx = select_physical_device(phys_devices, prefer_luid);
      if (idx < 0) {
        return nullptr;
      }
      self->phys_dev = std::move(phys_devices[idx]);

      if (self->phys_dev.getProperties().apiVersion < VK_API_VERSION_1_3) return nullptr;
      uint32_t cqf = find_compute_family(self->phys_dev);
      if (cqf == UINT32_MAX) {
        BOOST_LOG(error) << "pyrowave: no compute queue family";
        return nullptr;
      }
      self->caps_.compute_queue_family = cqf;

      // --- Device extensions ------------------------------------------------
      // On a Vulkan 1.1 device the 1.2/1.3 feature aggregates are invalid in the
      // device-create chain, so we enable the codec's needs through the
      // individual KHR/EXT extensions (8-bit storage, subgroup size control,
      // fp16, timeline semaphore). 16-bit storage and YCbCr conversion are core
      // 1.1. Mirrors WiVRn's "Lower Vulkan requirement to 1.1" device setup.
      static const char *wanted_exts[] = {
        VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        // The codec records vkCmdPipelineBarrier2 (Synchronization2); enable the
        // extension so it is available on 1.1 devices (null fn ptr -> crash otherwise).
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        // Zero-copy capture: import Sunshine's DRM-PRIME (dma-buf) capture frame
        // directly as a VkImage instead of a system-memory roundtrip + upload.
        // external_memory is core 1.1; the fd/dma_buf/drm-modifier bits are EXT/KHR.
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,
      };
      // Whether the dma-buf import path is usable (all 3 import exts present).
      std::vector<const char *> enabled_exts;
      {
        auto avail = self->phys_dev.enumerateDeviceExtensionProperties();
        for (auto *w : wanted_exts) {
          for (auto &e : avail) {
            if (std::strcmp(e.extensionName, w) == 0) { enabled_exts.push_back(w); break; }
          }
        }
      }
      if (std::find(enabled_exts.begin(), enabled_exts.end(), wanted_exts[5]) == enabled_exts.end() ||
          std::find(enabled_exts.begin(), enabled_exts.end(), wanted_exts[6]) == enabled_exts.end()) {
        BOOST_LOG(error) << "PyroWave requires Windows external memory and keyed mutex interoperability";
        return nullptr;
      }
      // --- Feature query (1.1 aggregate + individual extension structs) -----
      auto supported = self->phys_dev.getFeatures2<
        vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceVulkan11Features,
        vk::PhysicalDevice8BitStorageFeatures,
        vk::PhysicalDeviceSubgroupSizeControlFeatures,
        vk::PhysicalDeviceShaderFloat16Int8Features,
        vk::PhysicalDeviceTimelineSemaphoreFeatures,
        vk::PhysicalDeviceSynchronization2Features>();
      auto &q11 = supported.get<vk::PhysicalDeviceVulkan11Features>();
      auto &q8 = supported.get<vk::PhysicalDevice8BitStorageFeatures>();
      auto &qsg = supported.get<vk::PhysicalDeviceSubgroupSizeControlFeatures>();
      auto &qf16 = supported.get<vk::PhysicalDeviceShaderFloat16Int8Features>();
      auto &qts = supported.get<vk::PhysicalDeviceTimelineSemaphoreFeatures>();
      auto &qsync = supported.get<vk::PhysicalDeviceSynchronization2Features>();

      // Diagnostic toggle: PYROWAVE_NO_FP16 forces the fp32 wavelet path. The codec
      // selects fp16 vs fp32 shader variants from the ENABLED shaderFloat16 feature,
      // so masking it here makes the codec use fp32 (A/B precision testing).
      bool fp16_ok = qf16.shaderFloat16 && (std::getenv("PYROWAVE_NO_FP16") == nullptr);
      if (!fp16_ok && qf16.shaderFloat16) {
        BOOST_LOG(warning) << "pyrowave: PYROWAVE_NO_FP16 set -> forcing fp32 wavelet path";
      }
      qf16.shaderFloat16 = fp16_ok;

      self->caps_.shader_float16 = fp16_ok;
      self->caps_.timeline_semaphore = qts.timelineSemaphore;
      self->caps_.ycbcr_conversion = q11.samplerYcbcrConversion;



      // --- Logical device ---------------------------------------------------
      float prio = 1.0f;
      vk::DeviceQueueCreateInfo queue_info {
        .queueFamilyIndex = cqf,
        .queueCount = 1,
        .pQueuePriorities = &prio,
      };

      // The codec writes storage images without a format and uses 16-bit ints.
      vk::PhysicalDeviceFeatures base_features {};
      base_features.shaderStorageImageWriteWithoutFormat = true;
      base_features.shaderStorageImageExtendedFormats = supported.get<vk::PhysicalDeviceFeatures2>().features.shaderStorageImageExtendedFormats;
      base_features.shaderInt16 = supported.get<vk::PhysicalDeviceFeatures2>().features.shaderInt16;

      vk::StructureChain<
        vk::DeviceCreateInfo,
        vk::PhysicalDeviceVulkan11Features,
        vk::PhysicalDevice8BitStorageFeatures,
        vk::PhysicalDeviceSubgroupSizeControlFeatures,
        vk::PhysicalDeviceShaderFloat16Int8Features,
        vk::PhysicalDeviceTimelineSemaphoreFeatures,
        vk::PhysicalDeviceSynchronization2Features>
        dev_chain;

      auto &dci = dev_chain.get<vk::DeviceCreateInfo>();
      dci.queueCreateInfoCount = 1;
      dci.pQueueCreateInfos = &queue_info;
      dci.enabledExtensionCount = (uint32_t) enabled_exts.size();
      dci.ppEnabledExtensionNames = enabled_exts.data();
      dci.pEnabledFeatures = &base_features;

      auto &e11 = dev_chain.get<vk::PhysicalDeviceVulkan11Features>();
      e11.samplerYcbcrConversion = q11.samplerYcbcrConversion;
      e11.storageBuffer16BitAccess = q11.storageBuffer16BitAccess;  // core 1.1

      dev_chain.get<vk::PhysicalDevice8BitStorageFeatures>().storageBuffer8BitAccess = q8.storageBuffer8BitAccess;
      auto &esg = dev_chain.get<vk::PhysicalDeviceSubgroupSizeControlFeatures>();
      esg.subgroupSizeControl = qsg.subgroupSizeControl;
      esg.computeFullSubgroups = qsg.computeFullSubgroups;
      dev_chain.get<vk::PhysicalDeviceShaderFloat16Int8Features>().shaderFloat16 = qf16.shaderFloat16;
      dev_chain.get<vk::PhysicalDeviceTimelineSemaphoreFeatures>().timelineSemaphore = qts.timelineSemaphore;
      dev_chain.get<vk::PhysicalDeviceSynchronization2Features>().synchronization2 = qsync.synchronization2;

      if (!qsync.synchronization2 || !q11.storageBuffer16BitAccess || !q8.storageBuffer8BitAccess ||
          !supported.get<vk::PhysicalDeviceFeatures2>().features.shaderStorageImageWriteWithoutFormat) {
        BOOST_LOG(error) << "PyroWave required Vulkan features unavailable";
        return nullptr;
      }
      // Link each feature struct based on whether the FEATURE is supported, not on
      // whether it is exposed as an extension (on 1.2/1.3 devices these are core
      // and not enumerable as extensions, but the structs/queries are still valid).
      // Extension NAMES were only added to enabled_exts above when enumerable.
      if (!q8.storageBuffer8BitAccess)
        dev_chain.unlink<vk::PhysicalDevice8BitStorageFeatures>();
      if (!(qsg.subgroupSizeControl && qsg.computeFullSubgroups))
        dev_chain.unlink<vk::PhysicalDeviceSubgroupSizeControlFeatures>();
      if (!qf16.shaderFloat16)
        dev_chain.unlink<vk::PhysicalDeviceShaderFloat16Int8Features>();
      if (!qts.timelineSemaphore)
        dev_chain.unlink<vk::PhysicalDeviceTimelineSemaphoreFeatures>();
      if (!qsync.synchronization2)
        dev_chain.unlink<vk::PhysicalDeviceSynchronization2Features>();

      self->dev = vk::raii::Device(self->phys_dev, dev_chain.get<vk::DeviceCreateInfo>());
      self->compute_queue = vk::raii::Queue(self->dev, cqf, 0);

      // --- VMA allocator singleton (required by the codec) ------------------
      // VMA needs explicit function pointers under vulkan_raii's dynamic
      // dispatcher; it resolves the rest from these two entry points.
      VmaVulkanFunctions vma_fns {};
      vma_fns.vkGetInstanceProcAddr = self->ctx.getDispatcher()->vkGetInstanceProcAddr;
      vma_fns.vkGetDeviceProcAddr = self->dev.getDispatcher()->vkGetDeviceProcAddr;

      VmaAllocatorCreateInfo aci {};
      aci.physicalDevice = *self->phys_dev;
      aci.device = *self->dev;
      aci.instance = *self->inst;
      // VMA must use the version the DEVICE implements (instance may be 1.3 for the
      // codec's property query while the physical device is only 1.1; using 1.3
      // here would make VMA load 1.3-core functions the device lacks and assert).
      aci.vulkanApiVersion = std::min(api_version, self->phys_dev.getProperties().apiVersion);
      aci.pVulkanFunctions = &vma_fns;

      self->allocator.emplace(aci, /*has_debug_utils=*/false);

      BOOST_LOG(info) << "pyrowave: Vulkan context ready (fp16="
                      << self->caps_.shader_float16 << ")";
      return self;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "pyrowave: failed to create Vulkan context: " << e.what();
      return nullptr;
    }
  }

  context::~context() {
    // Destruction order: allocator (VMA singleton) must be torn down before the
    // device/instance. std::optional<vk_allocator> + raii members handle this as
    // long as `allocator` is declared after the handles (it is) — reset it first
    // to be explicit.
    allocator.reset();
  }



  imported_image context::import_d3d11(void *handle, uint32_t width, uint32_t height, vk::Format format) {
    imported_image out;
    constexpr auto kind = vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture;
    vk::ExternalMemoryImageCreateInfo external {.handleTypes = kind};
    out.image = dev.createImage(vk::ImageCreateInfo {
      .pNext = &external, .imageType = vk::ImageType::e2D, .format = format,
      .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
      .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
      .usage = vk::ImageUsageFlagBits::eSampled, .sharingMode = vk::SharingMode::eExclusive});
    auto req = out.image.getMemoryRequirements();
    auto imported = dev.getMemoryWin32HandlePropertiesKHR(kind, handle);
    uint32_t types = req.memoryTypeBits & imported.memoryTypeBits;
    if (!types) throw std::runtime_error("PyroWave D3D11 memory type unavailable");
    uint32_t index = 0;
    while (!(types & (1u << index))) ++index;
    vk::MemoryDedicatedAllocateInfo dedicated {.image = *out.image};
    vk::ImportMemoryWin32HandleInfoKHR info {.pNext = &dedicated, .handleType = kind, .handle = handle};
    out.memory = dev.allocateMemory(vk::MemoryAllocateInfo {
      .pNext = &info, .allocationSize = req.size, .memoryTypeIndex = index});
    out.image.bindMemory(*out.memory, 0);
    out.view = dev.createImageView(vk::ImageViewCreateInfo {
      .image = *out.image, .viewType = vk::ImageViewType::e2D, .format = format,
      .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
    out.width = width; out.height = height; out.format = format;
    return out;
  }
} // namespace pyrowave_vk
