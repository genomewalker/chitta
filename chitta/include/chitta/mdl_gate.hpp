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

} // namespace mdl
} // namespace chitta
