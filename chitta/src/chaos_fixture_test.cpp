// CTest's small snapshot family; the integration runner copies it through the
// same eval-replica selector used with the frozen evaluation corpus.
#include "chitta/field_store.hpp"
#include <cassert>
#include <filesystem>
int main(int argc, char** argv) {
    assert(argc == 2 || (argc == 4 && std::string(argv[2]) == "--zero-decay"));
    std::filesystem::create_directories(argv[1]);
    chitta::FieldStore store(argv[1], std::string(argv[1]) + "/locks");
    if (argc == 4) {
        // Called only with the scratch daemon stopped. get() exposes effective
        // strength, which decays with wall time; make the queue replay fixture
        // time-independent so exact state equality stays meaningful.
        const float unchanged = std::numeric_limits<float>::quiet_NaN();
        assert(cf_update_state(store.handle(), std::stoull(argv[3]),
                               unchanged, unchanged, 0.0f, 0, -1) == 0);
        assert(cf_sync(store.handle()) == 0);
        return 0;
    }
    std::vector<float> embedding(CHITTA_EMBED_DIM, 0.0f);
    embedding[0] = 1;
    for (int i = 0; i < 128; ++i)
        assert(store.remember("wisdom", "chaos", "chaos fixture " + std::to_string(i), embedding, .9f, .001f, 0) > 0);
    assert(store.save_full_snapshot());
}
