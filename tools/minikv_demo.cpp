#include "minikv/minikv.hpp"
#include "minikv/version.hpp"

#include <iostream>

int main() {
    minikv::MiniKV store;
    store.put("project", "MiniKV");
    store.put("language", "C++20");

    std::cout << "MiniKV " << minikv::version() << '\n';
    if (const auto project = store.get("project")) {
        std::cout << "project = " << *project << '\n';
    }

    std::cout << "keys before delete: " << store.size() << '\n';
    static_cast<void>(store.erase("language"));
    std::cout << "keys after delete: " << store.size() << '\n';
    std::cout << "All data disappears when this process exits.\n";
    return 0;
}
