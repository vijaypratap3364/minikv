#include "minikv/minikv.hpp"
#include "minikv/version.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argument_count, char* arguments[]) {
    try {
        const std::filesystem::path log_path =
            argument_count > 1 ? arguments[1] : "minikv-demo.minikv";
        {
            minikv::MiniKV store(
                log_path, minikv::DurabilityMode::Sync);
            store.put("project", "MiniKV");
            store.put("language", "C++20");
        }

        std::cout << "MiniKV " << minikv::version() << '\n';
        {
            minikv::MiniKV reopened(
                log_path, minikv::DurabilityMode::Sync);
            if (const auto project = reopened.get("project")) {
                std::cout << "recovered project = " << *project << '\n';
            }

            std::cout << "recovered keys: " << reopened.size() << '\n';
            static_cast<void>(reopened.erase("language"));
        }

        minikv::MiniKV after_delete(
            log_path, minikv::DurabilityMode::Sync);
        std::cout << "language exists after delete + restart: "
                  << std::boolalpha << after_delete.contains("language")
                  << '\n';
        std::cout << "database directory: " << log_path << '\n';
        std::cout << "DELETE persists as a checksummed tombstone\n";
        std::cout << "demo writes request native durable sync\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MiniKV error: " << error.what() << '\n';
        return 1;
    }
}
