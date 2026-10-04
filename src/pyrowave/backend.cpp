#include "backend.h"
#include "probe_protocol.h"
#include "pyrowave_encode.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/utf_utils.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
}

namespace pyrowave {
  namespace {
    std::mutex profile_probe_mutex;
    std::atomic<profile_mask_t> cached_profile_mask {0};
    std::atomic_bool profile_probe_done {false};
    std::atomic_bool profile_probe_cancelled {false};

    // A successful disposable probe exits with this sentinel plus its four-bit
    // profile mask. Ordinary CRT/exception/termination exit codes cannot be
    // mistaken for a capability result.
    constexpr DWORD kProfileProbeExitSentinel = 0x50570000u;  // "PW" + bounded payload
    constexpr DWORD kProfileProbeMask = 0x0Fu;
    constexpr DWORD kProfileProbeCandidateShift = 4u;
    constexpr DWORD kProfileProbeCandidateMask = 0xFF0u;
    constexpr DWORD kProfileProbePayloadMask = kProfileProbeMask | kProfileProbeCandidateMask;
    constexpr DWORD kProfileProbeTimeoutMs = 15'000;
    constexpr DWORD kProfileProbeTerminateCode = 0x5057F001u;

    class scoped_handle_t {
    public:
      scoped_handle_t() = default;
      explicit scoped_handle_t(HANDLE value): value_(value) {}
      scoped_handle_t(const scoped_handle_t &) = delete;
      scoped_handle_t &operator=(const scoped_handle_t &) = delete;
      scoped_handle_t(scoped_handle_t &&other) noexcept: value_(std::exchange(other.value_, nullptr)) {}
      scoped_handle_t &operator=(scoped_handle_t &&other) noexcept {
        if (this != &other) {
          reset();
          value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
      }
      ~scoped_handle_t() { reset(); }

      HANDLE get() const noexcept { return value_; }
      explicit operator bool() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
      void reset(HANDLE value = nullptr) noexcept {
        if (*this) ::CloseHandle(value_);
        value_ = value;
      }

    private:
      HANDLE value_ {nullptr};
    };

    struct probe_selection_t {
      std::string adapter_name;
      std::string output_name;
      std::optional<LUID> adapter_luid_override;
    };

    struct probe_process_result_t {
      profile_mask_t profiles = 0;
      std::uint8_t candidate_count = 0;
    };

    class session_t final: public video::encode_session_t {
    public:
      explicit session_t(std::unique_ptr<pyrowave_enc::pyrowave_encode_device_t> input): device(std::move(input)) {}
      ~session_t() override {
        // A timed-out GPU submission may still reference imported capture memory.
        // Keep that memory alive until the isolated worker exits.
        if (device && !device->safe_to_destroy()) {
          BOOST_LOG(error) << "PyroWave GPU still busy; retaining resources until worker exit";
          (void) device.release();
        }
      }
      int convert(platf::img_t &img) override { return device->convert(img); }
      void request_idr_frame() override { device->request_full_refresh(); }
      void request_normal_frame() override {}
      void invalidate_ref_frames(std::int64_t, std::int64_t) override { request_idr_frame(); }
      void notify_packet_loss() override { device->notify_packet_loss(); }
      void set_fec_video_scale(int percent) override { device->set_fec_video_scale(percent); }
      std::unique_ptr<pyrowave_enc::pyrowave_encode_device_t> device;
    };

    probe_selection_t snapshot_probe_selection() {
      // Config apply mutates these strings in place. Never retain references,
      // and never hold the apply gate while reading the session LUID or doing
      // process work. The parent deliberately performs no DXGI enumeration.
      probe_selection_t result;
      {
        auto apply_gate = config::acquire_apply_read_gate();
        result.adapter_name = config::video.adapter_name;
        result.output_name = config::get_active_output_name();
      }
      result.adapter_luid_override = platf::dxgi::get_dxgi_adapter_luid_override();
      return result;
    }

    bool same_luid(const LUID &left, const LUID &right) noexcept {
      return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
    }

    bool contains_luid(const std::vector<LUID> &values, const LUID &wanted) noexcept {
      for (const auto &value : values) {
        if (same_luid(value, wanted)) return true;
      }
      return false;
    }

    /**
     * Resolve every plausible hardware adapter inside the disposable child.
     *
     * If a configured output currently exists, it is an exact selector and
     * safely narrows the set to its owning adapter. A per-client LuminalVGD
     * output frequently does not exist before session launch; in that case it
     * must not cause us to guess one GPU. We retain every hardware adapter
     * allowed by an explicit LUID/name selector and later advertise only the
     * intersection of their independently probed profiles.
     */
    std::optional<std::vector<LUID>> resolve_probe_adapters(const probe_selection_t &selection) {
      const auto wanted_adapter = utf_utils::from_utf8(selection.adapter_name);
      if (!selection.adapter_name.empty() && wanted_adapter.empty()) return std::nullopt;
      const auto mapped_output = selection.output_name.empty() ? std::string {} :
                                                               display_device::map_output_name(selection.output_name);
      const auto wanted_output = utf_utils::from_utf8(mapped_output);
      if (!mapped_output.empty() && wanted_output.empty()) return std::nullopt;
      platf::dxgi::factory1_t factory;
      const auto factory_status = CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void **>(&factory));
      if (FAILED(factory_status)) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed: DXGI factory creation failed (0x"
                           << std::hex << factory_status << std::dec << ").";
        return std::nullopt;
      }

      std::vector<LUID> plausible_adapters;
      std::vector<LUID> exact_output_adapters;
      for (UINT adapter_index = 0;; ++adapter_index) {
        platf::dxgi::adapter_t adapter;
        const HRESULT adapter_status = factory->EnumAdapters1(adapter_index, &adapter);
        if (adapter_status == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapter_status)) {
          BOOST_LOG(warning) << "PyroWave profile probe failed closed: DXGI adapter enumeration failed at index "
                             << adapter_index << " (0x" << std::hex << adapter_status << std::dec << ").";
          return std::nullopt;
        }
        DXGI_ADAPTER_DESC1 adapter_desc {};
        const HRESULT desc_status = adapter->GetDesc1(&adapter_desc);
        if (FAILED(desc_status)) {
          BOOST_LOG(warning) << "PyroWave profile probe failed closed: DXGI adapter description failed at index "
                             << adapter_index << " (0x" << std::hex << desc_status << std::dec << ").";
          return std::nullopt;
        }

        // Software adapters cannot own the hardware capture/encode path and
        // including WARP would force the conservative intersection to zero on
        // every otherwise-capable host.
        if ((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

        const bool override_matches = !selection.adapter_luid_override ||
                                      same_luid(adapter_desc.AdapterLuid, *selection.adapter_luid_override);
        if (!override_matches || (!wanted_adapter.empty() && adapter_desc.Description != wanted_adapter)) {
          continue;
        }

        if (!contains_luid(plausible_adapters, adapter_desc.AdapterLuid)) {
          plausible_adapters.push_back(adapter_desc.AdapterLuid);
        }

        for (UINT output_index = 0;; ++output_index) {
          platf::dxgi::output_t output;
          const HRESULT output_status = adapter->EnumOutputs(output_index, &output);
          if (output_status == DXGI_ERROR_NOT_FOUND) break;
          if (FAILED(output_status)) {
            BOOST_LOG(warning) << "PyroWave profile probe failed closed: DXGI output enumeration failed for adapter "
                               << adapter_index << " at index " << output_index << " (0x"
                               << std::hex << output_status << std::dec << ").";
            return std::nullopt;
          }
          DXGI_OUTPUT_DESC output_desc {};
          const HRESULT output_desc_status = output->GetDesc(&output_desc);
          if (FAILED(output_desc_status)) {
            BOOST_LOG(warning) << "PyroWave profile probe failed closed: DXGI output description failed for adapter "
                               << adapter_index << " at index " << output_index << " (0x"
                               << std::hex << output_desc_status << std::dec << ").";
            return std::nullopt;
          }
          if (!wanted_output.empty() && output_desc.DeviceName == wanted_output) {
            if (!contains_luid(exact_output_adapters, adapter_desc.AdapterLuid)) {
              exact_output_adapters.push_back(adapter_desc.AdapterLuid);
            }
          }
        }
      }

      if (!exact_output_adapters.empty()) return exact_output_adapters;
      if (!plausible_adapters.empty()) return plausible_adapters;
      BOOST_LOG(warning) << "PyroWave profile probe failed closed: no hardware DXGI adapter matched"
                         << " adapter=" << logging::bracket(selection.adapter_name)
                         << " output=" << logging::bracket(mapped_output) << '.';
      return std::nullopt;
    }

    std::optional<profile_mask_t> probe_profiles(const LUID &adapter_luid) {
      // One Vulkan context is sufficient to prove all four combinations on the
      // selected adapter. Each profile is independent: an HDR or 4:4:4 failure
      // must not hide a working SDR 4:2:0 path (or advertise the failed one).
      auto context = pyrowave_vk::context::create(
        reinterpret_cast<const std::uint8_t *>(&adapter_luid));
      if (!context) return std::nullopt;
      std::shared_ptr<pyrowave_vk::context> shared(std::move(context));
      profile_mask_t result = 0;
      for (const bool hdr : {false, true}) {
        for (const bool chroma444 : {false, true}) {
          video::config_t config {};
          config.encoderCscMode = 2;
          config.dynamicRange = hdr ? 1 : 0;
          config.chromaSamplingType = chroma444 ? 1 : 0;
          const auto colors = video::colorspace_from_client_config(config, hdr);
          auto device = pyrowave_enc::pyrowave_encode_device_t::create(
            shared,
            128,
            128,
            8'000'000,
            60,
            colors,
            0,
            0,
            0,
            chroma444,
            false
          );
          if (device) result |= profile_bit(profile(hdr, chroma444));
        }
      }
      return result;
    }

    std::optional<profile_mask_t> probe_profiles_intersection(const std::vector<LUID> &adapters) {
      if (adapters.empty()) return std::nullopt;
      profile_mask_t intersection = kProfileProbeMask;
      for (const auto &adapter : adapters) {
        const auto supported = probe_profiles(adapter);
        // An unprobeable plausible adapter is not evidence of capability.
        // Treat it as supporting no profiles rather than advertising features
        // that may fail when LuminalVGD later materializes on that adapter.
        intersection &= supported.value_or(profile_mask_t {0});
        if (intersection == 0) break;
      }
      return intersection;
    }

    std::wstring executable_path() {
      std::wstring path(32768, L'\0');
      const DWORD size = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
      if (!size || size >= path.size()) return {};
      path.resize(size);
      return path;
    }

    void terminate_probe_process(HANDLE job, HANDLE process) noexcept {
      bool terminated = false;
      if (job) {
        terminated = ::TerminateJobObject(job, kProfileProbeTerminateCode) != FALSE;
      }
      if (!terminated && process) {
        (void) ::TerminateProcess(process, kProfileProbeTerminateCode);
      }
      if (process) (void) ::WaitForSingleObject(process, 2'000);
    }

    std::optional<probe_process_result_t> run_probe_process(const probe_selection_t &selection) {
      const auto executable = executable_path();
      if (executable.empty()) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed: executable path is unavailable (err="
                           << ::GetLastError() << ").";
        return std::nullopt;
      }

      const auto encoded_adapter = probe_protocol::encode_selection(selection.adapter_name);
      const auto encoded_output = probe_protocol::encode_selection(selection.output_name);
      probe_protocol::optional_luid_t wire_luid;
      if (selection.adapter_luid_override) {
        wire_luid.present = true;
        wire_luid.high = static_cast<std::uint32_t>(selection.adapter_luid_override->HighPart);
        wire_luid.low = selection.adapter_luid_override->LowPart;
      }
      if (!encoded_adapter || !encoded_output) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed: adapter/output selection is too long or contains NUL.";
        return std::nullopt;
      }
      const auto encoded_luid = probe_protocol::encode_optional_luid(wire_luid);

      // The child receives only canonical ASCII hexadecimal tokens. Passing
      // lpApplicationName separately makes the quoted path non-authoritative
      // for executable selection; no config-controlled text is interpreted as
      // quoting, switches, or an executable path.
      std::wstring command = L"\"" + executable + L"\" --internal-pyrowave-probe " +
                             std::wstring(encoded_adapter->begin(), encoded_adapter->end()) + L" " +
                             std::wstring(encoded_output->begin(), encoded_output->end()) + L" " +
                             std::wstring(encoded_luid.begin(), encoded_luid.end());
      STARTUPINFOW startup {.cb = sizeof(STARTUPINFOW)};
      PROCESS_INFORMATION process_info {};
      if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process_info)) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed: CreateProcess failed (err="
                           << ::GetLastError() << ").";
        return std::nullopt;
      }
      scoped_handle_t process {process_info.hProcess};
      scoped_handle_t thread {process_info.hThread};

      scoped_handle_t job {::CreateJobObjectW(nullptr, nullptr)};
      if (!job) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed: CreateJobObject failed (err="
                           << ::GetLastError() << ").";
        terminate_probe_process(nullptr, process.get());
        return std::nullopt;
      }
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed: job limit configuration failed (err="
                           << ::GetLastError() << ").";
        terminate_probe_process(nullptr, process.get());
        return std::nullopt;
      }
      if (!::AssignProcessToJobObject(job.get(), process.get())) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed: job assignment failed (err="
                           << ::GetLastError() << ").";
        terminate_probe_process(nullptr, process.get());
        return std::nullopt;
      }
      if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed: ResumeThread failed (err="
                           << ::GetLastError() << ").";
        terminate_probe_process(job.get(), process.get());
        return std::nullopt;
      }
      thread.reset();

      DWORD wait_status = WAIT_TIMEOUT;
      DWORD waited_ms = 0;
      while (waited_ms < kProfileProbeTimeoutMs) {
        if (profile_probe_cancelled.load(std::memory_order_acquire)) {
          BOOST_LOG(info) << "PyroWave profile probe cancelled during shutdown.";
          terminate_probe_process(job.get(), process.get());
          return std::nullopt;
        }
        constexpr DWORD kWaitSliceMs = 100;
        const DWORD wait_slice = (kProfileProbeTimeoutMs - waited_ms < kWaitSliceMs) ?
                                   kProfileProbeTimeoutMs - waited_ms : kWaitSliceMs;
        wait_status = ::WaitForSingleObject(process.get(), wait_slice);
        if (wait_status != WAIT_TIMEOUT) break;
        waited_ms += wait_slice;
      }
      if (wait_status != WAIT_OBJECT_0) {
        if (wait_status == WAIT_TIMEOUT) {
          BOOST_LOG(warning) << "PyroWave profile probe exceeded " << kProfileProbeTimeoutMs
                             << " ms; terminating the disposable child.";
        } else {
          BOOST_LOG(warning) << "PyroWave profile probe wait failed (status=0x" << std::hex
                             << wait_status << ", err=" << ::GetLastError() << std::dec << ").";
        }
        terminate_probe_process(job.get(), process.get());
        return std::nullopt;
      }

      DWORD exit_code = 0;
      if (!::GetExitCodeProcess(process.get(), &exit_code) ||
          (exit_code & ~kProfileProbePayloadMask) != kProfileProbeExitSentinel) {
        BOOST_LOG(warning) << "PyroWave profile probe child did not return a valid capability sentinel"
                           << " (exit=0x" << std::hex << exit_code << std::dec << ").";
        return std::nullopt;
      }
      const auto candidate_count = static_cast<std::uint8_t>(
        (exit_code & kProfileProbeCandidateMask) >> kProfileProbeCandidateShift);
      if (candidate_count == 0) {
        BOOST_LOG(warning) << "PyroWave profile probe child returned an empty adapter set; failing closed.";
        return std::nullopt;
      }
      return probe_process_result_t {
        .profiles = static_cast<profile_mask_t>(exit_code & kProfileProbeMask),
        .candidate_count = candidate_count,
      };
    }
  }

  int run_profile_probe_child(
    std::string_view encoded_adapter,
    std::string_view encoded_output,
    std::string_view encoded_luid
  ) noexcept {
    try {
      const auto adapter_name = probe_protocol::decode_selection(encoded_adapter);
      const auto output_name = probe_protocol::decode_selection(encoded_output);
      const auto wire_luid = probe_protocol::decode_optional_luid(encoded_luid);
      if (!adapter_name || !output_name || !wire_luid) return 9;

      probe_selection_t selection {
        .adapter_name = *adapter_name,
        .output_name = *output_name,
      };
      if (wire_luid->present) {
        LUID luid {};
        luid.HighPart = static_cast<LONG>(wire_luid->high);
        luid.LowPart = wire_luid->low;
        selection.adapter_luid_override = luid;
      }

      const auto adapters = resolve_probe_adapters(selection);
      if (!adapters) return 10;
      const auto supported = probe_profiles_intersection(*adapters);
      if (!supported) return 10;
      const auto candidate_count = static_cast<DWORD>(std::min<std::size_t>(adapters->size(), 0xFFu));
      return static_cast<int>(kProfileProbeExitSentinel |
                              (candidate_count << kProfileProbeCandidateShift) |
                              (*supported & kProfileProbeMask));
    } catch (...) {
      return 10;
    }
  }

  void initialize_profile_probe() {
    if (profile_probe_done.load(std::memory_order_acquire) ||
        profile_probe_cancelled.load(std::memory_order_acquire)) return;
    std::lock_guard lock(profile_probe_mutex);
    if (profile_probe_done.load(std::memory_order_relaxed) ||
        profile_probe_cancelled.load(std::memory_order_acquire)) return;
    try {
      const auto selection = snapshot_probe_selection();
      const auto supported = run_probe_process(selection);
      if (!supported) {
        BOOST_LOG(warning) << "PyroWave profile probe failed closed; publishing intersection mask 0x0 until restart.";
        return;
      }
      cached_profile_mask.store(supported->profiles, std::memory_order_release);
      // A valid child sentinel with mask 0 is a completed, unsupported probe.
      profile_probe_done.store(true, std::memory_order_release);
      BOOST_LOG(info) << "PyroWave profile probe completed across "
                      << static_cast<unsigned>(supported->candidate_count)
                      << " plausible hardware adapter(s); publishing intersection mask 0x"
                      << std::hex << static_cast<unsigned>(supported->profiles) << std::dec << '.';
    } catch (const std::exception &error) {
      BOOST_LOG(warning) << "PyroWave profile probe failed closed after exception: " << error.what();
    } catch (...) {
      BOOST_LOG(warning) << "PyroWave profile probe failed closed after an unknown exception.";
    }
  }

  void cancel_profile_probe() noexcept {
    profile_probe_cancelled.store(true, std::memory_order_release);
  }

  bool profile_probe_complete() {
    return profile_probe_done.load(std::memory_order_acquire);
  }

  profile_mask_t profile_mask() {
    // Snapshot-only by design. HTTP, RTSP, configuration metadata, and worker
    // admission call this on latency-sensitive threads and must never create a
    // Vulkan device. main() performs one bounded, fail-closed startup attempt.
    return cached_profile_mask.load(std::memory_order_acquire);
  }

  bool supports_profile(bool hdr, bool chroma444) {
    return profile_mask_supports(profile_mask(), hdr, chroma444);
  }

  std::uint32_t server_codec_mask(bool allow_yuv444) {
    return server_codec_mask_for_profiles(profile_mask(), allow_yuv444);
  }

  bool available() {
    // SDR 4:2:0 is the baseline format-3 interoperability contract. A machine
    // that can create only an optional profile is not generally available.
    return supports_profile(false, false);
  }

  video::encoder_t &encoder() {
    static video::encoder_t result = [] {
      auto formats = std::make_unique<video::encoder_platform_formats_t>();
      formats->dev_type = platf::mem_type_e::dxgi;
      formats->pix_fmt_8bit = platf::pix_fmt_e::nv12;
      formats->pix_fmt_10bit = platf::pix_fmt_e::p010;
      // No legacy codec capabilities: this object is only dispatched explicitly.
      return video::encoder_t {"pyrowave", std::move(formats), {}, {}, {}, 0};
    }();
    return result;
  }

  std::unique_ptr<platf::encode_device_t> make_device(platf::display_t &display, const video::config_t &config) {
    auto *d3d = dynamic_cast<platf::dxgi::display_base_t *>(&display);
    const bool chroma444 = config.chromaSamplingType != 0;
    // The actual selected-adapter/LUID create below is authoritative. Do not
    // invoke the cached discovery probe here: this function runs in the worker
    // child's bounded startup path, and probing four devices there can consume
    // its entire readiness deadline (and may target a different adapter).
    if (!d3d || !d3d->adapter ||
        config.pyrowave_quality_bias < 0 || config.pyrowave_quality_bias > 3 ||
        config.pyrowave_refresh_interval < 0 || config.pyrowave_refresh_interval > 255 ||
        (config.pyrowave_adaptive_fec != 0 && config.pyrowave_adaptive_fec != 1) ||
        (config.pyrowave_adaptive_bitrate != 0 && config.pyrowave_adaptive_bitrate != 1) ||
        !valid_session(config.width, config.height, config.framerate, config.bitrate, config.pyrowave_packet_size)) return {};
    // Never label an SDR source as HDR. Existing setup must first enable HDR.
    if (config.dynamicRange && !display.is_hdr()) return {};
    DXGI_ADAPTER_DESC1 adapter {};
    if (FAILED(d3d->adapter->GetDesc1(&adapter))) return {};
    auto context = pyrowave_vk::context::create(reinterpret_cast<const uint8_t *>(&adapter.AdapterLuid));
    if (!context) return {};
    auto colors = video::colorspace_from_client_config(config, display.is_hdr());
    auto device = pyrowave_enc::pyrowave_encode_device_t::create(
      std::move(context), config.width, config.height, std::int64_t(config.bitrate) * 1000,
      config.framerate, colors, config.pyrowave_packet_size - sizeof(NV_VIDEO_PACKET),
      config.pyrowave_quality_bias, config.pyrowave_refresh_interval, chroma444,
      config.pyrowave_adaptive_bitrate != 0);
    if (device) {
      device->colorspace = colors;
      device->max_output_bytes = max_frame_bytes(config.pyrowave_packet_size);
    }
    return device;
  }

  std::unique_ptr<video::encode_session_t> make_session(std::unique_ptr<platf::encode_device_t> device) {
    auto *typed = dynamic_cast<pyrowave_enc::pyrowave_encode_device_t *>(device.get());
    if (!typed) return {};
    device.release();
    return std::make_unique<session_t>(std::unique_ptr<pyrowave_enc::pyrowave_encode_device_t>(typed));
  }

  video::packet_t encode(video::encode_session_t &session, std::int64_t frame) {
    auto *typed = dynamic_cast<session_t *>(&session);
    if (!typed) return {};
    auto result = typed->device->encode_frame(frame);
    if (result.data.empty()) return {};
    auto packet = std::make_unique<video::packet_raw_generic>(std::move(result.data), frame, result.idr);
    packet->pacing_bitrate_kbps = result.pacing_bitrate_kbps;
    return packet;
  }
}
