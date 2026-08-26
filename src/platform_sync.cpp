#include "platform_sync.hpp"

#include <system_error>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace minikv::detail {
namespace {

class NativeFileHandle {
public:
    explicit NativeFileHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~NativeFileHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
        }
    }

    NativeFileHandle(const NativeFileHandle&) = delete;
    NativeFileHandle& operator=(const NativeFileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_;
};

}  // namespace

void sync_file_to_storage(const std::filesystem::path& path) {
    const auto native_path = path.wstring();
    NativeFileHandle handle(CreateFileW(
        native_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "could not open file for FlushFileBuffers");
    }

    if (FlushFileBuffers(handle.get()) == 0) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "FlushFileBuffers failed");
    }
}

}  // namespace minikv::detail

#else

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace minikv::detail {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor) noexcept
        : descriptor_(descriptor) {}

    ~FileDescriptor() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

}  // namespace

void sync_file_to_storage(const std::filesystem::path& path) {
    int flags = O_WRONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    FileDescriptor descriptor(::open(path.c_str(), flags));
    if (descriptor.get() < 0) {
        throw std::system_error(
            errno, std::generic_category(), "could not open file for fsync");
    }

    while (::fsync(descriptor.get()) != 0) {
        if (errno != EINTR) {
            throw std::system_error(
                errno, std::generic_category(), "fsync failed");
        }
    }
}

}  // namespace minikv::detail

#endif
