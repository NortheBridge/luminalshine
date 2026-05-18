/**
 * @file src/amf/amf_caps.cpp
 */
#include "amf_caps.h"

#include "src/logging.h"

#include <mutex>
#include <optional>

#ifdef _WIN32

  // clang-format off
  #include <winsock2.h>
  #include <windows.h>
  #include <dxgi.h>
  #include <wchar.h>
  // clang-format on

namespace amf_caps {

  namespace {

    // --------------------------------------------------------------------
    // Minimal hand-rolled AMF SDK declarations. Just enough to call
    //   1. AMFInit(version, &factory)
    //   2. factory->CreateContext(&ctx)
    //   3. ctx->InitDX11(d3d11_device, dx11_amf_engine_type)
    //   4. factory->CreateComponent(ctx, encoder_id, &component)
    //   5. component->GetCaps(&caps)
    //   6. caps->GetProperty(property_name, &value)
    //
    // All of this is declared as opaque vtables matching the AMF C++
    // ABI exactly. We do NOT vendor the SDK headers — there's no need
    // for them when the only purpose is a one-shot capability probe.
    // --------------------------------------------------------------------

    constexpr std::uint64_t k_amf_full_version = (1ull << 48) | (4ull << 32) | (36ull << 16) | 0ull;

    using AMF_RESULT = std::int32_t;
    constexpr AMF_RESULT AMF_OK = 0;

    enum AMF_VARIANT_TYPE : std::int32_t {
      AMF_VARIANT_EMPTY = 0,
      AMF_VARIANT_BOOL = 1,
      AMF_VARIANT_INT64 = 2,
      AMF_VARIANT_DOUBLE = 3,
      AMF_VARIANT_STRING = 8,
      AMF_VARIANT_WSTRING = 9,
    };

    struct AMFVariantStruct {
      AMF_VARIANT_TYPE type;
      union {
        std::int64_t int64Value;
        bool boolValue;
        double doubleValue;
        char *stringValue;
        wchar_t *wstringValue;
        void *pInterface;
      };
    };

    // Forward decls.
    struct AMFCaps;
    struct AMFComponent;
    struct AMFContext;
    struct AMFFactory;

    // AMFCaps minimal vtable. Order matches the official SDK header
    // `public/include/components/Component.h`.
    struct AMFCapsVtbl {
      // AMFInterface
      void *(*QueryInterface)(AMFCaps *, const void *iid, void **out);
      std::uint64_t (*Acquire)(AMFCaps *);
      std::uint64_t (*Release)(AMFCaps *);
      // AMFPropertyStorage
      AMF_RESULT(*SetProperty)
      (AMFCaps *, const wchar_t *name, AMFVariantStruct value);
      AMF_RESULT(*GetProperty)
      (AMFCaps *, const wchar_t *name, AMFVariantStruct *value);
      bool (*HasProperty)(AMFCaps *, const wchar_t *name);
      // ... additional methods exist but we don't call them.
    };
    struct AMFCaps {
      AMFCapsVtbl *vtbl;
    };

    // AMFComponent minimal vtable; we only call GetCaps + Release.
    struct AMFComponentVtbl {
      void *(*QueryInterface)(AMFComponent *, const void *iid, void **out);
      std::uint64_t (*Acquire)(AMFComponent *);
      std::uint64_t (*Release)(AMFComponent *);
      // AMFPropertyStorage (5 entries; skipped)
      void *pad_propstorage[5];
      // AMFPropertyStorageEx (3 entries; skipped)
      void *pad_propstorageex[3];
      // AMFComponent
      AMF_RESULT(*Init)
      (AMFComponent *, std::int32_t format, std::int32_t width, std::int32_t height);
      AMF_RESULT(*ReInit)(AMFComponent *, std::int32_t, std::int32_t);
      AMF_RESULT(*Terminate)(AMFComponent *);
      AMF_RESULT(*Drain)(AMFComponent *);
      AMF_RESULT(*Flush)(AMFComponent *);
      AMF_RESULT(*SubmitInput)(AMFComponent *, void *);
      AMF_RESULT(*QueryOutput)(AMFComponent *, void **);
      void *(*GetContext)(AMFComponent *);
      AMF_RESULT(*SetOutputDataAllocatorCB)(AMFComponent *, void *);
      AMF_RESULT(*GetCaps)(AMFComponent *, AMFCaps **caps);
      // ... additional methods; not needed.
    };
    struct AMFComponent {
      AMFComponentVtbl *vtbl;
    };

    enum AMF_DX_VERSION : std::int32_t {
      AMF_DX9 = 90,
      AMF_DX9_EX = 91,
      AMF_DX11_0 = 110,
      AMF_DX11_1 = 111,
      AMF_DX12 = 120,
    };

    // AMFContext minimal vtable.
    struct AMFContextVtbl {
      void *(*QueryInterface)(AMFContext *, const void *iid, void **out);
      std::uint64_t (*Acquire)(AMFContext *);
      std::uint64_t (*Release)(AMFContext *);
      // AMFPropertyStorage (5)
      void *pad_propstorage[5];
      // AMFContext
      AMF_RESULT(*Terminate)(AMFContext *);
      AMF_RESULT(*InitDX9)(AMFContext *, void *device);
      AMF_RESULT(*InitDX11)(AMFContext *, void *device, AMF_DX_VERSION dx_version);
      // ... additional Init* / surface / buffer methods exist; not called.
    };
    struct AMFContext {
      AMFContextVtbl *vtbl;
    };

    // AMFFactory minimal vtable.
    struct AMFFactoryVtbl {
      AMF_RESULT(*CreateContext)
      (AMFFactory *, AMFContext **out);
      AMF_RESULT(*CreateComponent)
      (AMFFactory *, AMFContext *ctx, const wchar_t *encoder_id, AMFComponent **out);
      AMF_RESULT(*SetCacheFolder)(AMFFactory *, const wchar_t *);
      const wchar_t *(*GetCacheFolder)(AMFFactory *);
      AMF_RESULT(*GetDebug)(AMFFactory *, void **);
      AMF_RESULT(*GetTrace)(AMFFactory *, void **);
      AMF_RESULT(*GetPrograms)(AMFFactory *, void **);
    };
    struct AMFFactory {
      AMFFactoryVtbl *vtbl;
    };

    using AMFInit_fn = AMF_RESULT (*)(std::uint64_t version, AMFFactory **factory);
    using AMFQueryVersion_fn = AMF_RESULT (*)(std::uint64_t *version);

    // AMF encoder component identifiers (wstring literals as in the
    // SDK's VideoEncoderVCE.h / VideoEncoderHEVC.h / VideoEncoderAV1.h).
    constexpr const wchar_t *kEncoderId_AVC = L"AMFVideoEncoderVCE_AVC";
    constexpr const wchar_t *kEncoderId_HEVC = L"AMFVideoEncoder_HEVC";
    constexpr const wchar_t *kEncoderId_AV1 = L"AMFVideoEncoder_AV1";

    // Capability property names. These wstring literals are stable
    // public API surface: changing them breaks consumers, so AMD pins
    // them across SDK versions.
    constexpr const wchar_t *kCaps_MaxNumOfHWInstances = L"NumOfHWInstances";

    // Friendly RAII helper.
    template <typename T>
    struct amf_releaser {
      T *ptr = nullptr;
      ~amf_releaser() {
        if (ptr && ptr->vtbl && ptr->vtbl->Release) {
          ptr->vtbl->Release(ptr);
        }
      }
      T *operator->() const {
        return ptr;
      }
      operator bool() const {
        return ptr != nullptr;
      }
    };

    std::int64_t read_int64_prop(AMFCaps *caps, const wchar_t *name) {
      if (!caps || !caps->vtbl || !caps->vtbl->GetProperty) {
        return 0;
      }
      AMFVariantStruct v {};
      v.type = AMF_VARIANT_EMPTY;
      if (caps->vtbl->GetProperty(caps, name, &v) != AMF_OK) {
        return 0;
      }
      if (v.type == AMF_VARIANT_INT64) {
        return v.int64Value;
      }
      if (v.type == AMF_VARIANT_BOOL) {
        return v.boolValue ? 1 : 0;
      }
      return 0;
    }

    // Build a tiny D3D11 device just so InitDX11 can bind to the
    // user's primary AMD adapter. We DO NOT keep this device — the
    // probe is one-shot.
    bool create_probe_d3d11(IUnknown **out_device, std::string &adapter_desc) {
      using PFN_D3D11CreateDevice = HRESULT(WINAPI *)(
        IDXGIAdapter *, std::int32_t /*D3D_DRIVER_TYPE*/, HMODULE, std::uint32_t,
        const std::int32_t * /*feature levels*/, std::uint32_t, std::uint32_t,
        void ** /*ID3D11Device*/, std::int32_t * /*level*/, void ** /*ctx*/);

      HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
      if (!d3d11) {
        return false;
      }
      auto fn = reinterpret_cast<PFN_D3D11CreateDevice>(
        GetProcAddress(d3d11, "D3D11CreateDevice")
      );
      if (!fn) {
        FreeLibrary(d3d11);
        return false;
      }

      // Find the first AMD adapter via DXGI so we bind AMF to the
      // right GPU on hybrid systems.
      IDXGIFactory *factory = nullptr;
      using PFN_CreateDXGIFactory = HRESULT(WINAPI *)(REFIID, void **);
      HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
      if (!dxgi) {
        FreeLibrary(d3d11);
        return false;
      }
      auto pCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(
        GetProcAddress(dxgi, "CreateDXGIFactory")
      );
      if (!pCreateDXGIFactory ||
          FAILED(pCreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void **>(&factory)))) {
        FreeLibrary(dxgi);
        FreeLibrary(d3d11);
        return false;
      }

      IDXGIAdapter *chosen = nullptr;
      IDXGIAdapter *adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc {};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
          // AMD vendor ID = 0x1002.
          if (desc.VendorId == 0x1002) {
            chosen = adapter;
            char buf[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
            adapter_desc = buf;
            break;
          }
        }
        adapter->Release();
      }

      if (!chosen) {
        factory->Release();
        FreeLibrary(dxgi);
        FreeLibrary(d3d11);
        return false;
      }

      constexpr std::int32_t feature_levels[] = {
        0xb100,  // D3D_FEATURE_LEVEL_11_1
        0xb000,  // D3D_FEATURE_LEVEL_11_0
      };
      void *device = nullptr;
      void *ctx = nullptr;
      std::int32_t level = 0;
      HRESULT hr = fn(
        chosen,
        0 /* D3D_DRIVER_TYPE_UNKNOWN */,
        nullptr,
        0,
        feature_levels,
        static_cast<std::uint32_t>(std::size(feature_levels)),
        7 /* D3D11_SDK_VERSION */,
        &device,
        &level,
        &ctx
      );
      chosen->Release();
      factory->Release();
      FreeLibrary(dxgi);
      FreeLibrary(d3d11);
      if (FAILED(hr) || !device) {
        return false;
      }
      if (ctx) {
        // Release the immediate context immediately; we only need the
        // device pointer for AMF's InitDX11 call.
        static_cast<IUnknown *>(ctx)->Release();
      }
      *out_device = static_cast<IUnknown *>(device);
      return true;
    }

    std::string format_amf_version(std::uint64_t v) {
      if (v == 0) {
        return {};
      }
      const auto major = (v >> 48) & 0xffff;
      const auto minor = (v >> 32) & 0xffff;
      const auto patch = (v >> 16) & 0xffff;
      char buf[64] = {};
      std::snprintf(buf, sizeof(buf), "%llu.%llu.%llu",
        static_cast<unsigned long long>(major),
        static_cast<unsigned long long>(minor),
        static_cast<unsigned long long>(patch));
      return buf;
    }

    codec_caps_t probe_codec(
      AMFFactory *factory,
      AMFContext *ctx,
      const wchar_t *encoder_id
    ) {
      codec_caps_t out {};
      AMFComponent *raw_component = nullptr;
      if (factory->vtbl->CreateComponent(factory, ctx, encoder_id, &raw_component) != AMF_OK ||
          !raw_component) {
        return out;
      }
      amf_releaser<AMFComponent> component {raw_component};
      out.supported = true;

      AMFCaps *raw_caps = nullptr;
      if (component->vtbl->GetCaps && component->vtbl->GetCaps(component.ptr, &raw_caps) == AMF_OK &&
          raw_caps) {
        amf_releaser<AMFCaps> caps {raw_caps};
        const auto n = read_int64_prop(caps.ptr, kCaps_MaxNumOfHWInstances);
        if (n > 0) {
          out.max_hw_instances = static_cast<std::uint32_t>(n);
        } else {
          // SDKs that don't expose the property at all are dual-VCN-
          // unaware; treat as 1 so the UI never claims multi-instance
          // on guessed-not-supported hardware.
          out.max_hw_instances = 1;
        }
      } else {
        out.max_hw_instances = 1;
      }

      return out;
    }

    // Cached result for `last_result()`.
    std::mutex g_mutex;
    std::optional<probe_result_t> g_cached;
  }  // namespace

  probe_result_t probe(bool force_refresh) {
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      if (!force_refresh && g_cached) {
        return *g_cached;
      }
    }

    probe_result_t result {};
    result.at = std::chrono::system_clock::now();
    const auto t0 = std::chrono::steady_clock::now();

    auto finalize = [&]() {
      result.probe_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0
      );
      std::lock_guard<std::mutex> lk(g_mutex);
      g_cached = result;
    };

    // 1. Load amfrt64.dll.
    HMODULE amf = LoadLibraryW(L"amfrt64.dll");
    if (!amf) {
      result.error = "amfrt64.dll not found (AMD driver / AMF runtime not installed)";
      BOOST_LOG(info) << "AMF caps probe: " << result.error;
      finalize();
      return result;
    }

    auto pAMFInit = reinterpret_cast<AMFInit_fn>(GetProcAddress(amf, "AMFInit"));
    auto pAMFQueryVersion = reinterpret_cast<AMFQueryVersion_fn>(GetProcAddress(amf, "AMFQueryVersion"));
    if (!pAMFInit) {
      result.error = "AMFInit symbol missing from amfrt64.dll";
      FreeLibrary(amf);
      BOOST_LOG(warning) << "AMF caps probe: " << result.error;
      finalize();
      return result;
    }

    if (pAMFQueryVersion) {
      std::uint64_t v = 0;
      if (pAMFQueryVersion(&v) == AMF_OK) {
        result.runtime_version_raw = v;
        result.runtime_version = format_amf_version(v);
      }
    }

    AMFFactory *factory = nullptr;
    AMF_RESULT hr = pAMFInit(k_amf_full_version, &factory);
    if (hr != AMF_OK || !factory) {
      // Fallback: try the runtime's own advertised version. Some AMF
      // builds reject a higher version than they were compiled with.
      if (result.runtime_version_raw != 0) {
        hr = pAMFInit(result.runtime_version_raw, &factory);
      }
    }
    if (hr != AMF_OK || !factory) {
      char buf[64] = {};
      std::snprintf(buf, sizeof(buf), "AMFInit failed (hr=0x%08x)", static_cast<unsigned>(hr));
      result.error = buf;
      FreeLibrary(amf);
      BOOST_LOG(warning) << "AMF caps probe: " << result.error;
      finalize();
      return result;
    }

    // 2. Build an AMF context bound to a one-shot D3D11 device.
    IUnknown *d3d_device = nullptr;
    if (!create_probe_d3d11(&d3d_device, result.adapter_description)) {
      result.error = "No AMD adapter found (vendor=0x1002)";
      FreeLibrary(amf);
      BOOST_LOG(info) << "AMF caps probe: " << result.error;
      finalize();
      return result;
    }

    AMFContext *raw_ctx = nullptr;
    if (factory->vtbl->CreateContext(factory, &raw_ctx) != AMF_OK || !raw_ctx) {
      result.error = "AMFFactory::CreateContext failed";
      d3d_device->Release();
      FreeLibrary(amf);
      BOOST_LOG(warning) << "AMF caps probe: " << result.error;
      finalize();
      return result;
    }
    amf_releaser<AMFContext> ctx {raw_ctx};

    if (ctx->vtbl->InitDX11(ctx.ptr, d3d_device, AMF_DX11_1) != AMF_OK) {
      // Some older drivers don't accept DX11_1; fall back to DX11_0.
      if (ctx->vtbl->InitDX11(ctx.ptr, d3d_device, AMF_DX11_0) != AMF_OK) {
        result.error = "AMFContext::InitDX11 failed (driver may be too old)";
        d3d_device->Release();
        FreeLibrary(amf);
        BOOST_LOG(warning) << "AMF caps probe: " << result.error;
        finalize();
        return result;
      }
    }

    // We can release the D3D11 device now — AMF retains its own
    // reference internally.
    d3d_device->Release();

    // 3. Probe per-codec caps.
    result.av1 = probe_codec(factory, ctx.ptr, kEncoderId_AV1);
    result.hevc = probe_codec(factory, ctx.ptr, kEncoderId_HEVC);
    result.h264 = probe_codec(factory, ctx.ptr, kEncoderId_AVC);
    result.runtime_available = true;

    // Don't bother unloading amfrt64.dll — keeping it in our address
    // space is fine and avoids repeated load cost if we re-probe.
    (void) amf;

    BOOST_LOG(info) << "AMF caps probe: runtime=" << result.runtime_version
                    << " adapter='" << result.adapter_description << "'"
                    << " av1=" << result.av1.max_hw_instances << "x"
                    << " hevc=" << result.hevc.max_hw_instances << "x"
                    << " h264=" << result.h264.max_hw_instances << "x"
                    << " (" << result.probe_duration.count() << " ms)";

    finalize();
    return result;
  }

  probe_result_t last_result() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_cached) {
      return *g_cached;
    }
    return probe_result_t {};
  }

}  // namespace amf_caps

#else  // !_WIN32

namespace amf_caps {
  probe_result_t probe(bool) {
    probe_result_t r {};
    r.at = std::chrono::system_clock::now();
    r.error = "AMF is Windows-only";
    return r;
  }
  probe_result_t last_result() {
    return probe(false);
  }
}  // namespace amf_caps

#endif  // _WIN32
