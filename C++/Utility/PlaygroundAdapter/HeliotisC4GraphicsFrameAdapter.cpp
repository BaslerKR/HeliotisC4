#include "HeliotisC4GraphicsFrameAdapter.h"

#include "DeviceRuntime/Contract/FrameStorage.h"
#include "graphics/data/GraphicsRangeFrameWriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace {

// C4Utility delivers one source and one region, so every resource of a
// frame forms one cohort. The id scheme matches the GenTL adapters:
// "<kind>.<source>:<region>.<part index>".
constexpr const char* kResourceGroup = "0:0";

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

[[nodiscard]] const heliotis::FramePart* findPart(
    const heliotis::Frame& frame,
    const heliotis::FramePartKind kind) noexcept
{
    const auto index = findPartIndex(frame, kind);
    return index ? &frame.parts[*index] : nullptr;
}

[[nodiscard]] bool hasCompatibleDimensions(
    const heliotis::FramePart& part,
    const int width,
    const int height) noexcept
{
    return part.width == static_cast<std::uint32_t>(width)
        && part.height == static_cast<std::uint32_t>(height);
}

template <typename T>
[[nodiscard]] float convertedSample(const T value, const double scale) noexcept
{
    return static_cast<float>(static_cast<double>(value) * scale);
}

/** True when any converted sample is not finite, so the channel needs a validity mask. */
[[nodiscard]] bool hasNonFiniteSamples(const heliotis::FramePart& part)
{
    return std::visit([&part](const auto& values) {
        for (const auto value : values)
        {
            if (!std::isfinite(convertedSample(value, part.sampleScale)))
            {
                return true;
            }
        }
        return false;
    }, part.samples);
}

void writeConvertedSamples(const heliotis::FramePart& part,
                           float* destination,
                           std::uint8_t* mask)
{
    std::visit([&](const auto& values) {
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            const float value = convertedSample(values[index], part.sampleScale);
            destination[index] = value;
            if (mask != nullptr)
            {
                mask[index] = std::isfinite(value) ? 1U : 0U;
            }
        }
    }, part.samples);
}

[[nodiscard]] std::size_t sampleCount(const heliotis::FramePart& part) noexcept
{
    return std::visit([](const auto& values) { return values.size(); }, part.samples);
}

[[nodiscard]] bool hasRepresentableSamples(const heliotis::FramePart& part) noexcept
{
    return part.isValid()
        && sampleCount(part) != 0
        && sampleCount(part) <= static_cast<std::size_t>((std::numeric_limits<int>::max)());
}

[[nodiscard]] bool hasRepresentableDimensions(const heliotis::FramePart& part) noexcept
{
    return part.width != 0
        && part.height != 0
        && part.width <= static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
        && part.height <= static_cast<std::uint32_t>((std::numeric_limits<int>::max)());
}

/** Bytes carried by a part's own sample vector, independent of its declared shape. */
[[nodiscard]] std::uint64_t sampleBytes(const heliotis::FramePart& part) noexcept
{
    const std::uint64_t sampleSize = std::holds_alternative<std::vector<std::uint16_t>>(part.samples)
        ? sizeof(std::uint16_t) : sizeof(double);
    return static_cast<std::uint64_t>(sampleCount(part)) * sampleSize;
}

[[nodiscard]] std::uint64_t gridBytes(const heliotis::FramePart& part, const int width, const int height) noexcept
{
    if (width <= 0 || height <= 0) return 0U;
    const auto samples = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    const std::uint64_t sampleSize = std::holds_alternative<std::vector<std::uint16_t>>(part.samples)
        ? sizeof(std::uint16_t) : sizeof(double);
    return samples <= (std::numeric_limits<std::uint64_t>::max)() / sampleSize
        ? samples * sampleSize : 0U;
}

using RangeReason = heliotis::HeliotisRangeRejection::Reason;

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
    case GraphicsRangeFrameWriterFailure::MisalignedStorage: reason = RangeReason::StorageMisaligned; break;
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

/**
 * Describes how one integer intensity part becomes a Mono8 or Mono16 image, without storage.
 *
 * @param part Deep-owned C4 part with integer samples.
 * @param sequence Producer frame counter stored as image metadata.
 * @return The writer source, or no value for floating, empty, oversized or refused parts.
 * @note Samples keep their unscaled sensor codes. Mono16 rows use the
 *       little-endian host order of the copied C4 samples. A source the image
 *       writer would refuse for a layout reason is recorded in @p rejection
 *       here; only a storage refusal is possible later.
 */
[[nodiscard]] std::optional<GraphicsImageWriterSource> planIntensityImage(
    const heliotis::FramePart& part,
    const std::uint64_t sequence,
    heliotis::HeliotisImageRejection* rejection)
{
    if (!hasRepresentableSamples(part) || !hasRepresentableDimensions(part)) return std::nullopt;
    const auto* values = std::get_if<std::vector<std::uint16_t>>(&part.samples);
    if (!values) return std::nullopt;

    const std::uint8_t bits = part.bitsPerSample == 0
        ? std::uint8_t{16}
        : std::min<std::uint8_t>(part.bitsPerSample, std::uint8_t{16});
    const bool eightBit = bits <= 8;
    const std::size_t pixelCount = static_cast<std::size_t>(part.width) * part.height;
    if (values->size() > pixelCount) return std::nullopt;
    if (part.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) / 2U)
    {
        return std::nullopt;
    }

    // The writer copies or narrows the sample bytes once into the planned storage.
    GraphicsImageWriterSource source;
    source.data = reinterpret_cast<const std::uint8_t*>(values->data());
    source.size = values->size() * sizeof(std::uint16_t);
    source.width = static_cast<int>(part.width);
    source.height = static_cast<int>(part.height);
    source.stride = static_cast<int>(part.width) * 2;
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
    source.frameSequence = sequence;

    // A storage that always refuses validates the layout without copying a byte: any failure other
    // than its own refusal is a layout rule of the real writer.
    GraphicsImageFailure failure = GraphicsImageFailure::None;
    const auto probe = writeGraphicsImageWithFailure(
        source, [](std::size_t) { return std::shared_ptr<std::uint8_t>(); }, &failure);
    if (failure != GraphicsImageFailure::StorageRefused || probe.isValid()) {
        // Only an otherwise usable integer intensity part that the real writer
        // refused is observable. Floating, empty, oversized and malformed parts
        // above are intentionally skipped without a warning.
        const ImageReason reason = imageRejectionReason(failure);
        if (reason != ImageReason::None) return rejectImage(rejection, reason, source);
        return std::nullopt;
    }
    return source;
}

/** Validated range: every non-sample field plus the step that writes the samples. */
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
    /** Writes the samples; returns the part whose raw copy failed, or null on success. */
    std::function<const heliotis::FramePart*(GraphicsRangeFrameWriter&)> fill;
};

/**
 * Plans the organized range for the typed Range part or a raw preview.
 *
 * @param frame Source frame whose parts stay owned by the caller.
 * @param request Requested components and auxiliary channels.
 * @return The validated range plan, or no value when no part holds representable samples.
 */
[[nodiscard]] std::optional<PlannedRange> planRange(
    const heliotis::Frame& frame,
    const GraphicsFrameRequest& request,
    heliotis::HeliotisRangeRejection* rejection)
{
    using heliotis::FramePart;
    using heliotis::FramePartKind;
    using heliotis::Scan3dGeometry;

    const std::optional<std::size_t> typedRangeIndex = findPartIndex(frame, FramePartKind::Range);
    std::optional<std::size_t> rangeIndex;
    bool rawPartPreview = true;
    if (typedRangeIndex && hasRepresentableSamples(frame.parts[*typedRangeIndex]))
    {
        rangeIndex = typedRangeIndex;
        rawPartPreview = frame.scan3dGeometry.has_value()
            && !hasUsableScan3dGeometry(*frame.scan3dGeometry);
    }
    else if (typedRangeIndex && rejection)
    {
        const FramePart& part = frame.parts[*typedRangeIndex];
        if (hasRepresentableDimensions(part))
        {
            const auto width = static_cast<int>(part.width);
            const auto height = static_cast<int>(part.height);
            const auto readableBytes = sampleBytes(part);
            const auto requiredBytes = gridBytes(part, width, height);
            if (readableBytes != 0U && requiredBytes != 0U && readableBytes != requiredBytes)
            {
                static_cast<void>(rejectRange(rejection, RangeReason::InvalidLayout,
                    static_cast<std::uint32_t>(*typedRangeIndex), frame.parts.size(),
                    width, height, frame.sequence, readableBytes, requiredBytes));
            }
        }
    }
    if (!rangeIndex)
    {
        for (std::size_t index = 0; index < frame.parts.size(); ++index)
        {
            if (hasRepresentableSamples(frame.parts[index]))
            {
                rangeIndex = index;
                break;
            }
        }
    }
    if (!rangeIndex)
    {
        return rejectRange(rejection, RangeReason::MissingRange,
                           heliotis::HeliotisRangeRejection::noPart, frame.parts.size(),
                           0, 0, frame.sequence, 0U, 0U);
    }
    if (rangeIndex != typedRangeIndex) rawPartPreview = true;
    const FramePart& rangePart = frame.parts[*rangeIndex];
    const auto partIndex = static_cast<std::uint32_t>(*rangeIndex);
    const std::size_t partCount = frame.parts.size();

    const bool oneRowPreview = !hasRepresentableDimensions(rangePart);
    rawPartPreview = rawPartPreview || oneRowPreview;
    const std::size_t expected = sampleCount(rangePart);
    const int width = oneRowPreview ? static_cast<int>(expected) : static_cast<int>(rangePart.width);
    const int height = oneRowPreview ? 1 : static_cast<int>(rangePart.height);
    const auto* rangeRawSamples = std::get_if<std::vector<std::uint16_t>>(&rangePart.samples);
    const FramePart* intensityPart = request.includeRangeAuxiliaryChannels
        ? findPart(frame, FramePartKind::Intensity) : nullptr;
    if (intensityPart != nullptr)
    {
        if (!hasCompatibleDimensions(*intensityPart, width, height))
        {
            intensityPart = nullptr;
        }
        else if (sampleCount(*intensityPart) != expected)
        {
            return rejectRange(rejection, RangeReason::InvalidLayout,
                               static_cast<std::uint32_t>(intensityPart - frame.parts.data()),
                               partCount, width, height, frame.sequence,
                               sampleBytes(*intensityPart), gridBytes(*intensityPart, width, height));
        }
    }
    const FramePart* confidencePart = request.includeRangeAuxiliaryChannels
        ? findPart(frame, FramePartKind::Confidence) : nullptr;
    if (confidencePart != nullptr)
    {
        if (!hasCompatibleDimensions(*confidencePart, width, height))
        {
            confidencePart = nullptr;
        }
        else if (sampleCount(*confidencePart) != expected)
        {
            return rejectRange(rejection, RangeReason::InvalidLayout,
                               static_cast<std::uint32_t>(confidencePart - frame.parts.data()),
                               partCount, width, height, frame.sequence,
                               sampleBytes(*confidencePart), gridBytes(*confidencePart, width, height));
        }
    }

    GraphicsRangeFrameLayout layout;
    layout.width = width;
    layout.height = height;
    layout.channels.validMask = hasNonFiniteSamples(rangePart);
    layout.channels.rangeRaw = rangeRawSamples != nullptr && rangeRawSamples->size() == expected;
    layout.channels.intensity = intensityPart != nullptr;
    layout.channels.intensityValidMask = intensityPart != nullptr && hasNonFiniteSamples(*intensityPart);
    layout.channels.intensityRaw = intensityPart != nullptr
        && std::get_if<std::vector<std::uint16_t>>(&intensityPart->samples) != nullptr
        && std::get<std::vector<std::uint16_t>>(intensityPart->samples).size() == expected;
    layout.channels.confidence = confidencePart != nullptr;
    layout.channels.confidenceValidMask = confidencePart != nullptr && hasNonFiniteSamples(*confidencePart);
    layout.channels.confidenceRaw = confidencePart != nullptr
        && std::get_if<std::vector<std::uint16_t>>(&confidencePart->samples) != nullptr
        && std::get<std::vector<std::uint16_t>>(confidencePart->samples).size() == expected;

    const std::uint64_t readableBytes = sampleBytes(rangePart);
    const std::uint64_t requiredBytes = gridBytes(rangePart, width, height);
    {
        // A storage that always refuses validates the layout and reports the block size; any
        // failure other than its own refusal is a real writer rule.
        const GraphicsRangeFrameWriter probe(
            layout, [](std::size_t) { return std::shared_ptr<std::uint8_t>(); });
        if (probe.failure() != GraphicsRangeFrameWriterFailure::StorageRefused)
        {
            return rejectRangeWriter(rejection, probe, partIndex, partCount, width, height,
                                     frame.sequence, readableBytes, requiredBytes);
        }
    }

    PlannedRange planned;
    planned.layout = layout;
    planned.partIndex = *rangeIndex;
    planned.partCount = partCount;
    planned.width = width;
    planned.height = height;
    planned.sequence = frame.sequence;
    planned.readableBytes = readableBytes;
    planned.requiredBytes = requiredBytes;
    planned.firstPart = frame.parts.data();
    planned.fill = [&rangePart, intensityPart, confidencePart, rangeRawSamples, channels = layout.channels](
        GraphicsRangeFrameWriter& writer) -> const FramePart*
    {
        writeConvertedSamples(rangePart, writer.zData(), writer.validMaskData());
        if (channels.rangeRaw
            && !writer.writeRangeRaw(rangeRawSamples->data(), GraphicsSampleByteOrder::LittleEndian))
        {
            return &rangePart;
        }
        if (intensityPart != nullptr)
        {
            writeConvertedSamples(*intensityPart, writer.intensityData(), writer.intensityValidMaskData());
            if (channels.intensityRaw
                && !writer.writeIntensityRaw(
                    std::get<std::vector<std::uint16_t>>(intensityPart->samples).data(),
                    GraphicsSampleByteOrder::LittleEndian))
            {
                return intensityPart;
            }
        }
        if (confidencePart != nullptr)
        {
            writeConvertedSamples(*confidencePart, writer.confidenceData(), writer.confidenceValidMaskData());
            if (channels.confidenceRaw
                && !writer.writeConfidenceRaw(
                    std::get<std::vector<std::uint16_t>>(confidencePart->samples).data(),
                    GraphicsSampleByteOrder::LittleEndian))
            {
                return confidencePart;
            }
        }
        return nullptr;
    };

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
    else if (!oneRowPreview)
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

/** Writes a planned range's samples into @p storage and returns the complete range frame. */
[[nodiscard]] std::optional<RangeFrame> writeRange(
    PlannedRange planned,
    const GraphicsImageStorage& storage,
    heliotis::HeliotisRangeRejection* rejection)
{
    GraphicsRangeFrameWriter writer(planned.layout, storage);
    const auto partIndex = static_cast<std::uint32_t>(planned.partIndex);
    if (!writer.isValid())
    {
        return rejectRangeWriter(rejection, writer, partIndex, planned.partCount, planned.width,
                                 planned.height, planned.sequence, planned.readableBytes,
                                 planned.requiredBytes);
    }
    if (const auto* failed = planned.fill(writer))
    {
        return rejectRange(rejection, RangeReason::InvalidLayout,
                           static_cast<std::uint32_t>(failed - planned.firstPart), planned.partCount,
                           planned.width, planned.height, planned.sequence,
                           sampleBytes(*failed), gridBytes(*failed, planned.width, planned.height));
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

} // namespace

namespace heliotis {

void HeliotisC4GraphicsFrameAdapter::setFrameStorageProvider(std::shared_ptr<IFrameStorageProvider> provider)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    _provider = std::move(provider);
}

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertFrameWithRejection(
    const Frame& frame,
    const GraphicsFrameRequest& request,
    HeliotisRangeRejection& rangeRejection,
    HeliotisImageRejection& imageRejection) const
{
    rangeRejection = {};
    imageRejection = {};
    return convertGraphicsFrame(frame, request, &rangeRejection, &imageRejection);
}

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertGraphicsFrame(
    const Frame& frame,
    const GraphicsFrameRequest& request,
    HeliotisRangeRejection* rangeRejection,
    HeliotisImageRejection* imageRejection) const
{
    std::shared_ptr<IFrameStorageProvider> provider;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        provider = _provider;
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

    std::optional<PlannedRange> rangePlan;
    if (rangeRequested)
    {
        rangePlan = planRange(frame, request, rangeRejection);
    }

    struct PlannedImage
    {
        GraphicsImageWriterSource source;
        std::size_t partIndex = 0;
    };
    std::vector<PlannedImage> imagePlans;
    if (imageRequested)
    {
        // Every integer intensity-class part is also a 2D image of the same
        // cohort, so image consumers such as get().image() resolve the range
        // through the shared group without an explicit registration edge.
        for (std::size_t index = 0; index < frame.parts.size(); ++index)
        {
            const FramePart& part = frame.parts[index];
            if (part.kind != FramePartKind::Intensity) continue;
            if (auto source = planIntensityImage(part, frame.sequence, imageRejection))
            {
                imagePlans.push_back({*source, index});
            }
        }
    }
    if (!rangePlan && imagePlans.empty())
    {
        return std::nullopt;
    }

    // Descriptors, display names (with duplicate suffixes) and metadata are final before planning,
    // so the frame serializes exactly as the plan assumed.
    GraphicsFrame result;
    if (rangePlan)
    {
        result.addRange(resourceId("range", rangePlan->partIndex), RangeFrame{}, "Range", kResourceGroup);
    }
    for (const auto& image : imagePlans)
    {
        const FramePart& part = frame.parts[image.partIndex];
        result.addImage(resourceId("image", image.partIndex), GraphicsImage{},
                        part.name.empty() ? std::string("Intensity") : part.name, kResourceGroup);
    }
    std::vector<std::string> names;
    for (const auto& image : result.images) names.push_back(image.descriptor.displayName);
    for (std::size_t index = 0; index < result.images.size(); ++index)
    {
        if (std::count(names.cbegin(), names.cend(), names[index]) > 1)
        {
            result.images[index].descriptor.displayName += " " + std::to_string(index + 1);
        }
    }
    result.metadata.sourceName = "Heliotis C4";
    result.metadata.frameId = frame.frameId;
    result.metadata.frameIndex = frame.sequence;

    FrameStoragePlan storagePlan;
    if (provider)
    {
        FrameStorageShape shape;
        shape.metadata = result.metadata;
        bool shaped = true;
        for (std::size_t index = 0; index < imagePlans.size(); ++index)
        {
            auto imageShape = frameStorageImageShape(result.images[index].descriptor,
                result.images[index].initialView, imagePlans[index].source);
            if (!imageShape)
            {
                shaped = false;
                break;
            }
            shape.images.push_back(std::move(*imageShape));
        }
        if (rangePlan)
        {
            shape.ranges.push_back(frameStorageRangeShape(result.ranges.front().descriptor,
                result.ranges.front().initialView, rangePlan->layout, rangePlan->fields));
        }
        if (shaped) storagePlan = provider->acquireFrame(shape);
        if (!storagePlan.succeeded())
        {
            // One refusal ends the frame attempt; the adapter never retries component by component.
            if (rangePlan && rangeRejection)
            {
                if (rangeRejection->reason == RangeReason::None)
                {
                    rangeRejection->requestedStorageBytes =
                        static_cast<std::uint64_t>(storagePlan.totalBytes);
                }
                static_cast<void>(rejectRange(rangeRejection, RangeReason::StorageRefused,
                    static_cast<std::uint32_t>(rangePlan->partIndex), rangePlan->partCount,
                    rangePlan->width, rangePlan->height, rangePlan->sequence,
                    rangePlan->readableBytes, rangePlan->requiredBytes));
            }
            if (!imagePlans.empty())
            {
                static_cast<void>(rejectImage(imageRejection, ImageReason::StorageRefused,
                                              imagePlans.front().source));
            }
            return std::nullopt;
        }
    }

    for (std::size_t index = 0; index < imagePlans.size(); ++index)
    {
        const GraphicsImageStorage storage = provider
            ? frameStorageBlockFunction(storagePlan, storagePlan.images[index])
            : GraphicsImageStorage{};
        GraphicsImageFailure failure = GraphicsImageFailure::None;
        auto image = writeGraphicsImageWithFailure(imagePlans[index].source, storage, &failure);
        if (!image.isValid())
        {
            const ImageReason reason = imageRejectionReason(failure);
            if (reason != ImageReason::None)
            {
                static_cast<void>(rejectImage(imageRejection, reason, imagePlans[index].source));
            }
            return std::nullopt;
        }
        result.images[index].payload = std::move(image);
    }
    if (rangePlan)
    {
        const GraphicsImageStorage storage = provider
            ? frameStorageBlockFunction(storagePlan, storagePlan.ranges.front())
            : GraphicsImageStorage{};
        auto range = writeRange(std::move(*rangePlan), storage, rangeRejection);
        if (!range)
        {
            return std::nullopt;
        }
        result.ranges.front().payload = std::move(*range);
    }

    // Retention is observed from the assembled result, never inferred from the failure.
    if (!result.isValid())
    {
        return std::nullopt;
    }
    if (rangeRejection) {
        rangeRejection->rangeRetained = !result.ranges.empty();
        rangeRejection->imageRetained = !result.images.empty();
    }
    return result;
}

} // namespace heliotis
