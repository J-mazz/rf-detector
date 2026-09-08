// rf.simd — vector kernels behind one interface.
//
// Ordering rule: GenericFallback is defined first, NeonBackend second
// (it may delegate to the fallback), DefaultSIMDBackend alias last. The
// whole namespace is exported once; nothing inside carries its own `export`.
//
// Determinism rule: every kernel is bitwise-identical across backends when
// compiled with -ffp-contract=off. Reductions use the same 4-lane
// accumulation and the same ((l0+l1)+(l2+l3)) fold in both backends, so a
// Pi (NEON) and an x86 workstation (scalar) produce the same detector
// decisions on the same IQ file. Switching to fused FMA is a deliberate
// later decision that must come with tolerance-based tests.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <cstddef>
#include <cstdint>
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

export module rf.simd;

#ifdef RF_IMPORT_STD
import std;
#endif

export namespace rf::simd {

enum class InstructionSet : std::uint8_t { GenericScalar, ArmNeon };

inline constexpr float S16Scale = 1.0f / 32768.0f;   // exact power of two

class GenericFallback {
public:
    static constexpr InstructionSet ISA = InstructionSet::GenericScalar;
    static constexpr std::size_t VectorLanes = 4;      // mirrored fold width

    // Interleaved int16 I/Q → planar float32 in [-1, 1).
    static void deinterleave_and_convert_s16_f32(
        const std::int16_t* __restrict src_interleaved,
        float* __restrict dst_i,
        float* __restrict dst_q,
        std::size_t complex_count, float scale = S16Scale) noexcept
    {
        for (std::size_t n = 0; n < complex_count; ++n) {
            dst_i[n] = static_cast<float>(src_interleaved[2 * n])     * scale;
            dst_q[n] = static_cast<float>(src_interleaved[2 * n + 1]) * scale;
        }
    }

    static void magnitude_squared(
        const float* __restrict src_i,
        const float* __restrict src_q,
        float* __restrict dst,
        std::size_t count) noexcept
    {
        for (std::size_t n = 0; n < count; ++n) {
            const float ii = src_i[n] * src_i[n];
            const float qq = src_q[n] * src_q[n];
            dst[n] = ii + qq;
        }
    }

    // Sum with a fixed association order (see determinism rule).
    [[nodiscard]] static float sum(const float* __restrict src, std::size_t count) noexcept {
        float l0 = 0.0f, l1 = 0.0f, l2 = 0.0f, l3 = 0.0f;
        std::size_t n = 0;
        for (; n + 3 < count; n += 4) {
            l0 += src[n];
            l1 += src[n + 1];
            l2 += src[n + 2];
            l3 += src[n + 3];
        }
        float acc = (l0 + l1) + (l2 + l3);
        for (; n < count; ++n) acc += src[n];
        return acc;
    }
};

#if defined(__aarch64__)
class NeonBackend {
public:
    static constexpr InstructionSet ISA = InstructionSet::ArmNeon;
    static constexpr std::size_t VectorLanes = 4;

    static void deinterleave_and_convert_s16_f32(
        const std::int16_t* __restrict src_interleaved,
        float* __restrict dst_i,
        float* __restrict dst_q,
        std::size_t complex_count, float scale = S16Scale) noexcept
    {
        const float32x4_t vscale = vdupq_n_f32(scale);
        std::size_t n = 0;
        for (; n + 7 < complex_count; n += 8) {
            const int16x8x2_t raw = vld2q_s16(src_interleaved + 2 * n);   // structure load deinterleaves
            const int32x4_t i_lo = vmovl_s16(vget_low_s16(raw.val[0]));
            const int32x4_t i_hi = vmovl_s16(vget_high_s16(raw.val[0]));
            const int32x4_t q_lo = vmovl_s16(vget_low_s16(raw.val[1]));
            const int32x4_t q_hi = vmovl_s16(vget_high_s16(raw.val[1]));
            vst1q_f32(dst_i + n,     vmulq_f32(vcvtq_f32_s32(i_lo), vscale));
            vst1q_f32(dst_i + n + 4, vmulq_f32(vcvtq_f32_s32(i_hi), vscale));
            vst1q_f32(dst_q + n,     vmulq_f32(vcvtq_f32_s32(q_lo), vscale));
            vst1q_f32(dst_q + n + 4, vmulq_f32(vcvtq_f32_s32(q_hi), vscale));
        }
        if (n < complex_count) {
            GenericFallback::deinterleave_and_convert_s16_f32(
                src_interleaved + 2 * n, dst_i + n, dst_q + n, complex_count - n, scale);
        }
    }

    static void magnitude_squared(
        const float* __restrict src_i,
        const float* __restrict src_q,
        float* __restrict dst,
        std::size_t count) noexcept
    {
        std::size_t n = 0;
        for (; n + 3 < count; n += 4) {
            const float32x4_t vi = vld1q_f32(src_i + n);
            const float32x4_t vq = vld1q_f32(src_q + n);
            // explicit mul + add (not vmla/vfma): matches the scalar path bit for bit
            vst1q_f32(dst + n, vaddq_f32(vmulq_f32(vi, vi), vmulq_f32(vq, vq)));
        }
        if (n < count) GenericFallback::magnitude_squared(src_i + n, src_q + n, dst + n, count - n);
    }

    [[nodiscard]] static float sum(const float* __restrict src, std::size_t count) noexcept {
        float32x4_t acc = vdupq_n_f32(0.0f);
        std::size_t n = 0;
        for (; n + 3 < count; n += 4) acc = vaddq_f32(acc, vld1q_f32(src + n));
        // (l0+l1) and (l2+l3) via pairwise add, then one scalar add: same order as the fallback
        const float32x2_t pairs = vpadd_f32(vget_low_f32(acc), vget_high_f32(acc));
        float result = vget_lane_f32(pairs, 0) + vget_lane_f32(pairs, 1);
        for (; n < count; ++n) result += src[n];
        return result;
    }
};
using DefaultSIMDBackend = NeonBackend;
#else
using DefaultSIMDBackend = GenericFallback;
#endif

} // namespace rf::simd
