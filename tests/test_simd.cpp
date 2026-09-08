#include <cstdio> // stderr and CHECK diagnostics also require macros with import std.
#include <new>
// test_simd — kernel correctness against a double-precision reference and,
// on AArch64, bitwise parity between NeonBackend and GenericFallback
// (including every tail length 0..15).
#ifndef RF_IMPORT_STD
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#endif

#ifdef RF_IMPORT_STD
import std;
#endif
import rf.simd;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); std::exit(1); } } while (0)

namespace {
std::uint64_t rng = 0x243F6A8885A308D3ull;
std::int16_t next_s16() {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return static_cast<std::int16_t>(rng & 0xFFFFu);
}

template <typename A, typename B>
void check_parity(std::size_t n) {
    std::vector<std::int16_t> src(2 * n + 16);
    for (auto& v : src) v = next_s16();
    std::vector<float> ia(n + 16), qa(n + 16), ib(n + 16), qb(n + 16);
    A::deinterleave_and_convert_s16_f32(src.data(), ia.data(), qa.data(), n);
    B::deinterleave_and_convert_s16_f32(src.data(), ib.data(), qb.data(), n);
    CHECK(std::memcmp(ia.data(), ib.data(), n * sizeof(float)) == 0);
    CHECK(std::memcmp(qa.data(), qb.data(), n * sizeof(float)) == 0);

    std::vector<float> ma(n + 16), mb(n + 16);
    A::magnitude_squared(ia.data(), qa.data(), ma.data(), n);
    B::magnitude_squared(ib.data(), qb.data(), mb.data(), n);
    CHECK(std::memcmp(ma.data(), mb.data(), n * sizeof(float)) == 0);

    const float sa = A::sum(ma.data(), n);
    const float sb = B::sum(mb.data(), n);
    CHECK(std::memcmp(&sa, &sb, sizeof(float)) == 0);      // bitwise, not approximately
}

template <typename K>
void check_reference(std::size_t n) {
    std::vector<std::int16_t> src(2 * n);
    for (auto& v : src) v = next_s16();
    std::vector<float> i(n), q(n), m(n);
    K::deinterleave_and_convert_s16_f32(src.data(), i.data(), q.data(), n);
    for (std::size_t k = 0; k < n; ++k) {
        CHECK(i[k] == static_cast<float>(src[2 * k]) / 32768.0f);       // exact: power-of-two scale
        CHECK(q[k] == static_cast<float>(src[2 * k + 1]) / 32768.0f);
    }
    K::magnitude_squared(i.data(), q.data(), m.data(), n);
    double ref_sum = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double ref = double(i[k]) * i[k] + double(q[k]) * q[k];
        CHECK(std::fabs(m[k] - ref) <= 1e-6 * (1.0 + ref));
        ref_sum += ref;
    }
    const float s = K::sum(m.data(), n);
    CHECK(std::fabs(s - ref_sum) <= 1e-4 * (1.0 + ref_sum));            // float accumulation over n terms
}
} // namespace

int main() {
    using rf::simd::GenericFallback;
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{1023},
                          std::size_t{1024}, std::size_t{131'072}}) {
        check_reference<GenericFallback>(n);
        check_reference<rf::simd::DefaultSIMDBackend>(n);
    }
#if defined(__aarch64__)
    for (std::size_t n = 0; n < 64; ++n) check_parity<rf::simd::NeonBackend, GenericFallback>(n);
    for (std::size_t n : {std::size_t{1023}, std::size_t{1024}, std::size_t{4097}, std::size_t{131'072}})
        check_parity<rf::simd::NeonBackend, GenericFallback>(n);
    std::puts("simd: NEON == scalar bitwise, reference ok");
#else
    std::puts("simd: scalar reference ok (no NEON on this host)");
#endif
    std::puts("test_simd: PASS");
    return 0;
}
