#pragma once
// MDL (minimum description length) consolidation gate — C++ port of
// chitta-mcp/mdl_gate.py. Read that file's module docstring for the full
// derivation and the design note on why naive concatenation (C(w+E) - C(w))
// was tried and rejected in favor of a zlib preset dictionary.
//
// Criterion: a distilled wisdom `w` is justified over its source evidence `E`
// iff C(w) + C(E|w) < C(E), where C(x) is deflate codelength and C(E|w) is
// measured by compressing E with w loaded as a zlib preset dictionary
// (deflateSetDictionary) rather than by concatenation. Evidence longer than
// the 32KB deflate window is chunked and each chunk compressed independently
// (with and without the dictionary); C(w) is charged once overall.
//
// zlib settings are pinned to match mdl_gate.py's
// zlib.compressobj(9, DEFLATED, wbits=-15, memLevel=9, strategy=0) exactly,
// so verdicts from the two implementations are directly comparable.
//
// Fail-open by construction: any zlib error yields a default (non-accepting,
// zero-saving) Verdict rather than throwing — callers should never let this
// gate block or fail a storage path.

#include <zlib.h>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace chitta {
namespace mdl {

constexpr std::size_t kChunkBytes    = 32 * 1024;  // deflate window (wbits=-15 => 32KB)
constexpr int         kDefaultMargin = 64;          // bytes; must save at least this much to accept

struct Verdict {
    bool accept  = false;
    long c_e     = 0;   // C(E): evidence cost alone
    long c_we    = 0;   // C(w) + C(E|w): two-part code cost
    long saving  = 0;   // c_e - c_we
    int  margin  = kDefaultMargin;
};

// Compressed byte length of `data` at deflate level 9, raw stream (no zlib
// header/adler32), matching mdl_gate.py's _WBITS=-15. If `zdict` is non-null
// and non-empty, it primes the compressor as a preset dictionary — this is
// the conditional-cost mechanism C(data|zdict). Returns -1 on any zlib error.
inline long compress_len(const std::string& data, const std::string* zdict) {
    z_stream strm{};
    if (deflateInit2(&strm, /*level=*/9, Z_DEFLATED, /*windowBits=*/-15,
                      /*memLevel=*/9, Z_DEFAULT_STRATEGY) != Z_OK) {
        return -1;
    }
    if (zdict && !zdict->empty()) {
        if (deflateSetDictionary(&strm,
                reinterpret_cast<const Bytef*>(zdict->data()),
                static_cast<uInt>(zdict->size())) != Z_OK) {
            deflateEnd(&strm);
            return -1;
        }
    }
    uLong bound = deflateBound(&strm, static_cast<uLong>(data.size()));
    std::vector<Bytef> out(bound > 0 ? bound : 1);

    strm.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    strm.avail_in  = static_cast<uInt>(data.size());
    strm.next_out  = out.data();
    strm.avail_out = static_cast<uInt>(out.size());

    int rc = deflate(&strm, Z_FINISH);
    long produced = static_cast<long>(strm.total_out);
    deflateEnd(&strm);
    return (rc == Z_STREAM_END) ? produced : -1;
}

// Split `data` into <=kChunkBytes pieces, mirroring mdl_gate.py's _chunks():
// empty input yields a single empty chunk so C(E) for empty evidence is
// well-defined (the deflate codelength of zero bytes, not zero itself).
inline std::vector<std::string> chunks(const std::string& data, std::size_t size = kChunkBytes) {
    std::vector<std::string> out;
    if (data.empty()) {
        out.emplace_back();
        return out;
    }
    for (std::size_t i = 0; i < data.size(); i += size) {
        out.push_back(data.substr(i, size));
    }
    return out;
}

// Judge whether `wisdom` compresses its source `evidence` by at least
// `margin` bytes under the two-part MDL code. Fail-open: any internal zlib
// error returns a default Verdict (accept=false, saving=0) rather than
// throwing.
// Preserve source-chunk boundaries: each conditional cost gets a fresh deflate
// dictionary, while the wisdom is charged once across the whole evidence pool.
inline Verdict judge_chunks(const std::string& wisdom, const std::vector<std::string>& evidence,
                      int margin = kDefaultMargin) {
    Verdict v;
    v.margin = margin;

    long c_w = 0;
    if (!wisdom.empty()) {
        long r = compress_len(wisdom, nullptr);
        if (r < 0) return Verdict{};
        c_w = r;
    }

    long c_e = 0;
    long c_e_given_w = 0;
    for (const auto& source : evidence) {
        for (const auto& chunk : chunks(source)) {
            long ce = compress_len(chunk, nullptr);
            if (ce < 0) return Verdict{};
            c_e += ce;

            long cew = wisdom.empty() ? compress_len(chunk, nullptr)
                                       : compress_len(chunk, &wisdom);
            if (cew < 0) return Verdict{};
            c_e_given_w += cew;
        }
    }

    v.c_e    = c_e;
    v.c_we   = c_w + c_e_given_w;
    v.saving = c_e - v.c_we;
    v.accept = v.saving >= margin;
    return v;
}

inline Verdict judge(const std::string& wisdom, const std::string& evidence,
                     int margin = kDefaultMargin) {
    return judge_chunks(wisdom, {evidence}, margin);
}

// The corpus model is separate from the legacy source-evidence judge above.
// Charge raw learning bytes once; each independent conversation earns savings.
// A zlib dictionary uses only its final 32 KiB. Keep this truncation explicit.
inline std::string dictionary_tail(const std::string& text) {
    return text.substr(text.size() > kChunkBytes ? text.size() - kChunkBytes : 0);
}

inline Verdict judge_corpus(const std::string& learning,
                            const std::vector<std::string>& corpus,
                            const std::string& baseline = "",
                            int margin = kDefaultMargin) {
    Verdict v;
    v.margin = margin;
    const auto before = dictionary_tail(baseline);
    const auto after = dictionary_tail(before + learning);
    long conditional = 0;
    for (const auto& source : corpus) {
        for (const auto& chunk : chunks(source)) {
            const long a = compress_len(chunk, &before);
            const long b = compress_len(chunk, &after);
            if (a < 0 || b < 0) return Verdict{};
            v.c_e += a;
            conditional += b;
        }
    }
    v.c_we = conditional + static_cast<long>(learning.size());
    v.saving = v.c_e - v.c_we;
    v.accept = !learning.empty() && !corpus.empty() && v.saving >= margin;
    return v;
}

inline size_t corpus_env_limit(const char* name, size_t fallback, size_t maximum) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    const std::string text(value);
    if (text.find_first_not_of("0123456789") != std::string::npos) return fallback;
    try { return std::min<size_t>(std::stoull(text), maximum); }
    catch (...) { return fallback; }
}
inline size_t corpus_chunk_limit() {
    return corpus_env_limit("CHITTA_MDL_CORPUS_CHUNKS", 8, 128);
}
inline size_t corpus_byte_limit() {
    return corpus_env_limit("CHITTA_MDL_CORPUS_BYTES", 512 * 1024, 8 * 1024 * 1024);
}

// Shared across ephemeral distillers. A snapshot is taken BEFORE insertion and
// travels with its prepared candidates, so parallel prepares cannot leak future
// evidence into their verdicts. Overlapping/retried ranges never earn credit.
class CorpusRing {
public:
    using Key = std::array<std::string, 2>; // mind, realm
    struct Source {
        std::string session;
        int64_t begin = 0;
        int64_t end = 0;
        std::string text;
    };
    std::vector<std::string> observe(const Key& key, const Source& current,
                                    size_t count = corpus_chunk_limit(),
                                    size_t cap = corpus_byte_limit()) {
        std::lock_guard<std::mutex> lock(mutex_);
        count = std::min<size_t>(count, 128);
        cap = std::min<size_t>(cap, 8 * 1024 * 1024);
        if (!count || !cap) { history_.erase(key); return {}; }
        // Bound the number of realm/mind rings as well as each ring's payload.
        if (!history_.count(key) && history_.size() >= 64) {
            auto oldest = std::min_element(history_.begin(), history_.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            history_.erase(oldest);
        }
        auto& h = history_[key];
        h.used = ++clock_;
        for (auto it = h.sources.begin(); it != h.sources.end();) {
            if ((it->session == current.session && it->end > current.begin) ||
                it->text == current.text) {
                h.bytes -= it->text.size();
                it = h.sources.erase(it);
            } else ++it;
        }
        auto trim = [&] {
            while (!h.sources.empty() && (h.sources.size() > count || h.bytes > cap)) {
                h.bytes -= h.sources.front().text.size();
                h.sources.pop_front();
            }
        };
        trim();
        std::vector<std::string> result;
        for (const auto& source : h.sources) result.push_back(source.text);
        // An oversized chunk is skipped, never silently turned into a fragment.
        if (!current.text.empty() && current.text.size() <= cap &&
            !current.session.empty() && current.end > current.begin) {
            h.sources.push_back(current);
            h.bytes += current.text.size();
        }
        trim();
        return result;
    }
private:
    struct History { size_t used = 0, bytes = 0; std::deque<Source> sources; };
    std::map<Key, History> history_;
    size_t clock_ = 0;
    std::mutex mutex_;
};

} // namespace mdl
} // namespace chitta
