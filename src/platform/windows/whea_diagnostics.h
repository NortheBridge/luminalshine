#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace whea_diagnostics {
  std::optional<std::string> recent_pcie_aer_summary(
    std::chrono::milliseconds maximum_age = std::chrono::minutes(2)
  ) noexcept;
}
