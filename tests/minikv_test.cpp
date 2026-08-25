#include "minikv/minikv.hpp"
#include "minikv/version.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

class TestRunner {
public:
    void expect(bool condition, std::string_view description) {
        ++checks_;
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << description << '\n';
        }
    }

    [[nodiscard]] int finish() const {
        if (failures_ == 0) {
            std::cout << "All " << checks_ << " checks passed\n";
            return 0;
        }

        std::cerr << failures_ << " of " << checks_ << " checks failed\n";
        return 1;
    }

private:
    std::size_t checks_{0};
    std::size_t failures_{0};
};

void test_insert_read_and_overwrite(TestRunner& tests) {
    minikv::MiniKV store;

    tests.expect(store.empty(), "a new store is empty");
    tests.expect(store.size() == 0, "a new store has size zero");

    store.put("language", "C++20");
    tests.expect(!store.empty(), "put makes the store non-empty");
    tests.expect(store.size() == 1, "put inserts one key");
    tests.expect(store.contains("language"), "contains finds an existing key");
    tests.expect(
        store.get("language") == std::optional<std::string>{"C++20"},
        "get returns the inserted value");

    store.put("language", "modern C++");
    tests.expect(store.size() == 1, "overwrite does not add another key");
    tests.expect(
        store.get("language") == std::optional<std::string>{"modern C++"},
        "overwrite replaces the previous value");
}

void test_missing_and_delete(TestRunner& tests) {
    minikv::MiniKV store;

    tests.expect(!store.contains("missing"), "contains rejects a missing key");
    tests.expect(!store.get("missing").has_value(),
                 "get returns nullopt for a missing key");
    tests.expect(!store.erase("missing"),
                 "erase returns false for a missing key");

    store.put("temporary", "value");
    tests.expect(store.erase("temporary"),
                 "erase returns true for an existing key");
    tests.expect(!store.contains("temporary"),
                 "an erased key is no longer contained");
    tests.expect(!store.get("temporary").has_value(),
                 "get cannot read an erased key");
    tests.expect(store.empty(), "erasing the only key empties the store");
    tests.expect(!store.erase("temporary"),
                 "erasing the same key twice returns false");
}

void test_empty_and_binary_data(TestRunner& tests) {
    minikv::MiniKV store;

    store.put("", "empty key");
    tests.expect(
        store.get("") == std::optional<std::string>{"empty key"},
        "an empty key is valid");

    store.put("empty value", "");
    const auto empty_value = store.get("empty value");
    tests.expect(empty_value.has_value(),
                 "an empty value is distinct from a missing key");
    tests.expect(empty_value == std::optional<std::string>{""},
                 "get preserves an empty value");

    const std::string binary_key{"key\0tail", 8};
    const std::string binary_value{"left\0right", 10};
    store.put(binary_key, binary_value);
    tests.expect(store.contains(binary_key),
                 "keys may contain embedded NUL bytes");
    tests.expect(store.get(binary_key) ==
                     std::optional<std::string>{binary_value},
                 "values preserve embedded NUL bytes");
}

void test_independent_instances(TestRunner& tests) {
    minikv::MiniKV first;
    minikv::MiniKV second;

    first.put("shared name", "first value");
    second.put("shared name", "second value");

    tests.expect(first.get("shared name") ==
                     std::optional<std::string>{"first value"},
                 "the first instance owns its value");
    tests.expect(second.get("shared name") ==
                     std::optional<std::string>{"second value"},
                 "the second instance owns its value");

    tests.expect(first.erase("shared name"),
                 "the first instance can erase its key");
    tests.expect(second.contains("shared name"),
                 "erasing from one instance does not affect another");
}

}  // namespace

int main() {
    TestRunner tests;

    tests.expect(minikv::version() == "0.1.0-dev",
                 "the library exposes its version");
    test_insert_read_and_overwrite(tests);
    test_missing_and_delete(tests);
    test_empty_and_binary_data(tests);
    test_independent_instances(tests);

    return tests.finish();
}
