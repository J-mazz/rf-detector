// rf.signal — dataplane record layouts.
//
// Every type here is an OverwriteRecord (rf.core): no default member
// initializers, no constructors, no destructors. Storage is established
// once; producers write every field they claim is valid. `zero()`
// factories exist for control-plane convenience only.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <cstddef>
#include <cstdint>
#endif

export module rf.signal;

#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;

export namespace rf::signal {

using rf::core::CacheLineSize;
using rf::core::FrequencyHz;
using rf::core::SampleRateSps;
using rf::core::TimestampNs;
using rf::core::WallNs;

// Units convention: `_complex` counts complex (I,Q) samples. An interleaved
// int16 buffer holding N complex samples has 2N scalar elements.

enum GapFlags : std::uint32_t { GapNone=0, GapDevice=1, GapDiscard=2, GapConfiguration=4 };
struct CaptureMetadata {
    TimestampNs timestamp_ns; // host read-completion time, NOT first-sample time
    WallNs wall_ns;
    std::int64_t hw_time_ns; // first sample, meaningful only when hw_time_valid
    FrequencyHz center_frequency_hz;
    SampleRateSps sample_rate_sps;
    double bandwidth_hz;
    float gain_db;
    std::uint32_t receiver_id;
    std::uint64_t sequence;
    std::uint64_t segment_id;
    std::uint64_t config_id;
    std::uint64_t first_sample; // segment-relative; independent of USB read sizes
    std::uint64_t known_lost_samples;
    std::uint32_t gap_flags;
    bool hw_time_valid;
    std::uint8_t reserved[3];
    static constexpr CaptureMetadata zero() noexcept { return CaptureMetadata{}; }
};
static_assert(rf::core::OverwriteRecord<CaptureMetadata>);

// Non-owning view of a capture slot. `valid_complex` is established by the
// producer from the device's actual return count — never from capacity.
struct RawCaptureBlock {
    std::int16_t*   interleaved;        // I0 Q0 I1 Q1 ...
    std::size_t     capacity_complex;
    std::size_t     valid_complex;
    CaptureMetadata meta;
};
static_assert(rf::core::OverwriteRecord<RawCaptureBlock>);

// Non-owning planar (SoA) view over a scratch slot.
struct PlanarIQBlock {
    float*          i;
    float*          q;
    std::size_t     capacity_complex;
    std::size_t     count_complex;
    CaptureMetadata meta;
};
static_assert(rf::core::OverwriteRecord<PlanarIQBlock>);

// Bin ranges are half-open: [bin_begin, bin_end).
enum class EventPhase : std::uint8_t { begin, update, end };
enum class EventEnd : std::uint8_t { quiet, gap, invalid_input, finish };
enum class DetectorHealth : std::uint8_t { uncalibrated, tracking, uncertain };
struct SpectralCandidate {
    std::uint64_t event_id; // stable across begin/update/end records
    std::uint32_t bin_begin; // FFT-shifted bins, half-open
    std::uint32_t bin_end;
    std::uint64_t first_sample; // first analysis window supporting this event
    std::uint64_t end_sample;   // end of last supporting window, exclusive
    std::uint64_t segment_id;
    float energy_db;
    float noise_floor_db;
    float excess_over_background_db;
    float center_offset_hz;
    float bandwidth_hz;
    float duration_s;
    std::int64_t first_hw_time_ns;
    bool hw_time_valid;
    EventEnd end_reason;
    EventPhase phase;
    std::uint8_t reserved[5];
    static constexpr SpectralCandidate zero() noexcept { return SpectralCandidate{}; }
};
static_assert(rf::core::OverwriteRecord<SpectralCandidate>);

// Event tensor layout invariant (also the ONNX/TensorRT input contract):
//   time-major, data[hop * Bins + bin]; bins contiguous.
//   Model input shape: [1, 1, Hops, Bins] (N, C, H=time, W=frequency).
//   Quantization: value_db = scale * (q - zero_point), symmetric int8.
inline constexpr std::size_t SpectrogramBins = 128;
inline constexpr std::size_t SpectrogramHops = 32;

struct TensorQuant {
    float       scale;                 // dB per LSB
    std::int8_t zero_point;
    std::int8_t reserved0[3];
};
static_assert(rf::core::OverwriteRecord<TensorQuant>);

struct EventTensor {
    static constexpr std::size_t Hops = SpectrogramHops;
    static constexpr std::size_t Bins = SpectrogramBins;
    static constexpr std::size_t Elements = Hops * Bins;

    alignas(CacheLineSize) std::int8_t data[Elements];   // overwrite-only; never zeroed on reuse

    [[nodiscard]] static constexpr std::size_t index(std::size_t hop, std::size_t bin) noexcept {
        return hop * Bins + bin;
    }
    [[nodiscard]] std::int8_t* row(std::size_t hop) noexcept { return data + hop * Bins; }
};
static_assert(rf::core::OverwriteRecord<EventTensor>);
static_assert(sizeof(EventTensor) == SpectrogramBins * SpectrogramHops);

struct CandidateEvent {
    std::uint64_t     event_id;
    TimestampNs       timestamp_ns; // event publication, not sample time
    WallNs            wall_ns;       // event publication wall time
    CaptureMetadata   source_meta;
    SpectralCandidate detection;
    TensorQuant       quant;
    std::uint32_t     tensor_valid_hops;   // rows of `tensor` actually written
    EventTensor       tensor;
};
static_assert(rf::core::OverwriteRecord<CandidateEvent>);
static_assert(sizeof(CandidateEvent) % CacheLineSize == 0,
              "CandidateEvent must tile cache lines for ObjectPool");

} // namespace rf::signal
