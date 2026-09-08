#include <cstdio> // stderr and CHECK diagnostics also require macros with import std.
#include <new>
// test_fft — radix-2 FFT correctness against analytic cases and an
// independent double-precision DFT.
#ifndef RF_IMPORT_STD
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
#endif

#ifdef RF_IMPORT_STD
import std;
#endif
import rf.memory;
import rf.core;
import rf.fft;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); std::exit(1); } } while (0)

namespace {

using rf::fft::Complex;

[[nodiscard]] float magnitude(Complex z) {
    return std::sqrt(z.re * z.re + z.im * z.im);
}

void check_close(float actual, double expected, double tolerance) {
    CHECK(std::fabs(static_cast<double>(actual) - expected) <= tolerance);
}

void test_invalid_initialization_and_reset() {
    rf::fft::Radix2FFT fft;
    CHECK(fft.size() == 0);
    CHECK(fft.initialize(0, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::invalid_size);
    CHECK(fft.initialize(1, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::invalid_size);
    CHECK(fft.initialize(3, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::invalid_size);
    CHECK(fft.initialize(std::numeric_limits<std::size_t>::max(), rf::memory::UnlockedRegionConfig) ==
          rf::memory::MemoryError::invalid_size);

    CHECK(fft.initialize(8, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::none);
    CHECK(fft.size() == 8);
    CHECK(fft.initialize(16, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::already_initialized);
    CHECK(fft.size() == 8);
    fft.reset();
    CHECK(fft.size() == 0);
    fft.reset();
    CHECK(fft.initialize(16, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::none);
    CHECK(fft.size() == 16);
}

void test_impulse() {
    constexpr std::size_t N = 16;
    rf::fft::Radix2FFT fft;
    CHECK(fft.initialize(N, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::none);
    std::vector<Complex> data(N, Complex{0.0f, 0.0f});
    data[0] = {1.0f, 0.0f};
    fft.execute(data.data());
    for (Complex z : data) {
        check_close(z.re, 1.0, 1e-6);
        check_close(z.im, 0.0, 1e-6);
    }
}

void test_signed_frequency_tones() {
    constexpr std::size_t N = 64;
    constexpr std::size_t Bin = 7;
    constexpr double Pi = 3.141592653589793238462643383279502884;
    rf::fft::Radix2FFT fft;
    CHECK(fft.initialize(N, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::none);

    for (int sign : {1, -1}) {
        std::vector<Complex> data(N);
        for (std::size_t n = 0; n < N; ++n) {
            const double angle = sign * 2.0 * Pi * static_cast<double>(Bin * n) / static_cast<double>(N);
            data[n] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
        }
        fft.execute(data.data());
        const std::size_t peak = sign > 0 ? Bin : N - Bin;
        check_close(data[peak].re, static_cast<double>(N), 2e-4);
        check_close(data[peak].im, 0.0, 2e-4);
        for (std::size_t k = 0; k < N; ++k) {
            if (k != peak) CHECK(magnitude(data[k]) < 2e-4f);
        }
    }
}

void test_independent_direct_dft() {
    constexpr std::size_t N = 8;
    constexpr double Pi = 3.141592653589793238462643383279502884;
    const Complex input[N] = {
        {0.25f, -0.50f}, {1.00f, 0.125f}, {-0.75f, 0.25f}, {0.50f, -1.00f},
        {0.00f, 0.75f}, {-0.25f, -0.25f}, {0.625f, 0.50f}, {-1.00f, 0.00f}
    };
    std::vector<Complex> data(input, input + N);
    rf::fft::Radix2FFT fft;
    CHECK(fft.initialize(N, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::none);
    fft.execute(data.data());

    for (std::size_t k = 0; k < N; ++k) {
        double expected_re = 0.0;
        double expected_im = 0.0;
        for (std::size_t n = 0; n < N; ++n) {
            const double angle = -2.0 * Pi * static_cast<double>(k * n) / static_cast<double>(N);
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            expected_re += static_cast<double>(input[n].re) * c - static_cast<double>(input[n].im) * s;
            expected_im += static_cast<double>(input[n].re) * s + static_cast<double>(input[n].im) * c;
        }
        check_close(data[k].re, expected_re, 2e-5);
        check_close(data[k].im, expected_im, 2e-5);
    }
}

void test_parseval_at_detector_size() {
    constexpr std::size_t N = 8192;
    rf::fft::Radix2FFT fft;
    CHECK(fft.initialize(N, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::none);
    std::vector<Complex> data(N);
    std::uint32_t state = 0x9e3779b9u;
    double input_energy = 0.0;
    for (Complex& z : data) {
        state = state * 1664525u + 1013904223u;
        z.re = static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f;
        state = state * 1664525u + 1013904223u;
        z.im = static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f;
        input_energy += static_cast<double>(z.re) * z.re + static_cast<double>(z.im) * z.im;
    }
    fft.execute(data.data());
    double output_energy = 0.0;
    for (Complex z : data) {
        output_energy += static_cast<double>(z.re) * z.re + static_cast<double>(z.im) * z.im;
    }
    const double recovered_input_energy = output_energy / static_cast<double>(N);
    CHECK(std::fabs(recovered_input_energy - input_energy) <= 2e-5 * input_energy);
}

} // namespace

int main() {
    test_invalid_initialization_and_reset();
    test_impulse();
    test_signed_frequency_tones();
    test_independent_direct_dft();
    test_parseval_at_detector_size();
    std::puts("test_fft: PASS");
    return 0;
}
