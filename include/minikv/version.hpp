#pragma once

#include <string_view>

namespace minikv {

// Returns build metadata for diagnostics.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace minikv
