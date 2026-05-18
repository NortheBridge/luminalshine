/**
 * @file src/platform/windows/render_stack_detect.cpp
 */
#include "render_stack_detect.h"

#include "src/logging.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
// clang-format on

namespace platf::render_stack {

  namespace {

    constexpr std::array<const wchar_t *, 3> kAiModuleNames = {
      L"nvngx_dlss.dll",
      L"nvngx_dlssg.dll",
      L"nvngx_dlaa.dll",
    };

    std::string to_utf8(const std::wstring &w) {
      if (w.empty()) {
        return {};
      }
      const int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (needed <= 0) {
        return {};
      }
      std::string s(static_cast<size_t>(needed - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), needed, nullptr, nullptr);
      return s;
    }

    std::wstring to_lower(std::wstring s) {
      for (auto &c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
      }
      return s;
    }

    // Pull the list of loaded modules out of a process via psapi.
    // Returns true when at least one EnumProcessModulesEx call
    // succeeded. The caller filters the result for the AI modules.
    bool enumerate_modules(HANDLE proc, std::vector<HMODULE> &out) {
      DWORD needed = 0;
      out.assign(512, nullptr);
      while (true) {
        const DWORD bytes = static_cast<DWORD>(out.size() * sizeof(HMODULE));
        if (!EnumProcessModulesEx(proc, out.data(), bytes, &needed, LIST_MODULES_ALL)) {
          // Common: ERROR_PARTIAL_COPY (299) on 32-bit processes from
          // 64-bit caller, or just access denied for SYSTEM processes
          // when we run as a user-context tool. Either way, give up
          // quietly — we will check the other PIDs.
          return false;
        }
        if (needed <= bytes) {
          out.resize(needed / sizeof(HMODULE));
          return true;
        }
        // Buffer too small — grow and retry.
        out.assign(out.size() * 2, nullptr);
        if (out.size() > 32 * 1024) {
          return false;
        }
      }
    }

    // Best-effort image path → just the filename component.
    std::wstring process_image_basename(HANDLE proc) {
      wchar_t buf[MAX_PATH] = {};
      DWORD len = GetProcessImageFileNameW(proc, buf, _countof(buf));
      if (len == 0) {
        return {};
      }
      std::wstring path(buf, len);
      const auto slash = path.find_last_of(L"\\/");
      if (slash != std::wstring::npos) {
        return path.substr(slash + 1);
      }
      return path;
    }

    // Returns the matching AI module names (lowercased filenames) for
    // a single PID. Empty if the process has none or we can't see it.
    std::vector<std::wstring> ai_modules_for_pid(DWORD pid, std::wstring &out_image) {
      std::vector<std::wstring> matches;
      HANDLE proc = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
        FALSE,
        pid
      );
      if (!proc) {
        return matches;
      }
      out_image = process_image_basename(proc);
      std::vector<HMODULE> mods;
      if (enumerate_modules(proc, mods)) {
        for (HMODULE m : mods) {
          if (!m) {
            continue;
          }
          wchar_t name[MAX_PATH] = {};
          if (GetModuleBaseNameW(proc, m, name, _countof(name)) == 0) {
            continue;
          }
          const auto base = to_lower(name);
          for (auto target : kAiModuleNames) {
            if (base == target) {
              matches.emplace_back(base);
              break;
            }
          }
        }
      }
      CloseHandle(proc);
      return matches;
    }

    bool ends_with_ci(const std::string &haystack, const std::string &needle) {
      if (needle.size() > haystack.size()) {
        return false;
      }
      auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      };
      return std::equal(needle.rbegin(), needle.rend(), haystack.rbegin(), eq);
    }

    bool starts_with_ci(const std::string &haystack, const std::string &needle) {
      if (needle.size() > haystack.size()) {
        return false;
      }
      auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      };
      return std::equal(needle.begin(), needle.end(), haystack.begin(), eq);
    }

    // Shared state for last_event() / evaluate_and_tip().
    std::mutex g_mutex;
    std::optional<event_record_t> g_last;
    // Dedup key for evaluate_and_tip: prevents the tray spamming the
    // same tip every time the encoder is rebuilt mid-session.
    std::string g_last_tip_key;
    std::chrono::steady_clock::time_point g_last_tip_at {};
    constexpr std::chrono::minutes kTipCooldown {15};
  }  // namespace

  detection_t snapshot() {
    detection_t out {};
    out.any_match = false;
    out.has_dlss = false;
    out.has_dlss_fg = false;
    out.has_dlaa = false;
    out.at = std::chrono::system_clock::now();

    std::vector<DWORD> pids(1024);
    DWORD needed = 0;
    while (true) {
      const DWORD bytes = static_cast<DWORD>(pids.size() * sizeof(DWORD));
      if (!EnumProcesses(pids.data(), bytes, &needed)) {
        return out;
      }
      if (needed < bytes) {
        pids.resize(needed / sizeof(DWORD));
        break;
      }
      pids.assign(pids.size() * 2, 0);
      if (pids.size() > 32 * 1024) {
        return out;
      }
    }

    const DWORD self_pid = GetCurrentProcessId();
    for (DWORD pid : pids) {
      // System processes (idle = 0, system = 4) never load NVIDIA
      // user-mode AI modules, and our own service certainly doesn't.
      if (pid == 0 || pid == 4 || pid == self_pid) {
        continue;
      }
      std::wstring image;
      const auto matches = ai_modules_for_pid(pid, image);
      if (matches.empty()) {
        continue;
      }
      for (const auto &m : matches) {
        module_match_t mm;
        mm.module_name = to_utf8(m);
        mm.pid = static_cast<std::uint32_t>(pid);
        mm.process_image = to_utf8(image);
        out.matches.push_back(std::move(mm));
      }
      for (const auto &m : matches) {
        if (m == L"nvngx_dlss.dll") {
          out.has_dlss = true;
        } else if (m == L"nvngx_dlssg.dll") {
          out.has_dlss_fg = true;
        } else if (m == L"nvngx_dlaa.dll") {
          out.has_dlaa = true;
        }
      }
    }
    out.any_match = !out.matches.empty();
    return out;
  }

  bool config_is_at_risk(
    int resolution_width,
    int resolution_height,
    bool hdr_enabled,
    int bit_depth,
    const std::string &codec_label
  ) {
    if (!hdr_enabled || bit_depth < 10) {
      return false;
    }
    const long long pixels = static_cast<long long>(resolution_width) * resolution_height;
    if (pixels < (3840LL * 2160LL)) {
      return false;
    }
    return starts_with_ci(codec_label, "av1_nvenc")
      || starts_with_ci(codec_label, "hevc_nvenc")
      || ends_with_ci(codec_label, "_nvenc");
  }

  std::string make_streaming_tip(
    const detection_t &detection,
    int resolution_width,
    int resolution_height,
    const std::string &codec_label
  ) {
    if (!detection.any_match) {
      return {};
    }
    // Decide which AI feature to mention first. DLSS Frame Generation
    // is the most relevant when present; DLAA next; plain DLSS last.
    std::string ai_label;
    if (detection.has_dlss_fg) {
      ai_label = "DLSS Frame Generation";
    } else if (detection.has_dlaa) {
      ai_label = "DLAA";
    } else if (detection.has_dlss) {
      ai_label = "DLSS";
    } else {
      ai_label = "an NVIDIA AI render pass";
    }

    // Encoder hint: AV1 is the most contention-prone with concurrent
    // Tensor work; suggest HEVC as a smoother alternative without
    // claiming AV1 is broken.
    std::string codec_hint;
    if (starts_with_ci(codec_label, "av1_nvenc")) {
      codec_hint = " If you see occasional hiccups, switching the LuminalShine "
                   "codec to HEVC (Settings → Video) tends to ride out "
                   "concurrent Tensor + NVENC workloads more smoothly on "
                   "RTX 40 / 50 series.";
    } else {
      codec_hint = " If you see occasional hiccups, lowering the streaming "
                   "resolution or bit-depth (Settings → Video) often helps "
                   "keep the per-frame budget comfortable.";
    }

    std::string tip = "Streaming tip: this game has ";
    tip += ai_label;
    tip += " loaded, and the session is running at ";
    tip += std::to_string(resolution_width);
    tip += "×";
    tip += std::to_string(resolution_height);
    tip += " HDR. That stacks several heavy workloads on the GPU "
           "simultaneously — a known driver-level interaction, not "
           "a LuminalShine issue.";
    tip += codec_hint;
    tip += " No action needed if your session feels smooth.";
    return tip;
  }

  std::optional<event_record_t> last_event() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_last;
  }

  std::string evaluate_and_tip(
    int resolution_width,
    int resolution_height,
    bool hdr_enabled,
    int bit_depth,
    const std::string &codec_label
  ) {
    if (!config_is_at_risk(resolution_width, resolution_height, hdr_enabled, bit_depth, codec_label)) {
      return {};
    }
    const auto det = snapshot();

    event_record_t rec;
    rec.detection = det;
    rec.resolution_width = resolution_width;
    rec.resolution_height = resolution_height;
    rec.hdr_enabled = hdr_enabled;
    rec.bit_depth = bit_depth;
    rec.codec_label = codec_label;
    rec.tip = make_streaming_tip(det, resolution_width, resolution_height, codec_label);

    if (rec.tip.empty()) {
      // Still record the snapshot for diagnostics, but skip the
      // emission path entirely.
      std::lock_guard<std::mutex> lk(g_mutex);
      g_last = rec;
      return {};
    }

    // Dedup. Key on the (codec, resolution, hdr) tuple so a stream
    // that gets torn down and re-created with the same settings only
    // shows the tip once per cooldown window.
    std::string key = codec_label;
    key += '/';
    key += std::to_string(resolution_width);
    key += 'x';
    key += std::to_string(resolution_height);
    key += '/';
    key += hdr_enabled ? "hdr" : "sdr";

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      g_last = rec;
      const bool same_key = (key == g_last_tip_key);
      const bool within_cooldown =
        g_last_tip_at != std::chrono::steady_clock::time_point {}
        && (now - g_last_tip_at) < kTipCooldown;
      if (same_key && within_cooldown) {
        return {};
      }
      g_last_tip_key = key;
      g_last_tip_at = now;
    }

    BOOST_LOG(info) << rec.tip;
    return rec.tip;
  }

}  // namespace platf::render_stack
