/**
 * @file src/session_monitor_client.cpp
 * @brief Implementation of the producer client for the session
 *        monitor service.
 *
 * Threading: one background sender thread owns the named-pipe
 * handle. All public APIs enqueue serialised frames on a bounded
 * lock-protected queue and the sender drains them. The queue is
 * bounded at a few hundred frames so a misbehaving monitor that
 * stalls reads (or a non-existent monitor that we can never connect
 * to) cannot grow memory without bound — old frames are dropped at
 * the head when the queue overflows.
 *
 * The host-perf-counter sampler thread runs separately. It uses
 * GetSystemTimes to derive CPU utilisation, GlobalMemoryStatusEx for
 * RAM, and PDH's "GPU Engine" / "GPU Adapter Memory" counter sets
 * (the same source Task Manager reads) for GPU / encoder / VRAM
 * utilisation. Total dedicated-VRAM capacity — the denominator for
 * host_vram_pct — comes from DXGI adapter descriptors, captured once
 * at sampler start.
 */
#include "session_monitor_client.h"

#include "logging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>

  #include <dxgi.h>
  #include <pdh.h>
  #include <pdhmsg.h>
#endif

namespace session_mon {

  namespace {

#ifdef _WIN32
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\LuminalShineSessionMonIngest";
#endif

    /// Bounded outbound queue. ~256 frames is plenty for the steady
    /// state (one session = ~10 frames/s; even 8 concurrent sessions
    /// at 10Hz fits comfortably). Drop-oldest on overflow so the
    /// most recent metrics survive when the monitor is down.
    constexpr std::size_t kQueueMaxFrames = 256;

    struct State {
      std::mutex              mtx;
      std::condition_variable cv;
      std::deque<std::string> queue;  // serialised JSON frames
      bool                    stop = false;
      bool                    inited = false;
      std::thread             sender;
      std::thread             host_sampler;

      // Set of active session IDs the host sampler pushes to.
      // Guarded by mtx (same lock as queue) because mutations are
      // infrequent (session start/end) and reads are 1Hz.
      std::vector<std::string> active_sessions;
    };

    State &state() {
      static State s;
      return s;
    }

    void enqueue(std::string frame) {
      auto &s = state();
      {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.inited || s.stop) {
          return;
        }
        if (s.queue.size() >= kQueueMaxFrames) {
          s.queue.pop_front();
        }
        s.queue.push_back(std::move(frame));
      }
      s.cv.notify_one();
    }

#ifdef _WIN32

    /// Connect to the named pipe; returns INVALID_HANDLE_VALUE
    /// on any failure. Brief WaitNamedPipe so a transiently-busy
    /// pipe doesn't immediately give up.
    HANDLE try_connect() {
      // First attempt: open the pipe directly.
      HANDLE h = CreateFileW(
        kPipeName,
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
      );
      if (h != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
        return h;
      }
      // If all pipe instances are busy, wait briefly and retry once.
      if (GetLastError() == ERROR_PIPE_BUSY) {
        if (WaitNamedPipeW(kPipeName, 200)) {
          h = CreateFileW(
            kPipeName,
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
          );
          if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            return h;
          }
        }
      }
      return INVALID_HANDLE_VALUE;
    }

    bool write_frame(HANDLE pipe, const std::string &payload) {
      const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
      DWORD written = 0;
      if (!WriteFile(pipe, &len, sizeof(len), &written, nullptr) || written != sizeof(len)) {
        return false;
      }
      DWORD total = 0;
      while (total < len) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, payload.data() + total, len - total, &chunk, nullptr) || chunk == 0) {
          return false;
        }
        total += chunk;
      }
      return true;
    }

    void sender_loop() {
      auto &s = state();
      HANDLE pipe = INVALID_HANDLE_VALUE;
      using namespace std::chrono_literals;
      auto next_connect_attempt = std::chrono::steady_clock::now();

      for (;;) {
        std::string frame;
        {
          std::unique_lock<std::mutex> lk(s.mtx);
          s.cv.wait(lk, [&] { return s.stop || !s.queue.empty(); });
          if (s.stop && s.queue.empty()) {
            break;
          }
          frame = std::move(s.queue.front());
          s.queue.pop_front();
        }

        if (pipe == INVALID_HANDLE_VALUE) {
          // Backoff: don't hammer the pipe with reconnects when the
          // service is down. 1-second steady cadence is fast enough
          // that telemetry resumes within a tick of the service
          // coming back up, slow enough that a stopped monitor
          // doesn't burn CPU.
          if (std::chrono::steady_clock::now() < next_connect_attempt) {
            // Drop the frame — we can't deliver it and rebuffering
            // would just delay shedding load.
            continue;
          }
          pipe = try_connect();
          if (pipe == INVALID_HANDLE_VALUE) {
            next_connect_attempt = std::chrono::steady_clock::now() + 1s;
            continue;
          }
        }
        if (!write_frame(pipe, frame)) {
          CloseHandle(pipe);
          pipe = INVALID_HANDLE_VALUE;
          next_connect_attempt = std::chrono::steady_clock::now() + 1s;
          // Frame is gone. Caller does not expect at-least-once
          // delivery — telemetry is best-effort and a single dropped
          // frame is visually invisible at 1Hz sampling.
        }
      }
      if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
      }
    }

    /// GPU / encoder / VRAM utilisation via PDH. The "GPU Engine"
    /// counter set exposes one instance per (process, engine); we
    /// sum utilisation per engine type and report the busiest type
    /// as host_gpu_pct (matching Task Manager's headline GPU number)
    /// and the VideoEncode type as host_gpu_encoder_pct. "GPU
    /// Adapter Memory / Dedicated Usage" summed across adapters is
    /// normalised by the DXGI dedicated-VRAM capacity total.
    ///
    /// PDH utilisation counters need two collections before yielding
    /// data, so the first tick after init contributes no GPU keys —
    /// invisible at 1Hz sampling.
    class gpu_sampler_t {
    public:
      bool init() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
          query = nullptr;
          return false;
        }
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine_counter) != ERROR_SUCCESS) {
          engine_counter = nullptr;
        }
        if (PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vram_counter) != ERROR_SUCCESS) {
          vram_counter = nullptr;
        }
        if (!engine_counter && !vram_counter) {
          close();
          return false;
        }
        total_vram_bytes = query_total_dedicated_vram();
        // Prime the query: rate counters return data from the second
        // collection onwards.
        PdhCollectQueryData(query);
        return true;
      }

      void close() {
        if (query) {
          PdhCloseQuery(query);
          query = nullptr;
          engine_counter = nullptr;
          vram_counter = nullptr;
        }
      }

      /// Collect one round and merge whatever is available into the
      /// sample. Missing counters simply leave their keys absent.
      void collect(Sample &sample) {
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
          return;
        }
        if (engine_counter) {
          // Sum utilisation per engine type across all process
          // instances (names end in "...engtype_<Type>").
          std::map<std::wstring, double> by_type;
          const bool ok = for_each_instance(engine_counter, [&](const wchar_t *name, double value) {
            std::wstring_view n {name ? name : L""};
            const auto pos = n.find(L"engtype_");
            if (pos == std::wstring_view::npos) {
              return;
            }
            by_type[std::wstring {n.substr(pos + 8)}] += value;
          });
          if (ok && !by_type.empty()) {
            double busiest = 0.0;
            double encoder = 0.0;
            for (const auto &[type, sum] : by_type) {
              busiest = std::max(busiest, sum);
              if (type.find(L"VideoEncode") != std::wstring::npos) {
                encoder += sum;
              }
            }
            sample["host_gpu_pct"] = std::clamp(busiest, 0.0, 100.0);
            sample["host_gpu_encoder_pct"] = std::clamp(encoder, 0.0, 100.0);
          }
        }
        if (vram_counter && total_vram_bytes > 0) {
          double used_bytes = 0.0;
          const bool ok = for_each_instance(vram_counter, [&](const wchar_t *, double value) {
            used_bytes += value;
          });
          if (ok) {
            const double pct = 100.0 * used_bytes / static_cast<double>(total_vram_bytes);
            sample["host_vram_pct"] = std::clamp(pct, 0.0, 100.0);
          }
        }
      }

    private:
      template<typename F>
      bool for_each_instance(PDH_HCOUNTER counter, F &&fn) {
        DWORD bytes = 0;
        DWORD count = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
        if (st != PDH_MORE_DATA || bytes == 0) {
          return false;
        }
        buf.resize(bytes);
        st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data()));
        if (st != ERROR_SUCCESS) {
          return false;
        }
        auto *items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
          const auto status = items[i].FmtValue.CStatus;
          if (status != PDH_CSTATUS_VALID_DATA && status != PDH_CSTATUS_NEW_DATA) {
            continue;
          }
          fn(items[i].szName, items[i].FmtValue.doubleValue);
        }
        return true;
      }

      /// Total dedicated VRAM across all non-software adapters —
      /// mirrors the summed "Dedicated Usage" instances above so the
      /// percentage stays consistent on multi-GPU hosts.
      static std::uint64_t query_total_dedicated_vram() {
        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory) {
          return 0;
        }
        std::uint64_t total = 0;
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
          DXGI_ADAPTER_DESC1 desc {};
          if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                                  desc.VendorId == 0x1414;  // Microsoft Basic Render
            if (!software) {
              total += desc.DedicatedVideoMemory;
            }
          }
          adapter->Release();
          adapter = nullptr;
        }
        factory->Release();
        return total;
      }

      std::vector<std::uint8_t> buf;
      PDH_HQUERY query = nullptr;
      PDH_HCOUNTER engine_counter = nullptr;
      PDH_HCOUNTER vram_counter = nullptr;
      std::uint64_t total_vram_bytes = 0;
    };

    /// 1Hz host-perf-counter sampler. Computes CPU utilisation as
    /// the ratio of (kernel+user) time delta to total elapsed time
    /// between consecutive GetSystemTimes calls; RAM utilisation
    /// from GlobalMemoryStatusEx; GPU / encoder / VRAM utilisation
    /// via the PDH-backed gpu_sampler_t. Pushes to every
    /// currently-active session, so multiple concurrent streams
    /// (rare on Windows hosts but possible with WebRTC) all see the
    /// same host graphs.
    void host_sampler_loop() {
      auto &s = state();
      using namespace std::chrono_literals;

      auto sub_ft = [](const FILETIME &a, const FILETIME &b) -> std::uint64_t {
        ULARGE_INTEGER ua, ub;
        ua.LowPart = a.dwLowDateTime;
        ua.HighPart = a.dwHighDateTime;
        ub.LowPart = b.dwLowDateTime;
        ub.HighPart = b.dwHighDateTime;
        return ua.QuadPart - ub.QuadPart;
      };

      FILETIME prev_idle {}, prev_kernel {}, prev_user {};
      bool have_prev = false;

      gpu_sampler_t gpu;
      const bool gpu_ok = gpu.init();
      if (!gpu_ok) {
        BOOST_LOG(info) << "session_mon: GPU perf counters unavailable; host GPU/VRAM graphs will be empty";
      }

      auto gpu_cleanup = std::unique_ptr<gpu_sampler_t, void (*)(gpu_sampler_t *)>(
        gpu_ok ? &gpu : nullptr,
        [](gpu_sampler_t *g) {
          g->close();
        }
      );

      for (;;) {
        {
          std::unique_lock<std::mutex> lk(s.mtx);
          if (s.cv.wait_for(lk, 1s, [&] { return s.stop; })) {
            return;
          }
        }
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) {
          continue;
        }
        if (!have_prev) {
          prev_idle = idle;
          prev_kernel = kernel;
          prev_user = user;
          have_prev = true;
          continue;
        }
        const auto idle_delta   = sub_ft(idle, prev_idle);
        const auto kernel_delta = sub_ft(kernel, prev_kernel);
        const auto user_delta   = sub_ft(user, prev_user);
        prev_idle = idle;
        prev_kernel = kernel;
        prev_user = user;

        const auto total = kernel_delta + user_delta;
        double cpu_pct = 0.0;
        if (total > 0) {
          cpu_pct = 100.0 * static_cast<double>(total - idle_delta) / static_cast<double>(total);
          if (cpu_pct < 0.0) cpu_pct = 0.0;
          if (cpu_pct > 100.0) cpu_pct = 100.0;
        }

        MEMORYSTATUSEX mem {};
        mem.dwLength = sizeof(mem);
        double ram_pct = 0.0;
        if (GlobalMemoryStatusEx(&mem)) {
          ram_pct = static_cast<double>(mem.dwMemoryLoad);
        }

        std::vector<std::string> active_snapshot;
        {
          std::lock_guard<std::mutex> lk(s.mtx);
          active_snapshot = s.active_sessions;
        }
        if (active_snapshot.empty()) {
          continue;
        }
        Sample sample;
        sample["host_cpu_pct"] = cpu_pct;
        sample["host_ram_pct"] = ram_pct;
        if (gpu_ok) {
          gpu.collect(sample);
        }
        const auto ts =
          std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
          ).count();
        for (const auto &id : active_snapshot) {
          session_mon::sample(id, ts, sample);
        }
      }
    }

#else  // non-Windows stubs — LuminalShine is Windows-only by design
    void sender_loop() {}
    void host_sampler_loop() {}
#endif

  }  // namespace

  void init() {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.inited) {
      return;
    }
    s.inited = true;
    s.stop = false;
#ifdef _WIN32
    s.sender = std::thread(sender_loop);
    s.host_sampler = std::thread(host_sampler_loop);
#endif
    BOOST_LOG(info) << "session_mon: producer client initialised";
  }

  void shutdown() {
    auto &s = state();
    {
      std::lock_guard<std::mutex> lk(s.mtx);
      if (!s.inited) {
        return;
      }
      s.stop = true;
    }
    s.cv.notify_all();
    if (s.sender.joinable()) s.sender.join();
    if (s.host_sampler.joinable()) s.host_sampler.join();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.inited = false;
    s.queue.clear();
  }

  std::string make_id(std::uint32_t launch_session_id) {
    // Stable per-process prefix: the unix-epoch milliseconds at the
    // first call into this function. Captured once via the static
    // local-init guarantee. Combined with the launch_session_id (a
    // per-process counter starting at 1), this gives every session
    // a string that is unique across luminalshine.exe restarts.
    //
    // Why this exists: launch_session_id alone resets to 1 on every
    // restart, so the second run's first session would collide with
    // the persisted "1.json" the monitor rehydrates at startup. The
    // collision presented in the UI as "duplicate of the original
    // session, no new sessions ever appearing" because each new
    // stream silently overwrote the persisted record under the same
    // id. The epoch prefix is opaque to the rest of the code — the
    // service treats id as a free-form string key, the Web UI passes
    // it through encodeURIComponent, and disk filenames are <id>.json
    // which copes fine with the embedded hyphen.
    //
    // Legacy persisted sessions written with the pure-integer ID
    // remain readable: their map key is "1" / "2" / ..., the new
    // format's key always contains a hyphen, so the two namespaces
    // can never collide as map keys.
    static const std::string prefix = []() {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                      ).count();
      return std::to_string(ms);
    }();
    return prefix + "-" + std::to_string(launch_session_id);
  }

  void session_started(const std::string &id, const SessionMetadata &md) {
    nlohmann::json frame;
    frame["type"] = "session_started";
    frame["id"]   = id;
    frame["started_at"] =
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
    nlohmann::json m;
    m["client_name"]            = md.client_name;
    m["device"]                 = md.device;
    m["protocol"]               = md.protocol;
    m["codec"]                  = md.codec;
    m["resolution_w"]           = md.width;
    m["resolution_h"]           = md.height;
    m["fps"]                    = md.fps;
    m["bitrate_mbps_target"]    = md.bitrate_mbps_target;
    m["hdr"]                    = md.hdr;
    m["yuv444"]                 = md.yuv444;
    m["audio_channels"]         = md.audio_channels;
    m["application"]            = md.application;
    m["cpu_model"]              = md.cpu_model;
    m["gpu_model"]              = md.gpu_model;
    m["luminalshine_version"]   = md.luminalshine_version;
    frame["metadata"] = std::move(m);
    enqueue(frame.dump());

    {
      auto &s = state();
      std::lock_guard<std::mutex> lk(s.mtx);
      s.active_sessions.push_back(id);
    }
  }

  void metadata_update(const std::string &id, const std::map<std::string, std::string> &patch) {
    if (patch.empty()) {
      return;
    }
    nlohmann::json frame;
    frame["type"] = "metadata_update";
    frame["id"]   = id;
    nlohmann::json m;
    for (const auto &[k, v] : patch) {
      m[k] = v;
    }
    frame["metadata"] = std::move(m);
    enqueue(frame.dump());
  }

  void sample(const std::string &id, std::int64_t ts_epoch_s, const Sample &s) {
    if (s.empty()) {
      return;
    }
    nlohmann::json frame;
    frame["type"] = "sample";
    frame["id"]   = id;
    frame["ts"]   = ts_epoch_s;
    nlohmann::json series;
    for (const auto &[k, v] : s) {
      series[k] = v;
    }
    frame["series"] = std::move(series);
    enqueue(frame.dump());
  }

  void sample_now(const std::string &id, const Sample &s) {
    const auto ts =
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
    sample(id, ts, s);
  }

  void session_ended(const std::string &id) {
    nlohmann::json frame;
    frame["type"] = "session_ended";
    frame["id"]   = id;
    frame["ended_at"] =
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
    enqueue(frame.dump());

    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    auto it = std::find(s.active_sessions.begin(), s.active_sessions.end(), id);
    if (it != s.active_sessions.end()) {
      s.active_sessions.erase(it);
    }
  }

}  // namespace session_mon
