#include "minikv/version.hpp"

#include <iostream>

int main() {
    std::cout << "MiniKV " << minikv::version()
              << " (storage operations are not implemented yet)\n";
    return 0;
}
