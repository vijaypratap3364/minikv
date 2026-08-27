#pragma once

#include <filesystem>

namespace minikv::detail {

// Requests that file contents and metadata reach the operating system's
// durable-storage boundary. Throws std::system_error on failure.
void sync_file_to_storage(const std::filesystem::path& path);

// Atomically replaces a manifest file on the same filesystem. When durable is
// true, the platform-specific implementation also requests durable rename
// completion using the strongest practical local primitive.
void replace_file_atomically(const std::filesystem::path& source,
                             const std::filesystem::path& destination,
                             bool durable);

// Installs a completed directory at a destination that must not exist. Source
// and destination must share a filesystem so the rename is atomic.
void install_directory_atomically(const std::filesystem::path& source,
                                  const std::filesystem::path& destination,
                                  bool durable);

}  // namespace minikv::detail
