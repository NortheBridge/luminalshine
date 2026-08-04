#include <winsock2.h>
#include <windows.h>
#include <winevt.h>

#include "whea_diagnostics.h"

#include <string_view>
#include <vector>

namespace whea_diagnostics {
  namespace {
    std::string utf8(std::wstring_view value) {
      if (value.empty()) return {};
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
      if (bytes <= 0) return {};
      std::string out(static_cast<std::size_t>(bytes), '\0');
      WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), bytes, nullptr, nullptr);
      return out;
    }

    std::string xml_data(std::string_view xml, std::string_view name) {
      const std::string needle = "<Data Name='" + std::string(name) + "'>";
      auto begin = xml.find(needle);
      if (begin == std::string_view::npos) {
        const std::string double_needle = "<Data Name=\"" + std::string(name) + "\">";
        begin = xml.find(double_needle);
        if (begin == std::string_view::npos) return {};
        begin += double_needle.size();
      } else {
        begin += needle.size();
      }
      const auto end = xml.find("</Data>", begin);
      return end == std::string_view::npos ? std::string {} : std::string(xml.substr(begin, end - begin));
    }
  }

  std::optional<std::string> recent_pcie_aer_summary(std::chrono::milliseconds maximum_age) noexcept {
    try {
      const auto query_text = std::wstring(
        L"*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger'] and EventID=17 and "
        L"TimeCreated[timediff(@SystemTime) <= "
      ) + std::to_wstring(maximum_age.count()) + L"]]]";
      EVT_HANDLE query = EvtQuery(nullptr, L"System", query_text.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection);
      if (!query) return std::nullopt;

      EVT_HANDLE event = nullptr;
      DWORD returned = 0;
      const BOOL next_ok = EvtNext(query, 1, &event, 0, 0, &returned);
      EvtClose(query);
      if (!next_ok || returned == 0 || !event) return std::nullopt;

      DWORD bytes = 0;
      DWORD properties = 0;
      (void) EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &bytes, &properties);
      if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        EvtClose(event);
        return std::nullopt;
      }
      std::vector<wchar_t> buffer((bytes / sizeof(wchar_t)) + 1);
      if (!EvtRender(nullptr, event, EvtRenderEventXml, bytes, buffer.data(), &bytes, &properties)) {
        EvtClose(event);
        return std::nullopt;
      }
      EvtClose(event);

      const auto xml = utf8(buffer.data());
      const auto device = xml_data(xml, "PrimaryDeviceName");
      const auto uncorrectable = xml_data(xml, "UncorrectableErrorStatus");
      const auto correctable = xml_data(xml, "CorrectableErrorStatus");
      std::string summary = "recent WHEA-Logger event 17 (PCIe AER)";
      if (!device.empty()) summary += "; root_port=" + device;
      if (!uncorrectable.empty()) summary += "; uncorrectable_status=" + uncorrectable;
      if (!correctable.empty()) summary += "; correctable_status=" + correctable;
      return summary;
    } catch (...) {
      return std::nullopt;
    }
  }
}
