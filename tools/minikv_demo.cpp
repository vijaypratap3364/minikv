#include "minikv/minikv.hpp"
#include "minikv/version.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argument_count, char* arguments[]) {
    try {
        const std::filesystem::path log_path =
            argument_count > 1 ? arguments[1] : "minikv-demo.minikv";
        minikv::MiniKV store(log_path);
        store.put("project", "MiniKV");
        store.put("language", "C++20");

        std::cout << "MiniKV " << minikv::version() << '\n';
        if (const auto project = store.get("project")) {
            std::cout << "project = " << *project << '\n';
        }

        std::cout << "keys before delete: " << store.size() << '\n';
        static_cast<void>(store.erase("language"));
        std::cout << "keys after delete: " << store.size() << '\n';
        std::cout << "records appended to " << log_path << '\n';
        std::cout << "DELETE is in-memory only during Stage 2\n";
        std::cout << "restart recovery is not implemented yet\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MiniKV error: " << error.what() << '\n';
        return 1;
    }
}
