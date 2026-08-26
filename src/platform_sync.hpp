#pragma once

#include <filesystem>

namespace minikv::detail {

// Requests that file contents and metadata reach the operating system's
// durable-storage boundary. Throws std::system_error on failure.
void sync_file_to_storage(const std::filesystem::path& path);

}  // namespace minikv::detail
