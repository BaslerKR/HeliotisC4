#include "HeliotisC4GraphicsFrameAdapter.h"

#include "graphics/data/GraphicsRangeFrameWriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

[[nodiscard]] std::uint64_t gridBytes(const int width, const int height) noexcept
{
    return width > 0 && height > 0
        ? static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * sizeof(std::uint16_t)
        : 0U;
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
        rejection->sourceIndex = partIndex;
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
    if (!rejection) return std::nullopt;
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
    const std::size_t partIndex,
    const GraphicsImageWriterSource& source) noexcept
{
    if (rejection && rejection->reason == ImageReason::None) {
        rejection->reason = reason;
        rejection->conversion = heliotis::HeliotisImageRejection::Conversion::Copy;
        rejection->sourceIndex = static_cast<std::uint32_t>(partIndex);
        rejection->width = source.width > 0 ? static_cast<std::uint32_t>(source.width) : 0U;
        rejection->height = source.height > 0 ? static_cast<std::uint32_t>(source.height) : 0U;
        rejection->frameSequence = source.frameSequence;
        rejection->reportedStride = source.stride > 0 ? static_cast<std::uint64_t>(source.stride) : 0U;
        rejection->effectiveStride = rejection->reportedStride;
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
 * Copies one integer intensity part into an owned Mono8 or Mono16 image through the image writer.
 *
 * @param part Deep-owned C4 part with integer samples.
 * @param sequence Producer frame counter stored as image metadata.
 * @param frameStorage Destination storage; empty uses ordinary memory.
 * @return The image, or no value for floating, empty, oversized, or refused parts.
 * @note Samples keep their unscaled sensor codes. Mono16 rows use the
 *       little-endian host order of the copied C4 samples.
 */
[[nodiscard]] std::optional<GraphicsImage> intensityImage(
    const heliotis::FramePart& part,
    const std::uint64_t sequence,
    const std::size_t partIndex,
    const GraphicsImageStorage& frameStorage,
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

    // The writer copies or narrows the sample bytes once into the requested storage.
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

    GraphicsImageFailure failure = GraphicsImageFailure::None;
    GraphicsImage image = writeGraphicsImageWithFailure(source, frameStorage, &failure);
    if (!image.isValid()) {
        // Only an otherwise usable integer intensity part that the real writer
        // refused is observable. Floating, empty, oversized and malformed parts
        // above are intentionally skipped without a warning.
        const ImageReason reason = imageRejectionReason(failure);
        if (reason != ImageReason::None) return rejectImage(rejection, reason, partIndex, source);
        return std::nullopt;
    }
    return image;
}

struct SelectedRange
{
    RangeFrame range;
    std::size_t partIndex = 0;
};

/**
 * Builds the organized range for the typed Range part or a raw preview.
 *
 * @param frame Source frame whose parts stay owned by the caller.
 * @param request Requested components and auxiliary channels.
 * @param frameStorage Destination storage for the range samples; empty uses
 *        ordinary memory.
 * @return The range and its source part index, or no value when no part
 *         holds representable samples or the storage is refused.
 */
[[nodiscard]] std::optional<SelectedRange> selectRange(
    const heliotis::Frame& frame,
    const GraphicsFrameRequest& request,
    const GraphicsImageStorage& frameStorage,
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
    if (expected == 0U || expected > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return rejectRange(rejection,
                           expected == 0U ? RangeReason::MissingData : RangeReason::SizeLimitExceeded,
                           partIndex, partCount, 0, 0, frame.sequence,
                           sampleBytes(rangePart), 0U);
    }
    const int width = oneRowPreview ? static_cast<int>(expected) : static_cast<int>(rangePart.width);
    const int height = oneRowPreview ? 1 : static_cast<int>(rangePart.height);
    if (expected != static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
    {
        return rejectRange(rejection, RangeReason::InvalidLayout,
                           partIndex, partCount, width, height, frame.sequence,
                           sampleBytes(rangePart), gridBytes(width, height));
    }
    const auto* rangeRawSamples = std::get_if<std::vector<std::uint16_t>>(&rangePart.samples);
    if (rangeRawSamples != nullptr && !rangeRawSamples->empty()
        && rangeRawSamples->size() != expected)
    {
        return rejectRange(rejection, RangeReason::TruncatedData,
                           partIndex, partCount, width, height, frame.sequence,
                           sampleBytes(rangePart), gridBytes(width, height));
    }

    const auto rawFits = [expected](const FramePart& part) {
        const auto* values = std::get_if<std::vector<std::uint16_t>>(&part.samples);
        return values == nullptr || values->empty() || values->size() == expected;
    };
    const FramePart* intensityPart = request.includeRangeAuxiliaryChannels
        ? findPart(frame, FramePartKind::Intensity) : nullptr;
    if (intensityPart != nullptr)
    {
        if (!hasCompatibleDimensions(*intensityPart, width, height))
        {
            intensityPart = nullptr;
        }
        else if (sampleCount(*intensityPart) != expected || !rawFits(*intensityPart))
        {
            return rejectRange(rejection, RangeReason::InvalidLayout,
                               partIndex, partCount, width, height, frame.sequence,
                               sampleBytes(*intensityPart), gridBytes(width, height));
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
        else if (sampleCount(*confidencePart) != expected || !rawFits(*confidencePart))
        {
            return rejectRange(rejection, RangeReason::InvalidLayout,
                               partIndex, partCount, width, height, frame.sequence,
                               sampleBytes(*confidencePart), gridBytes(width, height));
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
    const std::uint64_t requiredBytes = gridBytes(width, height);
    GraphicsRangeFrameWriter writer(layout, frameStorage);
    if (!writer.isValid())
    {
        return rejectRangeWriter(rejection, writer, partIndex, partCount, width, height,
                                 frame.sequence, readableBytes, requiredBytes);
    }
    writeConvertedSamples(rangePart, writer.zData(), writer.validMaskData());
    if (layout.channels.rangeRaw)
    {
        if (!writer.writeRangeRaw(rangeRawSamples->data(), GraphicsSampleByteOrder::LittleEndian))
        {
            return rejectRange(rejection, RangeReason::InvalidLayout, partIndex, partCount,
                               width, height, frame.sequence, readableBytes, requiredBytes);
        }
    }
    if (intensityPart != nullptr)
    {
        writeConvertedSamples(*intensityPart, writer.intensityData(), writer.intensityValidMaskData());
        if (layout.channels.intensityRaw)
        {
            if (!writer.writeIntensityRaw(
                std::get<std::vector<std::uint16_t>>(intensityPart->samples).data(),
                GraphicsSampleByteOrder::LittleEndian))
            {
                return rejectRange(rejection, RangeReason::InvalidLayout, partIndex, partCount,
                                   width, height, frame.sequence, readableBytes, requiredBytes);
            }
        }
    }
    if (confidencePart != nullptr)
    {
        writeConvertedSamples(*confidencePart, writer.confidenceData(), writer.confidenceValidMaskData());
        if (layout.channels.confidenceRaw)
        {
            if (!writer.writeConfidenceRaw(
                std::get<std::vector<std::uint16_t>>(confidencePart->samples).data(),
                GraphicsSampleByteOrder::LittleEndian))
            {
                return rejectRange(rejection, RangeReason::InvalidLayout, partIndex, partCount,
                                   width, height, frame.sequence, readableBytes, requiredBytes);
            }
        }
    }

    RangeFrame range = writer.takeFrame();
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
    if (!range.isValid())
    {
        return rejectRange(rejection, RangeReason::InvalidDimensions, partIndex, partCount,
                           width, height, frame.sequence, readableBytes, requiredBytes);
    }
    return SelectedRange{std::move(range), *rangeIndex};
}

} // namespace

namespace heliotis {

void HeliotisC4GraphicsFrameAdapter::setFrameStorage(GraphicsImageStorage storage)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    _storage = std::move(storage);
}

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertFrameWithRejection(
    const Frame& frame,
    const GraphicsFrameRequest& request,
    HeliotisRangeRejection& rangeRejection,
    HeliotisImageRejection& imageRejection) const
{
    return convertGraphicsFrame(frame, request, &rangeRejection, &imageRejection);
}

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertGraphicsFrame(
    const Frame& frame,
    const GraphicsFrameRequest& request,
    HeliotisRangeRejection* rangeRejection,
    HeliotisImageRejection* imageRejection) const
{
    GraphicsImageStorage frameStorage;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        frameStorage = _storage;
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

    GraphicsFrame result;
    if (rangeRequested)
    {
        if (auto selected = selectRange(frame, request, frameStorage, rangeRejection))
        {
            result.addRange(resourceId("range", selected->partIndex), std::move(selected->range),
                            "Range", kResourceGroup);
        }
    }

    if (imageRequested)
    {
        // Every integer intensity-class part is also a 2D image of the same
        // cohort, so image consumers such as get().image() resolve the range
        // through the shared group without an explicit registration edge.
        for (std::size_t index = 0; index < frame.parts.size(); ++index)
        {
            const FramePart& part = frame.parts[index];
            if (part.kind != FramePartKind::Intensity) continue;
            auto image = intensityImage(part, frame.sequence, index, frameStorage, imageRejection);
            if (!image) continue;
            result.addImage(resourceId("image", index), std::move(*image),
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
    }

    // Retention is observed from the assembled result, never inferred from the
    // failure: a rejected range can leave a valid intensity image and vice versa.
    if (rangeRejection) {
        rangeRejection->rangeRetained = !result.ranges.empty();
        rangeRejection->imageRetained = !result.images.empty();
    }
    if (!result.isValid())
    {
        return std::nullopt;
    }
    result.metadata.sourceName = "Heliotis C4";
    result.metadata.frameId = frame.frameId;
    result.metadata.frameIndex = frame.sequence;
    return result;
}

} // namespace heliotis
