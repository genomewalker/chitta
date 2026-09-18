#include "chitta/llm_http.hpp"
#include "chitta/native_distiller.hpp"
#include <cassert>

int main(int argc, char** argv) {
    assert(argc == 2);
    unsetenv("CHITTA_DISTILL_THINK");
    assert(chitta::NativeDistillConfig{}.think);
    for (const char* setting : {"0", "false", "off"}) {
        setenv("CHITTA_DISTILL_THINK", setting, 1);
        assert(!chitta::NativeDistillConfig{}.think);
    }
    setenv("CHITTA_DISTILL_THINK", "true", 1);
    assert(chitta::NativeDistillConfig{}.think);
    for (const std::optional<bool> think : {std::optional<bool>{true}, std::optional<bool>{false}, std::optional<bool>{}}) {
        auto text = chitta::call_llm_http(argv[1], "test-model", "body", "system", 5, 0.3f, 8192, nullptr, think);
        assert(text == "[PATTERN] a→b");
    }
}
