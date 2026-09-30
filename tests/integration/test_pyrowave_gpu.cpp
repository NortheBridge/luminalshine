#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>

#if defined(_WIN32) && defined(SUNSHINE_ENABLE_PYROWAVE)
  #include <winsock2.h>

// Keep Winsock ahead of the Vulkan headers, which include windows.h.
  #include "pyrowave/pyrowave_common.h"
  #include "pyrowave/pyrowave_decoder.h"
  #include "src/platform/windows/display_vram.h"
  #include "src/pyrowave/contract.h"
  #include "src/pyrowave/pyrowave_encode.h"

  #include <array>
  #include <cmath>
  #include <d3d11.h>
  #include <dxgi1_2.h>
  #include <memory>
  #include <vector>
  #include <wrl/client.h>

namespace {
  using Microsoft::WRL::ComPtr;

  struct decode_resources {
    std::shared_ptr<pyrowave_vk::context> context;
    std::unique_ptr<PyroWave::Decoder> decoder;
    std::unique_ptr<PyroWave::DecoderInput> input;
    std::array<image_allocation, 3> planes;
    std::array<vk::raii::ImageView, 3> views {nullptr, nullptr, nullptr};
    buffer_allocation readback;
    vk::raii::CommandPool pool = nullptr;
    vk::raii::CommandBuffer command = nullptr;
    vk::raii::Fence fence = nullptr;
  };

  void expect_decoded_color(const std::shared_ptr<pyrowave_vk::context> &context, const std::vector<std::uint8_t> &frame, bool hdr, bool placeholder = false) {
    auto resources = std::make_unique<decode_resources>();
    resources->context = context;
    auto &device = context->device();
    resources->decoder = std::make_unique<PyroWave::Decoder>(context->physical_device(), device, 128, 128, PyroWave::ChromaSubsampling::Chroma420);
    resources->input = std::make_unique<PyroWave::DecoderInput>(*resources->decoder);
    // Match the client: fragment records at arbitrary network boundaries,
    // retaining parser state when no packet-loss gap was reported.
    for (size_t offset = 0; offset < frame.size(); offset += 1376) {
      ASSERT_TRUE(resources->input->push_data(std::span(frame).subspan(offset, std::min<size_t>(1376, frame.size() - offset))));
    }
    EXPECT_EQ(resources->input->parse_anomalies(), 0u);
    EXPECT_FALSE(resources->input->awaiting_state_init());
    resources->input->flush();

    for (unsigned i = 0; i < 3; ++i) {
      const unsigned size = i == 0 ? 128 : 64;
      resources->planes[i] = image_allocation(device, vk::ImageCreateInfo {.imageType = vk::ImageType::e2D, .format = vk::Format::eR32Sfloat, .extent = {size, size, 1}, .mipLevels = 1, .arrayLayers = 1, .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc}, {.usage = VMA_MEMORY_USAGE_AUTO}, "pyrowave test decoded plane");
      resources->views[i] = device.createImageView(vk::ImageViewCreateInfo {.image = resources->planes[i], .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR32Sfloat, .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
    }
    resources->readback = buffer_allocation(device, {.size = 3 * sizeof(float), .usage = vk::BufferUsageFlagBits::eTransferDst}, {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage = VMA_MEMORY_USAGE_AUTO, .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT}, "pyrowave test readback");
    resources->pool = device.createCommandPool(vk::CommandPoolCreateInfo {.queueFamilyIndex = context->caps().compute_queue_family});
    resources->command = std::move(device.allocateCommandBuffers(vk::CommandBufferAllocateInfo {.commandPool = *resources->pool, .commandBufferCount = 1})[0]);
    resources->fence = device.createFence({});
    auto &command = resources->command;
    command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    for (auto &plane : resources->planes) {
      command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, vk::ImageMemoryBarrier {.dstAccessMask = vk::AccessFlagBits::eShaderWrite, .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral, .image = plane, .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
    }
    ASSERT_TRUE(resources->decoder->decode(command, *resources->input, {*resources->views[0], *resources->views[1], *resources->views[2]}));
    for (unsigned i = 0; i < 3; ++i) {
      command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier {.srcAccessMask = vk::AccessFlagBits::eShaderWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead, .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eTransferSrcOptimal, .image = resources->planes[i], .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
      const int center = i == 0 ? 64 : 32;
      command.copyImageToBuffer(resources->planes[i], vk::ImageLayout::eTransferSrcOptimal, resources->readback, vk::BufferImageCopy {.bufferOffset = i * sizeof(float), .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1}, .imageOffset = {center, center, 0}, .imageExtent = {1, 1, 1}});
    }
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, vk::MemoryBarrier {.srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eHostRead}, {}, {});
    command.end();
    context->queue().submit(vk::SubmitInfo {.commandBufferCount = 1, .pCommandBuffers = &*command}, *resources->fence);
    if (device.waitForFences(*resources->fence, true, 5'000'000'000ull) != vk::Result::eSuccess) {
      (void) resources.release();
      FAIL() << "Decoder GPU submission timed out; retaining pending allocations until process exit";
    }
    const auto *actual = static_cast<const float *>(resources->readback.map());
    std::array<double, 3> rgb {224.0 / 255, 128.0 / 255, 32.0 / 255};
    if (placeholder) {
      rgb = {0, 0, 0};
    } else if (hdr) {
      rgb = {0.627404 * 2 + 0.329283 + 0.043313 * 0.5, 0.069097 * 2 + 0.919540 + 0.011362 * 0.5, 0.016391 * 2 + 0.088013 + 0.895595 * 0.5};
      for (auto &channel : rgb) {
        const auto power = std::pow(channel * 80.0 / 10000, 2610.0 / 16384);
        channel = std::pow((3424.0 / 4096 + (2413.0 / 128) * power) / (1 + (2392.0 / 128) * power), 2523.0 / 32);
      }
    }
    const auto [r, g, b] = rgb;
    const std::array<double, 3> expected = hdr ? std::array<double, 3> {
                                                   16.0 / 255 + 0.2256 * r + 0.5823 * g + 0.0509 * b,
                                                   128.0 / 255 - 0.1227 * r - 0.3166 * g + 0.4392 * b,
                                                   128.0 / 255 + 0.4392 * r - 0.4039 * g - 0.0353 * b
                                                 } :
                                                 std::array<double, 3> {16.0 / 255 + 0.1826 * r + 0.6142 * g + 0.0620 * b, 128.0 / 255 - 0.1006 * r - 0.3386 * g + 0.4392 * b, 128.0 / 255 + 0.4392 * r - 0.3989 * g - 0.0403 * b};
    for (unsigned i = 0; i < 3; ++i) {
      EXPECT_NEAR(actual[i], expected[i], 0.035) << "decoded Y/Cb/Cr plane " << i;
    }
  }

  struct safe_encoder_delete {
    void operator()(pyrowave_enc::pyrowave_encode_device_t *device) const {
      if (!device) {
        return;
      }
      // A failing smoke test must not destroy allocations still used by the GPU.
      // The test process owns any deliberately retained failure-path resources.
      if (device->safe_to_destroy()) {
        delete device;
      }
    }
  };
}  // namespace
#endif

TEST(PyrowaveGpu, SharedD3D11SdrAndHdrEncodeAndIdleRefresh) {
#if !defined(_WIN32) || !defined(SUNSHINE_ENABLE_PYROWAVE)
  GTEST_SKIP() << "Requires Windows and SUNSHINE_ENABLE_PYROWAVE";
#else
  const char *enabled = std::getenv("LUMINALSHINE_TEST_PYROWAVE_GPU");
  if (!enabled || std::strcmp(enabled, "1") != 0) {
    GTEST_SKIP() << "Set LUMINALSHINE_TEST_PYROWAVE_GPU=1 for the real GPU interop smoke test";
  }

  auto owned_context = pyrowave_vk::context::create();
  ASSERT_NE(owned_context, nullptr) << "No supported PyroWave Vulkan device";
  std::shared_ptr<pyrowave_vk::context> context(std::move(owned_context));
  const auto properties = context->physical_device().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
  const auto &identity = properties.get<vk::PhysicalDeviceIDProperties>();
  ASSERT_TRUE(identity.deviceLUIDValid);

  ComPtr<IDXGIFactory1> factory;
  ASSERT_EQ(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), S_OK);
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> candidate;
    const auto status = factory->EnumAdapters1(index, candidate.GetAddressOf());
    if (status == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    ASSERT_EQ(status, S_OK);
    DXGI_ADAPTER_DESC1 desc {};
    ASSERT_EQ(candidate->GetDesc1(&desc), S_OK);
    if (std::memcmp(&desc.AdapterLuid, identity.deviceLUID.data(), sizeof(LUID)) == 0) {
      adapter = std::move(candidate);
      break;
    }
  }
  ASSERT_NE(adapter.Get(), nullptr) << "Vulkan adapter has no matching DXGI adapter";

  ComPtr<ID3D11Device> d3d_device;
  ComPtr<ID3D11DeviceContext> d3d_context;
  ASSERT_EQ(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d_device.GetAddressOf(), nullptr, d3d_context.GetAddressOf()), S_OK);

  constexpr UINT width = 128, height = 128;
  for (const bool hdr : {false, true}) {
    SCOPED_TRACE(hdr ? "HDR scRGB to BT.2020/PQ" : "SDR BGRA to BT.709");
    platf::dxgi::img_d3d_t image;
    image.width = width;
    image.height = height;
    image.pixel_pitch = hdr ? 8 : 4;
    image.row_pitch = image.width * image.pixel_pitch;
    image.format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    image.blank = false;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = image.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ID3D11Texture2D *texture = nullptr;
    ASSERT_EQ(d3d_device->CreateTexture2D(&desc, nullptr, &texture), S_OK);
    image.capture_texture.reset(texture);
    IDXGIKeyedMutex *mutex = nullptr;
    ASSERT_EQ(texture->QueryInterface(IID_PPV_ARGS(&mutex)), S_OK);
    image.capture_mutex.reset(mutex);
    ComPtr<IDXGIResource1> resource;
    ASSERT_EQ(texture->QueryInterface(IID_PPV_ARGS(resource.GetAddressOf())), S_OK);
    ASSERT_EQ(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &image.encoder_texture_handle), S_OK);

    std::vector<std::uint8_t> pixels(size_t(image.row_pitch) * height);
    for (size_t offset = 0; offset < pixels.size(); offset += image.pixel_pitch) {
      if (hdr) {
        // Exact half-floats: scRGB R=2, G=1, B=0.5, A=1 (160/80/40 nits).
        const std::array<std::uint16_t, 4> rgba {0x4000, 0x3c00, 0x3800, 0x3c00};
        std::memcpy(pixels.data() + offset, rgba.data(), sizeof(rgba));
      } else {
        const std::array<std::uint8_t, 4> bgra {32, 128, 224, 255};
        std::memcpy(pixels.data() + offset, bgra.data(), sizeof(bgra));
      }
    }
    ASSERT_EQ(image.capture_mutex->AcquireSync(0, 1000), S_OK);
    d3d_context->UpdateSubresource(texture, 0, nullptr, pixels.data(), image.row_pitch, 0);
    d3d_context->Flush();
    ASSERT_EQ(image.capture_mutex->ReleaseSync(0), S_OK);

    video::config_t config {};
    config.encoderCscMode = 2;
    config.dynamicRange = hdr ? 1 : 0;
    const auto colorspace = video::colorspace_from_client_config(config, hdr);
    auto created = pyrowave_enc::pyrowave_encode_device_t::create(context, width, height, 700'000'000, 60, colorspace, 1376);
    ASSERT_NE(created, nullptr);
    std::unique_ptr<pyrowave_enc::pyrowave_encode_device_t, safe_encoder_delete> encoder(created.release());
    encoder->max_output_bytes = pyrowave::max_frame_bytes(1392);
    if (hdr) {
      // Before the first VGD ring frame, capture_format is unknown and the
      // existing capture path creates a BGRA8 dummy even for an HDR session.
      platf::dxgi::img_d3d_t placeholder;
      placeholder.width = width;
      placeholder.height = height;
      placeholder.format = DXGI_FORMAT_B8G8R8A8_UNORM;
      auto placeholder_desc = desc;
      placeholder_desc.Format = placeholder.format;
      ID3D11Texture2D *placeholder_texture = nullptr;
      ASSERT_EQ(d3d_device->CreateTexture2D(&placeholder_desc, nullptr, &placeholder_texture), S_OK);
      placeholder.capture_texture.reset(placeholder_texture);
      ComPtr<IDXGIResource1> placeholder_resource;
      ASSERT_EQ(placeholder_texture->QueryInterface(IID_PPV_ARGS(placeholder_resource.GetAddressOf())), S_OK);
      ASSERT_EQ(placeholder_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &placeholder.encoder_texture_handle), S_OK);
      // A real 8-bit source must still be refused by an HDR encoder.
      ASSERT_NE(encoder->convert(placeholder), 0);
      placeholder.dummy = true;
      ASSERT_EQ(encoder->convert(placeholder), 0);
      const auto bootstrap = encoder->encode_frame(0);
      ASSERT_FALSE(bootstrap.data.empty());
      expect_decoded_color(context, bootstrap.data, true, true);
    }
    ASSERT_EQ(encoder->convert(image), 0);

    unsigned first_sequence = 0;
    for (std::int64_t frame_index : {1, 2}) {
      // The second frame has no convert(): static desktops and IDR requests
      // must still produce a decodable full refresh from the retained planes.
      if (frame_index == 2) {
        encoder->request_full_refresh();
      }
      const auto frame = encoder->encode_frame(frame_index);
      ASSERT_GE(frame.data.size(), sizeof(PyroWave::BitstreamSequenceHeader));
      EXPECT_LE(frame.data.size(), encoder->max_output_bytes);
      EXPECT_EQ(frame.frame_index, frame_index);
      EXPECT_TRUE(frame.idr);
      PyroWave::BitstreamSequenceHeader header {};
      std::memcpy(&header, frame.data.data(), sizeof(header));
      EXPECT_EQ(static_cast<unsigned>(header.width_minus_1), width - 1);
      EXPECT_EQ(static_cast<unsigned>(header.height_minus_1), height - 1);
      EXPECT_EQ(static_cast<unsigned>(header.code), PyroWave::BITSTREAM_EXTENDED_CODE_START_OF_FRAME);
      EXPECT_EQ(static_cast<unsigned>(header.color_primaries), hdr ? PyroWave::COLOR_PRIMARIES_BT2020 : PyroWave::COLOR_PRIMARIES_BT709);
      EXPECT_EQ(static_cast<unsigned>(header.transfer_function), hdr ? PyroWave::TRANSFER_FUNCTION_PQ : PyroWave::TRANSFER_FUNCTION_BT709);
      expect_decoded_color(context, frame.data, hdr);
      if (frame_index == 1) {
        first_sequence = header.sequence;
      } else {
        EXPECT_NE(static_cast<unsigned>(header.sequence), first_sequence);
      }
    }
    ASSERT_TRUE(encoder->safe_to_destroy());
    // Encoding has returned the pooled image's mutex to its capture owner.
    ASSERT_EQ(image.capture_mutex->AcquireSync(0, 1000), S_OK);
    EXPECT_EQ(image.capture_mutex->ReleaseSync(0), S_OK);
  }
#endif
}
