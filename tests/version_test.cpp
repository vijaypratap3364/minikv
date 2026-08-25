#include "minikv/version.hpp"

#include <iostream>
#include <string_view>

int main() {
    constexpr std::string_view expected_version{"0.1.0-dev"};

    if (minikv::version() != expected_version) {
        std::cerr << "expected version " << expected_version << ", got "
                  << minikv::version() << '\n';
        return 1;
    }

    return 0;
}
