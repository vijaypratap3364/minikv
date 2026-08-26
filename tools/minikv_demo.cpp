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
            minikv::MiniKV store(log_path);
            store.put("project", "MiniKV");
            store.put("language", "C++20");
        }

        minikv::MiniKV reopened(log_path);

        std::cout << "MiniKV " << minikv::version() << '\n';
        if (const auto project = reopened.get("project")) {
            std::cout << "recovered project = " << *project << '\n';
        }

        std::cout << "recovered keys: " << reopened.size() << '\n';
        static_cast<void>(reopened.erase("language"));
        std::cout << "keys after in-memory delete: " << reopened.size() << '\n';
        std::cout << "records appended to " << log_path << '\n';
        std::cout << "DELETE is in-memory only through Stage 3\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MiniKV error: " << error.what() << '\n';
        return 1;
    }
}
