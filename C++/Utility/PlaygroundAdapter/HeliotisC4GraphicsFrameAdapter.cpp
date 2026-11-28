#include "HeliotisC4GraphicsFrameAdapter.h"

#include "engine/GraphicsFrameContext.h"
#include "engine/detail/GraphicsWriteLayout.h"
#include "graphics/data/GraphicsRangeFrameWriter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

// C4Utility delivers one source and one region, so every resource of a
// frame forms one cohort. The id scheme matches the GenTL adapters:
// "<kind>.<source>:<region>.<part index>".
constexpr const char* kResourceGroup = "0:0";

constexpr std::size_t kIntMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());

[[nodiscard]] std::string resourceId(const char* kind, const std::size_t partIndex)
{
    return std::string(kind) + "." + kResourceGroup + "." + std::to_string(partIndex);
}

[[nodiscard]] std::optional<std::size_t> findPartIndex(
    const heliotis::Frame& frame,
    const heliotis::FramePartKind kind) noexcept
{
    const auto it = std::find_if(frame.parts.cbegin(), frame.parts.cend(), [kind](const auto& part) {
        return part.kind == kind;
    });
    if (it == frame.parts.cend()) return std::nullopt;
    return static_cast<std::size_t>(std::distance(frame.parts.cbegin(), it));
}

template <typename T>
[[nodiscard]] float convertedSample(const T value, const double scale) noexcept
{
    return static_cast<float>(static_cast<double>(value) * scale);
}

/** Bytes of one sample element of the part's payload alternative. */
[[nodiscard]] std::size_t elementSize(const heliotis::FramePart& part) noexcept
{
    return std::visit([](const auto& values) {
        return sizeof(typename std::decay_t<decltype(values)>::value_type);
    }, part.samples);
}

/**
 * How one part's payload is fitted onto a GraphicsEngine grid.
 *
 * The declared shape is kept whenever it can be represented: a short payload is
 * zero-filled to it and extra samples are ignored. Without a representable
 * declared shape the delivered samples form one row. `available` counts the
 * samples that exist and are copied; the rest of `expected` is zero-filled.
 */
struct Fit
{
    int width = 0;
    int height = 0;
    std::size_t expected = 0;
    std::size_t available = 0;
    bool oneRow = false;
};

/** @return The fitted grid, or no value when the part carries no bytes at all. */
[[nodiscard]] std::optional<Fit> fitPart(const heliotis::FramePart& part) noexcept
{
    const std::size_t count = part.sampleCount();
    if (count == 0U) return std::nullopt;
    Fit fit;
    const bool declared = part.width != 0U && part.height != 0U
        && part.width <= kIntMax && part.height <= kIntMax
        && static_cast<std::uint64_t>(part.width) * part.height <= kIntMax;
    if (declared) {
        fit.width = static_cast<int>(part.width);
        fit.height = static_cast<int>(part.height);
        fit.expected = static_cast<std::size_t>(part.width) * part.height;
    } else {
        fit.oneRow = true;
        fit.width = static_cast<int>((std::min)(count, kIntMax));
        fit.height = 1;
        fit.expected = static_cast<std::size_t>(fit.width);
    }
    fit.available = (std::min)(count, fit.expected);
    return fit;
}

using RangeReason = heliotis::HeliotisRangeRejection::Reason;

/** Per-frame anomaly recorder: fixed capacity, one record per part and reason. */
struct AnomalyContext
{
    heliotis::HeliotisFrameAnomalies* list = nullptr;
    std::size_t partCount = 0;
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, heliotis::HeliotisFrameAnomalies::reasonCount> noted{};

    void note(const RangeReason reason, const std::size_t partIndex, const int width, const int height,
              const std::uint64_t readableBytes, const std::uint64_t requiredBytes) noexcept
    {
        if (!list) return;
        const auto slot = static_cast<std::size_t>(reason);
        if (slot >= noted.size()) return;
        if (partIndex < 64U) {
            const std::uint64_t bit = std::uint64_t{1} << partIndex;
            if ((noted[slot] & bit) != 0U) return;
            noted[slot] |= bit;
        }
        heliotis::HeliotisRangeRejection evidence;
        evidence.reason = reason;
        evidence.partCount = static_cast<std::uint32_t>(partCount);
        evidence.affectedPart = static_cast<std::uint32_t>(partIndex);
        evidence.width = width > 0 ? static_cast<std::uint32_t>(width) : 0U;
        evidence.height = height > 0 ? static_cast<std::uint32_t>(height) : 0U;
        evidence.frameSequence = sequence;
        evidence.readableBytes = readableBytes;
        evidence.requiredBytes = requiredBytes;
        list->add(evidence);
    }

    /** Counts a short payload, extra samples or an unusable declared shape of one part. */
    void noteFit(const heliotis::FramePart& part, const std::size_t partIndex, const Fit& fit) noexcept
    {
        const std::uint64_t size = elementSize(part);
        const std::uint64_t readable = part.sampleBytes();
        const std::uint64_t required = static_cast<std::uint64_t>(fit.expected) * size;
        if (fit.oneRow) {
            note(RangeReason::InvalidDimensions, partIndex, fit.width, fit.height, readable, required);
        } else if (part.sampleCount() < fit.expected) {
            note(RangeReason::TruncatedData, partIndex, fit.width, fit.height, readable, required);
        } else if (part.sampleCount() > fit.expected) {
            note(RangeReason::InvalidLayout, partIndex, fit.width, fit.height, readable, required);
        }
    }
};

/** True when any converted available sample is not finite, so the channel needs a validity mask. */
[[nodiscard]] bool hasNonFiniteSamples(const heliotis::FramePart& part, const std::size_t limit)
{
    return std::visit([&](const auto& values) {
        const std::size_t count = (std::min)(values.size(), limit);
        for (std::size_t index = 0; index < count; ++index)
        {
            if (!std::isfinite(convertedSample(values[index], part.sampleScale)))
            {
                return true;
            }
        }
        return false;
    }, part.samples);
}

/** Writes the converted available samples and zero-fills the rest of the declared shape. */
void writeConvertedSamples(const heliotis::FramePart& part,
                           const Fit& fit,
                           float* destination,
                           std::uint8_t* mask)
{
    std::visit([&](const auto& values) {
        for (std::size_t index = 0; index < fit.available; ++index)
        {
            const float value = convertedSample(values[index], part.sampleScale);
            destination[index] = value;
            if (mask != nullptr)
            {
                mask[index] = std::isfinite(value) ? 1U : 0U;
            }
        }
    }, part.samples);
    for (std::size_t index = fit.available; index < fit.expected; ++index)
    {
        destination[index] = 0.0F;
        if (mask != nullptr) mask[index] = 1U;
    }
}

/** Copies the available raw 16-bit samples in host order and zero-fills the rest. */
void writeRawSamples(const heliotis::FramePart& part, const Fit& fit, std::uint16_t* destination)
{
    if (destination == nullptr) return;
    const auto* values = std::get_if<std::vector<std::uint16_t>>(&part.samples);
    if (values != nullptr && fit.available != 0U) {
        std::memcpy(destination, values->data(), fit.available * sizeof(std::uint16_t));
    }
    if (fit.available < fit.expected) {
        std::memset(destination + fit.available, 0, (fit.expected - fit.available) * sizeof(std::uint16_t));
    }
}

[[nodiscard]] bool holdsRawSamples(const heliotis::FramePart& part) noexcept
{
    return std::holds_alternative<std::vector<std::uint16_t>>(part.samples);
}

/** Records only the first failed rule; later failures on the same frame are ignored. */
[[nodiscard]] std::nullopt_t rejectRange(
    heliotis::HeliotisRangeRejection* rejection,
    const RangeReason reason,
    const std::uint32_t partIndex,
    const std::size_t partCount,
    const int width,
    const int height,
    const std::uint64_t sequence,
    const std::uint64_t readableBytes,
    const std::uint64_t requiredBytes) noexcept
{
    if (rejection && rejection->reason == RangeReason::None) {
        rejection->reason = reason;
        rejection->partCount = static_cast<std::uint32_t>(partCount);
        rejection->affectedPart = partIndex;
        rejection->width = width > 0 ? static_cast<std::uint32_t>(width) : 0U;
        rejection->height = height > 0 ? static_cast<std::uint32_t>(height) : 0U;
        rejection->frameSequence = sequence;
        rejection->readableBytes = readableBytes;
        rejection->requiredBytes = requiredBytes;
    }
    return std::nullopt;
}

/** Maps the real range writer failure and always retains its calculated block bytes. */
[[nodiscard]] std::nullopt_t rejectRangeWriter(
    heliotis::HeliotisRangeRejection* rejection,
    const GraphicsRangeFrameWriter& writer,
    const std::uint32_t partIndex,
    const std::size_t partCount,
    const int width,
    const int height,
    const std::uint64_t sequence,
    const std::uint64_t readableBytes,
    const std::uint64_t requiredBytes) noexcept
{
    if (!rejection || rejection->reason != RangeReason::None) return std::nullopt;
    rejection->requestedStorageBytes = static_cast<std::uint64_t>(writer.blockBytes());
    RangeReason reason = RangeReason::InvalidLayout;
    switch (writer.failure()) {
    case GraphicsRangeFrameWriterFailure::InvalidDimensions: reason = RangeReason::InvalidDimensions; break;
    case GraphicsRangeFrameWriterFailure::SizeLimitExceeded: reason = RangeReason::SizeLimitExceeded; break;
    case GraphicsRangeFrameWriterFailure::StorageRefused: reason = RangeReason::StorageRefused; break;
    case GraphicsRangeFrameWriterFailure::None: break;
    }
    return rejectRange(rejection, reason, partIndex, partCount, width, height,
                       sequence, readableBytes, requiredBytes);
}

using ImageReason = heliotis::HeliotisImageRejection::Reason;

/** Maps the real GraphicsImageWriter failure; rules with no host equivalent stay unreported. */
[[nodiscard]] ImageReason imageRejectionReason(const GraphicsImageFailure failure) noexcept
{
    switch (failure) {
    case GraphicsImageFailure::MissingBytes: return ImageReason::MissingBytes;
    case GraphicsImageFailure::InvalidDimensions: return ImageReason::InvalidDimensions;
    case GraphicsImageFailure::UnsupportedFormat: return ImageReason::UnsupportedFormat;
    case GraphicsImageFailure::InvalidStride: return ImageReason::InvalidStride;
    case GraphicsImageFailure::StrideBelowMinimum: return ImageReason::StrideBelowMinimum;
    case GraphicsImageFailure::ExtentOverflow: return ImageReason::ExtentOverflow;
    case GraphicsImageFailure::TruncatedBytes: return ImageReason::TruncatedBytes;
    case GraphicsImageFailure::OddWidth: return ImageReason::OddWidth;
    case GraphicsImageFailure::UnsupportedConversion: return ImageReason::UnsupportedConversion;
    case GraphicsImageFailure::StorageRefused: return ImageReason::StorageRefused;
    case GraphicsImageFailure::None:
    case GraphicsImageFailure::ExcessBytes: return ImageReason::None;
    }
    return ImageReason::None;
}

/** Records the first failed image writer rule with the source layout actually attempted. */
[[nodiscard]] std::nullopt_t rejectImage(
    heliotis::HeliotisImageRejection* rejection,
    const ImageReason reason,
    const GraphicsImageWriterSource& source) noexcept
{
    if (rejection && rejection->reason == ImageReason::None) {
        rejection->reason = reason;
        rejection->conversion = heliotis::HeliotisImageRejection::Conversion::Copy;
        rejection->width = source.width > 0 ? static_cast<std::uint32_t>(source.width) : 0U;
        rejection->height = source.height > 0 ? static_cast<std::uint32_t>(source.height) : 0U;
        rejection->frameSequence = source.frameSequence;
        rejection->effectiveStride = source.stride > 0 ? static_cast<std::uint64_t>(source.stride) : 0U;
        rejection->minimumRowBytes = static_cast<std::uint64_t>(
            GraphicsImageValidation::minimumRowBytes(source.pixelFormat, source.width));
        rejection->readableBytes = static_cast<std::uint64_t>(source.size);
        rejection->requiredBytes = rejection->effectiveStride * rejection->height;
        rejection->directInput = false;
    }
    return std::nullopt;
}

[[nodiscard]] std::uint8_t graphicsBitDepth(const heliotis::FramePart& part) noexcept
{
    return part.bitsPerSample == 0
        ? 0
        : static_cast<std::uint8_t>(std::min<std::uint16_t>(part.bitsPerSample, 255U));
}

[[nodiscard]] std::string lowerCase(std::string value);

[[nodiscard]] MeasurementFieldDescriptor fieldDescriptor(
    const heliotis::FramePart& part,
    const char* fallbackName,
    const MeasurementValueDomain domain)
{
    const std::string displayName = part.name.empty() ? std::string(fallbackName) : part.name;
    return {
        std::string("heliotis.") + lowerCase(displayName),
        displayName,
        "",
        domain,
        MeasurementSampleKind::GridSample,
        graphicsBitDepth(part)};
}


[[nodiscard]] std::string lowerCase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] std::optional<GraphicsLengthUnit> distanceUnit(const std::string& distanceUnit) noexcept
{
    const std::string value = lowerCase(distanceUnit);
    if (value == "nm") return GraphicsLengthUnit::Nanometer;
    if (value == "um") return GraphicsLengthUnit::Micrometer;
    if (value == "mm") return GraphicsLengthUnit::Millimeter;
    return std::nullopt;
}

[[nodiscard]] bool isRectifiedOutput(const std::string& outputMode) noexcept
{
    return lowerCase(outputMode) == "rectifiedc";
}

[[nodiscard]] bool hasUsableScan3dGeometry(const heliotis::Scan3dGeometry& geometry) noexcept
{
    if (!distanceUnit(geometry.distanceUnit).has_value()
        || !std::isfinite(geometry.zScale)
        || !std::isfinite(geometry.zOffset))
    {
        return false;
    }
    return !isRectifiedOutput(geometry.outputMode)
        || (std::isfinite(geometry.xScale)
            && std::isfinite(geometry.yScale)
            && std::isfinite(geometry.xOffset)
            && std::isfinite(geometry.yOffset));
}

/** Returns the PFNC Mono code that describes the sensor bit depth. */
[[nodiscard]] int monoPfnc(const std::uint8_t bits) noexcept
{
    switch (bits) {
    case 8: return 0x01080001;
    case 10: return 0x01100003;
    case 12: return 0x01100005;
    case 14: return 0x01100025;
    default: return 0x01100007;
    }
}

/** One image part, planned: the destination layout and the bytes that really exist. */
struct ImageJob
{
    std::size_t partIndex = 0;
    GraphicsImageWriterSource source;
    GraphicsImageWriteLayout layout;
    /** Source bytes that exist behind `source.data`; the rest of the layout is zero-filled. */
    std::size_t availableBytes = 0;
};

/**
 * Plans one part as a Mono8 or Mono16 image, without storage.
 *
 * @param part Deep-owned C4 part.
 * @param partIndex Ordinal in the frame, used for anomaly evidence.
 * @param sequence Producer frame counter stored as image metadata.
 * @param anomalies Receives fitted deviations; may be a recorder without a list.
 * @return The job, or no value for a part with no bytes or a size the image layout cannot hold.
 * @note Integer samples keep their unscaled sensor codes; Mono16 rows use the little-endian host
 *       order of the copied C4 samples and 8-bit parts saturate into Mono8. Floating and raw-byte
 *       payloads have no sample format that fits, so their bytes publish as raw Mono8 with the
 *       row length in bytes. A short payload keeps the declared shape; its missing tail is zero.
 */
[[nodiscard]] std::optional<ImageJob> planPartImage(
    const heliotis::FramePart& part,
    const std::size_t partIndex,
    const std::uint64_t sequence,
    AnomalyContext& anomalies,
    heliotis::HeliotisImageRejection* rejection)
{
    const auto fit = fitPart(part);
    if (!fit) {
        anomalies.note(RangeReason::MissingData, partIndex, static_cast<int>((std::min)(std::size_t{part.width}, kIntMax)),
                       static_cast<int>((std::min)(std::size_t{part.height}, kIntMax)), 0U, 0U);
        return std::nullopt;
    }
    anomalies.noteFit(part, partIndex, *fit);

    ImageJob job;
    job.partIndex = partIndex;
    GraphicsImageWriterSource& source = job.source;
    source.frameSequence = sequence;
    const auto* values = std::get_if<std::vector<std::uint16_t>>(&part.samples);
    if (values != nullptr) {
        const std::uint8_t bits = part.bitsPerSample == 0
            ? std::uint8_t{16}
            : std::min<std::uint8_t>(part.bitsPerSample, std::uint8_t{16});
        const bool eightBit = bits <= 8;
        if (static_cast<std::size_t>(fit->width) > kIntMax / 2U) {
            anomalies.note(RangeReason::SizeLimitExceeded, partIndex, fit->width, fit->height,
                           part.sampleBytes(), static_cast<std::uint64_t>(fit->expected) * 2U);
            return std::nullopt;
        }
        // The writer layout is planned for the full declared shape; only the existing bytes are read.
        source.data = reinterpret_cast<const std::uint8_t*>(values->data());
        source.size = fit->expected * sizeof(std::uint16_t);
        source.width = fit->width;
        source.height = fit->height;
        source.stride = fit->width * 2;
        source.pixelFormat = GraphicsImagePixelFormat::Mono16;
        source.operation = eightBit
            ? GraphicsImageWriterOperation::NarrowMono16To8
            : GraphicsImageWriterOperation::Copy;
        source.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(
            eightBit ? GraphicsImagePixelFormat::Mono8 : GraphicsImagePixelFormat::Mono16);
        source.sourceFormat = monoPfnc(eightBit ? std::uint8_t{8} : bits);
        source.declaredMonoSourceBits = eightBit || bits >= 16 ? std::uint8_t{0} : bits;
        source.bitAlignment = source.declaredMonoSourceBits != 0
            ? GraphicsImageBitAlignment::LeastSignificant
            : GraphicsImageBitAlignment::Packed;
        job.availableBytes = fit->available * sizeof(std::uint16_t);
    } else {
        const std::size_t size = elementSize(part);
        if (static_cast<std::size_t>(fit->width) > kIntMax / size) {
            anomalies.note(RangeReason::SizeLimitExceeded, partIndex, fit->width, fit->height,
                           part.sampleBytes(), static_cast<std::uint64_t>(fit->expected) * size);
            return std::nullopt;
        }
        const int rowBytes = fit->width * static_cast<int>(size);
        source.data = std::visit([](const auto& payload) {
            return reinterpret_cast<const std::uint8_t*>(payload.data());
        }, part.samples);
        source.size = fit->expected * size;
        source.width = rowBytes;
        source.height = fit->height;
        source.stride = rowBytes;
        source.pixelFormat = GraphicsImagePixelFormat::Mono8;
        source.operation = GraphicsImageWriterOperation::Copy;
        source.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(GraphicsImagePixelFormat::Mono8);
        source.sourceFormat = monoPfnc(8);
        source.bitAlignment = GraphicsImageBitAlignment::Packed;
        job.availableBytes = fit->available * size;
        if (size != sizeof(std::uint8_t)) {
            // Floating samples have no integer image format: the bytes publish as raw Mono8.
            anomalies.note(RangeReason::UnsupportedFormat, partIndex, fit->width, fit->height,
                           part.sampleBytes(), static_cast<std::uint64_t>(fit->expected) * size);
        }
    }

    GraphicsImageFailure failure = GraphicsImageFailure::None;
    const auto plan = planGraphicsImageWrite(source, &failure);
    if (!plan) {
        const ImageReason reason = imageRejectionReason(failure);
        if (reason != ImageReason::None) static_cast<void>(rejectImage(rejection, reason, source));
        return std::nullopt;
    }
    job.layout = plan.value();
    return job;
}

/**
 * Copies the existing bytes of a planned image into storage and zero-fills the rest.
 *
 * Never reads past `job.availableBytes`; this replaces the strict image writer, which refuses a
 * source that is shorter than the declared layout.
 */
[[nodiscard]] GraphicsImage writeFittedImage(
    GraphicsFrameBuilder& builder, const ImageJob& job, GraphicsImageFailure* failure)
{
    const GraphicsImageWriteLayout& layout = job.layout;
    const GraphicsImageWriterSource& source = job.source;
    auto reserved = builder.reserve<std::uint8_t>(layout.bytes, layout.alignment);
    if (!reserved) {
        if (failure) *failure = GraphicsImageFailure::StorageRefused;
        return {};
    }
    GraphicsWriteRegion<std::uint8_t> region = std::move(reserved).value();
    std::uint8_t* const destination = region.data();
    if (source.operation == GraphicsImageWriterOperation::NarrowMono16To8) {
        const std::size_t width = static_cast<std::size_t>(source.width);
        const std::size_t height = static_cast<std::size_t>(source.height);
        const std::size_t stride = static_cast<std::size_t>(layout.strideBytes);
        const std::size_t availableSamples = job.availableBytes / sizeof(std::uint16_t);
        for (std::size_t row = 0; row < height; ++row) {
            std::uint8_t* outputRow = destination + row * stride;
            const std::size_t first = row * width;
            const std::size_t samples = first < availableSamples
                ? (std::min)(width, availableSamples - first) : 0U;
            for (std::size_t index = 0; index < samples; ++index) {
                std::uint16_t value = 0U;
                std::memcpy(&value, source.data + (first + index) * sizeof(value), sizeof(value));
                outputRow[index] = static_cast<std::uint8_t>(std::min<std::uint16_t>(value, 255U));
            }
            if (samples < stride) std::memset(outputRow + samples, 0, stride - samples);
        }
    } else {
        const std::size_t copied = (std::min)(job.availableBytes, layout.bytes);
        if (copied != 0U) std::memcpy(destination, source.data, copied);
        if (copied < layout.bytes) std::memset(destination + copied, 0, layout.bytes - copied);
    }
    GraphicsImage image;
    image.pixels = builder.seal(std::move(region));
    image.width = source.width;
    image.height = source.height;
    image.stride = layout.strideBytes;
    image.bitsPerPixel = source.bitsPerPixel;
    image.declaredMonoSourceBits = source.declaredMonoSourceBits;
    image.sourceFormat = source.sourceFormat;
    image.pixelFormat = layout.format;
    image.bitAlignment = source.bitAlignment;
    image.frameSequence = source.frameSequence;
    if (failure) *failure = GraphicsImageFailure::None;
    return image;
}

/** Planned range: every non-sample field plus the step that writes the samples. */
struct PlannedRange
{
    GraphicsRangeFrameLayout layout;
    RangeFrame fields;
    std::size_t partIndex = 0;
    std::size_t partCount = 0;
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    std::uint64_t readableBytes = 0;
    std::uint64_t requiredBytes = 0;
    const heliotis::FramePart* firstPart = nullptr;
    const heliotis::FramePart* rangePart = nullptr;
    const heliotis::FramePart* intensityPart = nullptr;
    const heliotis::FramePart* confidencePart = nullptr;
    Fit rangeFit;
    Fit intensityFit;
    Fit confidenceFit;
};

/**
 * Attaches an auxiliary part as a range channel when its declared shape matches the grid.
 *
 * A part with a different shape cannot be a channel of this grid; it is counted and left to
 * publish as its own image. A payload that is short or longer than the grid is fitted to it.
 */
[[nodiscard]] const heliotis::FramePart* attachChannel(
    const heliotis::Frame& frame,
    const heliotis::FramePartKind kind,
    const Fit& grid,
    AnomalyContext& anomalies,
    Fit& channelFit)
{
    const auto index = findPartIndex(frame, kind);
    if (!index) return nullptr;
    const heliotis::FramePart& part = frame.parts[*index];
    const auto fit = fitPart(part);
    if (!fit) return nullptr;
    if (part.width != static_cast<std::uint32_t>(grid.width)
        || part.height != static_cast<std::uint32_t>(grid.height)) {
        anomalies.note(RangeReason::InvalidLayout, *index, grid.width, grid.height, part.sampleBytes(),
                       static_cast<std::uint64_t>(grid.expected) * elementSize(part));
        return nullptr;
    }
    anomalies.noteFit(part, *index, *fit);
    channelFit = *fit;
    return &part;
}

/**
 * Plans the organized range for the typed Range part or a raw preview.
 *
 * @param frame Source frame whose parts stay owned by the caller.
 * @param request Requested components and auxiliary channels.
 * @return The range plan, or no value when no part carries any bytes or the grid is too large.
 */
[[nodiscard]] std::optional<PlannedRange> planRange(
    const heliotis::Frame& frame,
    const GraphicsFrameRequest& request,
    heliotis::HeliotisRangeRejection* rejection,
    AnomalyContext& anomalies)
{
    using heliotis::FramePart;
    using heliotis::FramePartKind;
    using heliotis::Scan3dGeometry;

    const std::optional<std::size_t> typedRangeIndex = findPartIndex(frame, FramePartKind::Range);
    std::optional<std::size_t> rangeIndex;
    bool rawPartPreview = true;
    if (typedRangeIndex && frame.parts[*typedRangeIndex].sampleCount() != 0U)
    {
        rangeIndex = typedRangeIndex;
        rawPartPreview = frame.scan3dGeometry.has_value()
            && !hasUsableScan3dGeometry(*frame.scan3dGeometry);
    }
    if (!rangeIndex)
    {
        for (std::size_t index = 0; index < frame.parts.size(); ++index)
        {
            if (frame.parts[index].sampleCount() != 0U)
            {
                rangeIndex = index;
                break;
            }
        }
    }
    if (!rangeIndex)
    {
        // Every part is empty: there is no data to fit.
        return rejectRange(rejection, RangeReason::MissingRange,
                           heliotis::HeliotisRangeRejection::noPart, frame.parts.size(),
                           0, 0, frame.sequence, 0U, 0U);
    }
    if (rangeIndex != typedRangeIndex) rawPartPreview = true;
    const FramePart& rangePart = frame.parts[*rangeIndex];
    const auto partIndex = static_cast<std::uint32_t>(*rangeIndex);
    const std::size_t partCount = frame.parts.size();

    const Fit rangeFit = *fitPart(rangePart);
    anomalies.noteFit(rangePart, *rangeIndex, rangeFit);
    rawPartPreview = rawPartPreview || rangeFit.oneRow;
    const int width = rangeFit.width;
    const int height = rangeFit.height;
    const std::size_t expected = rangeFit.expected;
    PlannedRange planned;
    planned.rangeFit = rangeFit;
    if (request.includeRangeAuxiliaryChannels)
    {
        planned.intensityPart = attachChannel(frame, FramePartKind::Intensity, rangeFit, anomalies,
                                              planned.intensityFit);
        planned.confidencePart = attachChannel(frame, FramePartKind::Confidence, rangeFit, anomalies,
                                               planned.confidenceFit);
    }
    const FramePart* intensityPart = planned.intensityPart;
    const FramePart* confidencePart = planned.confidencePart;

    GraphicsRangeFrameLayout layout;
    layout.width = width;
    layout.height = height;
    layout.channels.validMask = hasNonFiniteSamples(rangePart, rangeFit.available);
    layout.channels.rangeRaw = holdsRawSamples(rangePart);
    layout.channels.intensity = intensityPart != nullptr;
    layout.channels.intensityValidMask = intensityPart != nullptr
        && hasNonFiniteSamples(*intensityPart, planned.intensityFit.available);
    layout.channels.intensityRaw = intensityPart != nullptr && holdsRawSamples(*intensityPart);
    layout.channels.confidence = confidencePart != nullptr;
    layout.channels.confidenceValidMask = confidencePart != nullptr
        && hasNonFiniteSamples(*confidencePart, planned.confidenceFit.available);
    layout.channels.confidenceRaw = confidencePart != nullptr && holdsRawSamples(*confidencePart);

    const std::uint64_t readableBytes = rangePart.sampleBytes();
    const std::uint64_t requiredBytes = static_cast<std::uint64_t>(expected) * elementSize(rangePart);
    const auto rangeWritePlan = planGraphicsRangeWrite(layout);
    if (!rangeWritePlan) {
        return rejectRange(rejection, rangeWritePlan.error() == GraphicsFrameError::Capacity
            ? RangeReason::SizeLimitExceeded : RangeReason::InvalidDimensions,
            partIndex, partCount, width, height, frame.sequence, readableBytes, requiredBytes);
    }

    planned.layout = layout;
    planned.partIndex = *rangeIndex;
    planned.partCount = partCount;
    planned.width = width;
    planned.height = height;
    planned.sequence = frame.sequence;
    planned.readableBytes = readableBytes;
    planned.requiredBytes = requiredBytes;
    planned.firstPart = frame.parts.data();
    planned.rangePart = &rangePart;

    RangeFrame& range = planned.fields;
    range.width = width;
    range.height = height;
    range.rangeField = fieldDescriptor(
        rangePart,
        rawPartPreview ? "Raw Part" : "Range",
        rawPartPreview ? MeasurementValueDomain::Native : MeasurementValueDomain::Calibrated);
    range.rangeBits = graphicsBitDepth(rangePart);
    range.zScale = 1.0;
    range.zOffset = 0.0;
    range.xScale = std::numeric_limits<double>::quiet_NaN();
    range.yScale = std::numeric_limits<double>::quiet_NaN();
    range.xOffset = std::numeric_limits<double>::quiet_NaN();
    range.yOffset = std::numeric_limits<double>::quiet_NaN();
    if (!rawPartPreview && frame.scan3dGeometry)
    {
        // hasUsableScan3dGeometry() admitted the unit and finite Z/XY values.
        const Scan3dGeometry& geometry = *frame.scan3dGeometry;
        range.lengthUnit = *distanceUnit(geometry.distanceUnit);
        range.zScale = geometry.zScale;
        range.zOffset = geometry.zOffset;
        if (isRectifiedOutput(geometry.outputMode))
        {
            // RectifiedC is a UniformPhysical grid; GraphicsEngine derives the
            // point cloud and surface from it, as for GenTL RectifiedC.
            range.xScale = geometry.xScale;
            range.yScale = geometry.yScale;
            range.xOffset = geometry.xOffset;
            range.yOffset = geometry.yOffset;
        }
        else
        {
            // CalibratedC has physical Z and pixel X/Y. Do not invent a pitch.
            range.xyCoordinateMode = RangeFrameXYCoordinateMode::PixelGrid;
        }
    }
    else if (!rangeFit.oneRow)
    {
        // An organized grid without rectified XY is still a height map.
        // UniformPhysical with non-finite X/Y hides Surface3D.
        range.xyCoordinateMode = RangeFrameXYCoordinateMode::PixelGrid;
    }
    range.sensorType = rawPartPreview
        ? "Heliotis H8 (raw part preview)"
        : (frame.scan3dGeometry
            ? (range.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid
                ? "Heliotis H8 (CalibratedC pixel grid)"
                : "Heliotis H8")
            : "Heliotis H8 (raw range)");
    range.frameId = frame.frameId;
    if (intensityPart != nullptr)
    {
        range.intensityField = fieldDescriptor(
            *intensityPart,
            "Intensity",
            MeasurementValueDomain::Native);
        range.intensityBits = graphicsBitDepth(*intensityPart);
    }
    if (confidencePart != nullptr)
    {
        range.confidenceField = fieldDescriptor(
            *confidencePart,
            "Confidence",
            MeasurementValueDomain::Native);
        range.confidenceBits = graphicsBitDepth(*confidencePart);
    }
    return planned;
}

/** Writes a planned range through the builder and returns the complete range frame. */
[[nodiscard]] std::optional<RangeFrame> writeRange(
    PlannedRange planned,
    GraphicsFrameBuilder& builder,
    heliotis::HeliotisRangeRejection* rejection)
{
    GraphicsRangeFrameWriter writer(builder, planned.layout);
    const auto partIndex = static_cast<std::uint32_t>(planned.partIndex);
    if (!writer.isValid())
    {
        return rejectRangeWriter(rejection, writer, partIndex, planned.partCount, planned.width,
                                 planned.height, planned.sequence, planned.readableBytes,
                                 planned.requiredBytes);
    }
    const auto& channels = planned.layout.channels;
    writeConvertedSamples(*planned.rangePart, planned.rangeFit, writer.zData(), writer.validMaskData());
    if (channels.rangeRaw) writeRawSamples(*planned.rangePart, planned.rangeFit, writer.rangeRawData());
    if (planned.intensityPart) {
        writeConvertedSamples(*planned.intensityPart, planned.intensityFit,
                              writer.intensityData(), writer.intensityValidMaskData());
        if (channels.intensityRaw)
            writeRawSamples(*planned.intensityPart, planned.intensityFit, writer.intensityRawData());
    }
    if (planned.confidencePart) {
        writeConvertedSamples(*planned.confidencePart, planned.confidenceFit,
                              writer.confidenceData(), writer.confidenceValidMaskData());
        if (channels.confidenceRaw)
            writeRawSamples(*planned.confidencePart, planned.confidenceFit, writer.confidenceRawData());
    }
    auto samples = writer.takeFrame();
    RangeFrame range = std::move(planned.fields);
    range.zValues = std::move(samples.zValues);
    range.xValues = std::move(samples.xValues);
    range.yValues = std::move(samples.yValues);
    range.validMask = std::move(samples.validMask);
    range.intensityValidMask = std::move(samples.intensityValidMask);
    range.confidenceValidMask = std::move(samples.confidenceValidMask);
    range.intensity = std::move(samples.intensity);
    range.confidence = std::move(samples.confidence);
    range.rangeRaw = std::move(samples.rangeRaw);
    range.intensityRaw = std::move(samples.intensityRaw);
    range.confidenceRaw = std::move(samples.confidenceRaw);
    range.rgb = std::move(samples.rgb);
    if (!range.isValid())
    {
        return rejectRange(rejection, RangeReason::InvalidDimensions, partIndex, planned.partCount,
                           planned.width, planned.height, planned.sequence,
                           planned.readableBytes, planned.requiredBytes);
    }
    return range;
}

[[nodiscard]] std::string imageName(const heliotis::FramePart& part)
{
    if (!part.name.empty()) return part.name;
    switch (part.kind) {
    case heliotis::FramePartKind::Intensity: return "Intensity";
    case heliotis::FramePartKind::Confidence: return "Confidence";
    case heliotis::FramePartKind::Range: return "Range";
    case heliotis::FramePartKind::Unknown: break;
    }
    return "Raw Part";
}

} // namespace

namespace heliotis {

void HeliotisC4GraphicsFrameAdapter::setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    _contextSource = std::move(source);
}

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertFrameWithRejection(
    const Frame& frame,
    const GraphicsFrameRequest& request,
    HeliotisRangeRejection& rangeRejection,
    HeliotisImageRejection& imageRejection,
    HeliotisFrameAnomalies* anomalies) const
{
    rangeRejection = {};
    imageRejection = {};
    if (anomalies) *anomalies = {};
    return convertGraphicsFrame(frame, request, &rangeRejection, &imageRejection,
                                nullptr, nullptr, nullptr, anomalies);
}

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertForRun(
    const Frame& frame, const GraphicsFrameRequest& request,
    GraphicsFrameContextSource& source, const std::atomic<bool>& cancelled,
    GraphicsFrameError& storageError, HeliotisRangeRejection& rangeRejection,
    HeliotisImageRejection& imageRejection, HeliotisFrameAnomalies* anomalies) const
{
    rangeRejection = {};
    imageRejection = {};
    if (anomalies) *anomalies = {};
    storageError = GraphicsFrameError::None;
    return convertGraphicsFrame(frame, request, &rangeRejection, &imageRejection,
                                &source, &cancelled, &storageError, anomalies);
}

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertGraphicsFrame(
    const Frame& frame,
    const GraphicsFrameRequest& request,
    HeliotisRangeRejection* rangeRejection,
    HeliotisImageRejection* imageRejection,
    GraphicsFrameContextSource* runSource, const std::atomic<bool>* cancelled,
    GraphicsFrameError* storageError,
    HeliotisFrameAnomalies* anomalies) const
{
    std::shared_ptr<GraphicsFrameContextSource> source;
    if (!runSource) {
        std::lock_guard<std::mutex> lock(_storageMutex);
        source = _contextSource;
        runSource = source.get();
    }
    if (!runSource || (cancelled && cancelled->load(std::memory_order_acquire))) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    const bool rangeRequested =
        hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range)
        || hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::PointCloud)
        || hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Surface);
    const bool imageRequested =
        hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Image);
    if (frame.parts.empty() || (!rangeRequested && !imageRequested))
    {
        return std::nullopt;
    }

    // Deviations seen while copying the SDK buffer travel with the frame; this conversion adds its own.
    AnomalyContext anomalyContext;
    anomalyContext.list = anomalies;
    anomalyContext.partCount = frame.parts.size();
    anomalyContext.sequence = frame.sequence;
    if (anomalies) {
        anomalies->merge(frame.anomalies);
        for (std::size_t index = 0; index < anomalies->count; ++index)
            anomalies->entries[index].frameSequence = frame.sequence;
    }

    std::size_t sampleBytes = 0;
    std::optional<PlannedRange> rangePlan;
    if (rangeRequested)
    {
        rangePlan = planRange(frame, request, rangeRejection, anomalyContext);
        if (rangePlan) {
            const auto plan = planGraphicsRangeWrite(rangePlan->layout);
            if (!plan || !appendAligned(sampleBytes, plan.value().totalBytes, plan.value().alignment)) {
                static_cast<void>(rejectRange(rangeRejection, RangeReason::SizeLimitExceeded,
                    static_cast<std::uint32_t>(rangePlan->partIndex), rangePlan->partCount,
                    rangePlan->width, rangePlan->height, rangePlan->sequence,
                    rangePlan->readableBytes, rangePlan->requiredBytes));
                rangePlan.reset();
                sampleBytes = 0;
            }
        }
    }

    // A part that cannot be planned or added to the frame total is skipped and recorded;
    // the other parts still publish.
    std::vector<ImageJob> imageJobs;
    if (imageRequested) {
        imageJobs.reserve(frame.parts.size());
        for (std::size_t partIndex = 0; partIndex < frame.parts.size(); ++partIndex) {
            const auto& part = frame.parts[partIndex];
            bool imagePart = false;
            switch (part.kind) {
            case FramePartKind::Intensity:
                // A floating intensity part attached to the range grid is that range's channel.
                imagePart = !(rangePlan && rangePlan->intensityPart == &part
                              && !holdsRawSamples(part));
                break;
            case FramePartKind::Confidence:
                // A confidence part is an image only when it could not become a range channel.
                imagePart = rangeRequested && request.includeRangeAuxiliaryChannels
                    && !(rangePlan && rangePlan->confidencePart == &part);
                break;
            case FramePartKind::Range:
                imagePart = rangeRequested && !(rangePlan && rangePlan->rangePart == &part);
                break;
            case FramePartKind::Unknown:
                imagePart = !(rangePlan && rangePlan->rangePart == &part);
                break;
            }
            if (!imagePart) continue;
            auto job = planPartImage(part, partIndex, frame.sequence, anomalyContext, imageRejection);
            if (!job) continue;
            if (!appendAligned(sampleBytes, job->layout.bytes, job->layout.alignment)) {
                static_cast<void>(rejectImage(imageRejection, ImageReason::ExtentOverflow, job->source));
                continue;
            }
            imageJobs.push_back(*job);
        }
    }
    if (!rangePlan && imageJobs.empty()) return std::nullopt;
    if (cancelled && cancelled->load(std::memory_order_acquire)) return std::nullopt;
    auto context = runSource->beginFrame(sampleBytes);
    if (!context) {
        if (storageError) *storageError = context.error();
        if (context.error() == GraphicsFrameError::Disconnected ||
            context.error() == GraphicsFrameError::NoReadyCredit) return std::nullopt;
        if (rangePlan && rangeRejection) {
            if (rangeRejection->reason == RangeReason::None)
                rangeRejection->requestedStorageBytes = sampleBytes;
            static_cast<void>(rejectRange(rangeRejection, RangeReason::StorageRefused,
                static_cast<std::uint32_t>(rangePlan->partIndex), rangePlan->partCount,
                rangePlan->width, rangePlan->height, rangePlan->sequence,
                rangePlan->readableBytes, rangePlan->requiredBytes));
        }
        if (!imageJobs.empty()) static_cast<void>(rejectImage(imageRejection,
            ImageReason::StorageRefused, imageJobs.front().source));
        return std::nullopt;
    }
    if (cancelled && cancelled->load(std::memory_order_acquire)) return std::nullopt;
    auto frameContext = std::move(context).value();
    GraphicsFrameBuilder frameBuilder(frameContext);
    auto& result = frameBuilder.content();
    result.metadata.sourceName = "Heliotis C4";
    result.metadata.frameId = frame.frameId;
    result.metadata.frameIndex = frame.sequence;
    std::size_t imageOrdinal = 0;
    for (const ImageJob& job : imageJobs)
    {
        const auto& part = frame.parts[job.partIndex];
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        auto image = writeFittedImage(frameBuilder, job, &failure);
        if (!image.isValid()) {
            // One image that cannot be written never removes the other parts.
            ImageReason reason = imageRejectionReason(failure);
            if (reason == ImageReason::None) reason = ImageReason::StorageRefused;
            static_cast<void>(rejectImage(imageRejection, reason, job.source));
            continue;
        }
        std::string name = imageName(part);
        std::size_t duplicates = 0;
        for (const ImageJob& other : imageJobs) {
            if (imageName(frame.parts[other.partIndex]) == name) ++duplicates;
        }
        if (duplicates > 1) name += " " + std::to_string(imageOrdinal + 1);
        ++imageOrdinal;
        result.addImage(resourceId("image", job.partIndex), std::move(image), name, kResourceGroup);
    }
    if (rangePlan)
    {
        auto range = writeRange(*rangePlan, frameBuilder, rangeRejection);
        if (range)
        {
            result.addRange(resourceId("range", rangePlan->partIndex), std::move(*range), "Range", kResourceGroup);
        }
    }

    if (cancelled && cancelled->load(std::memory_order_acquire)) return std::nullopt;
    auto published = std::move(frameBuilder).publish();
    if (!published)
    {
        if (storageError) *storageError = published.error();
        if (rangePlan && rangeRejection)
        {
            const RangeReason reason = published.error() == GraphicsFrameError::InvalidInput
                ? RangeReason::InvalidLayout
                : RangeReason::StorageRefused;
            static_cast<void>(rejectRange(rangeRejection, reason,
                static_cast<std::uint32_t>(rangePlan->partIndex), rangePlan->partCount,
                rangePlan->width, rangePlan->height, rangePlan->sequence,
                rangePlan->readableBytes, rangePlan->requiredBytes));
        }
        if (!imageJobs.empty())
        {
            const ImageReason reason = published.error() == GraphicsFrameError::InvalidInput
                ? ImageReason::InvalidDimensions
                : ImageReason::StorageRefused;
            static_cast<void>(rejectImage(imageRejection, reason, imageJobs.front().source));
        }
        return std::nullopt;
    }
    const bool rangeRetained = !published.value().ranges().empty();
    const bool imageRetained = !published.value().images().empty();
    if (rangeRejection) {
        rangeRejection->rangeRetained = rangeRetained;
        rangeRejection->imageRetained = imageRetained;
    }
    if (anomalies) {
        for (std::size_t index = 0; index < anomalies->count; ++index) {
            anomalies->entries[index].rangeRetained = rangeRetained;
            anomalies->entries[index].imageRetained = imageRetained;
        }
    }
    return std::move(published).value();
}

} // namespace heliotis
