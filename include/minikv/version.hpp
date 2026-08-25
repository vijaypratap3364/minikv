#pragma once

#include <string_view>

namespace minikv {

// Returns build metadata only. Storage operations begin in Stage 1.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace minikv
