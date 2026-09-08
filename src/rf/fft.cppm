// rf.fft — precomputed, in-place radix-2 forward FFT.
//
// Initialization owns all storage. execute() only reads the tables and mutates
// its caller-provided buffer, so the dataplane performs no allocation.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <cmath>
#include <cstddef>
#include <cstdint>
#endif

export module rf.fft;

#ifdef RF_IMPORT_STD
import std;
#endif
import rf.memory;

export namespace rf::fft {

struct Complex {
    float re;
    float im;
};

class Radix2FFT {
public:
    Radix2FFT() noexcept = default;
    Radix2FFT(const Radix2FFT&) = delete;
    Radix2FFT& operator=(const Radix2FFT&) = delete;

    [[nodiscard]] rf::memory::MemoryError initialize(
        std::size_t n,
        rf::memory::RegionConfig cfg = rf::memory::DefaultRegionConfig) noexcept
    {
        using rf::memory::MemoryError;
        if (n_ != 0) return MemoryError::already_initialized;
        if (n < 2 || n > MaxSize || (n & (n - 1)) != 0)
            return MemoryError::invalid_size;

        if (const MemoryError e = bit_reversal_.map(n, cfg); e != MemoryError::none)
            return e;
        if (const MemoryError e = twiddles_.map(n / 2, cfg); e != MemoryError::none) {
            bit_reversal_.unmap();
            return e;
        }

        unsigned bit_count = 0;
        for (std::size_t value = n; value > 1; value >>= 1) ++bit_count;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint32_t source = static_cast<std::uint32_t>(i);
            std::uint32_t reversed = 0;
            for (unsigned bit = 0; bit < bit_count; ++bit) {
                reversed = static_cast<std::uint32_t>((reversed << 1) | (source & 1u));
                source >>= 1;
            }
            bit_reversal_.data()[i] = reversed;
        }

        constexpr double Pi = 3.141592653589793238462643383279502884;
        for (std::size_t k = 0; k < n / 2; ++k) {
            const double angle = -2.0 * Pi * static_cast<double>(k) / static_cast<double>(n);
            twiddles_.data()[k] = {
                static_cast<float>(std::cos(angle)),
                static_cast<float>(std::sin(angle))
            };
        }
        n_ = n;
        return MemoryError::none;
    }

    void reset() noexcept {
        twiddles_.unmap();
        bit_reversal_.unmap();
        n_ = 0;
    }

    void execute(Complex* data) const noexcept {
        if (data == nullptr || n_ < 2) return;

        const std::uint32_t* reversal = bit_reversal_.data();
        for (std::size_t i = 0; i < n_; ++i) {
            const std::size_t j = reversal[i];
            if (j > i) {
                const Complex temporary = data[i];
                data[i] = data[j];
                data[j] = temporary;
            }
        }

        const Complex* twiddle = twiddles_.data();
        for (std::size_t length = 2; length <= n_; length <<= 1) {
            const std::size_t half = length / 2;
            const std::size_t twiddle_stride = n_ / length;
            for (std::size_t base = 0; base < n_; base += length) {
                for (std::size_t j = 0; j < half; ++j) {
                    const Complex w = twiddle[j * twiddle_stride];
                    const Complex odd = data[base + j + half];
                    const Complex product{
                        w.re * odd.re - w.im * odd.im,
                        w.re * odd.im + w.im * odd.re
                    };
                    const Complex even = data[base + j];
                    data[base + j] = {even.re + product.re, even.im + product.im};
                    data[base + j + half] = {even.re - product.re, even.im - product.im};
                }
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

private:
    static constexpr std::size_t MaxSize = std::size_t{1} << 20;

    rf::memory::PinnedRegion<std::uint32_t, 64> bit_reversal_;
    rf::memory::PinnedRegion<Complex, 64> twiddles_;
    std::size_t n_{0};
};

} // namespace rf::fft
