#include "HeliotisC4GraphicsFrameAdapter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
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

[[nodiscard]] std::vector<float> toFloatSamples(const heliotis::FramePart& part)
{
    return std::visit([&part](const auto& values) {
        std::vector<float> converted;
        converted.reserve(values.size());
        for (const auto value : values)
        {
            converted.push_back(static_cast<float>(static_cast<double>(value) * part.sampleScale));
        }
        return converted;
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

[[nodiscard]] std::vector<std::uint16_t> rawUint16Samples(const heliotis::FramePart& part)
{
    if (const auto* values = std::get_if<std::vector<std::uint16_t>>(&part.samples))
    {
        return *values;
    }
    return {};
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

[[nodiscard]] std::vector<std::uint8_t> finiteValidityMask(const std::vector<float>& values)
{
    std::vector<std::uint8_t> mask;
    bool allValid = true;
    for (const float value : values)
    {
        if (!std::isfinite(value))
        {
            allValid = false;
            break;
        }
    }
    if (allValid)
    {
        return mask;
    }

    mask.resize(values.size(), 0U);
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        mask[index] = std::isfinite(values[index]) ? 1U : 0U;
    }
    return mask;
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
 * Copies one integer intensity part into an owned Mono8 or Mono16 image.
 *
 * @param part Deep-owned C4 part with integer samples.
 * @param sequence Producer frame counter stored as image metadata.
 * @return The image, or no value for floating, empty, or oversized parts.
 * @note Samples keep their unscaled sensor codes. Mono16 rows use the
 *       little-endian host order of the copied C4 samples.
 */
[[nodiscard]] std::optional<GraphicsImage> intensityImage(
    const heliotis::FramePart& part,
    const std::uint64_t sequence)
{
    if (!hasRepresentableSamples(part) || !hasRepresentableDimensions(part)) return std::nullopt;
    const auto* values = std::get_if<std::vector<std::uint16_t>>(&part.samples);
    if (!values) return std::nullopt;

    const std::uint8_t bits = part.bitsPerSample == 0
        ? std::uint8_t{16}
        : std::min<std::uint8_t>(part.bitsPerSample, std::uint8_t{16});
    const bool eightBit = bits <= 8;
    const std::size_t width = part.width;
    const std::size_t height = part.height;
    const std::size_t bytesPerSample = eightBit ? 1U : 2U;
    if (width > static_cast<std::size_t>((std::numeric_limits<int>::max)()) / bytesPerSample) return std::nullopt;
    const std::size_t stride = width * bytesPerSample;

    auto storage = std::make_shared<std::vector<std::uint8_t>>(stride * height);
    if (eightBit) {
        for (std::size_t index = 0; index < values->size(); ++index) {
            (*storage)[index] = static_cast<std::uint8_t>(std::min<std::uint16_t>((*values)[index], 255U));
        }
    } else {
        std::memcpy(storage->data(), values->data(), values->size() * sizeof(std::uint16_t));
    }

    GraphicsImage image;
    image.storage = std::shared_ptr<const std::uint8_t>(storage, storage->data());
    image.size = storage->size();
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.stride = static_cast<int>(stride);
    image.pixelFormat = eightBit ? GraphicsImagePixelFormat::Mono8 : GraphicsImagePixelFormat::Mono16;
    image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(image.pixelFormat);
    image.declaredMonoSourceBits = eightBit || bits >= 16 ? std::uint8_t{0} : bits;
    image.sourceFormat = monoPfnc(eightBit ? std::uint8_t{8} : bits);
    image.bitAlignment = image.declaredMonoSourceBits != 0
        ? GraphicsImageBitAlignment::LeastSignificant
        : GraphicsImageBitAlignment::Packed;
    image.frameSequence = sequence;
    if (!image.isValid()) return std::nullopt;
    return image;
}

struct SelectedRange {
    RangeFrame range;
    std::size_t partIndex = 0;
};

/**
 * Builds the organized range for the typed Range part or a raw preview.
 *
 * @return The range and its source part index, or no value when no part
 *         holds representable samples.
 */
[[nodiscard]] std::optional<SelectedRange> selectRange(
    const heliotis::Frame& frame,
    const GraphicsFrameRequest& request)
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
        return std::nullopt;
    }
    if (rangeIndex != typedRangeIndex) rawPartPreview = true;
    const FramePart& rangePart = frame.parts[*rangeIndex];

    const bool oneRowPreview = !hasRepresentableDimensions(rangePart);
    rawPartPreview = rawPartPreview || oneRowPreview;

    RangeFrame range;
    range.width = oneRowPreview
        ? static_cast<int>(sampleCount(rangePart))
        : static_cast<int>(rangePart.width);
    range.height = oneRowPreview ? 1 : static_cast<int>(rangePart.height);
    range.zValues = toFloatSamples(rangePart);
    range.rangeField = fieldDescriptor(
        rangePart,
        rawPartPreview ? "Raw Part" : "Range",
        rawPartPreview ? MeasurementValueDomain::Native : MeasurementValueDomain::Calibrated);
    range.validMask = finiteValidityMask(range.zValues);
    range.rangeRaw = rawUint16Samples(rangePart);
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
    if (!range.isValid())
    {
        return std::nullopt;
    }

    if (request.includeRangeAuxiliaryChannels)
    {
        if (const FramePart* intensityPart = findPart(frame, FramePartKind::Intensity);
            intensityPart && hasCompatibleDimensions(*intensityPart, range.width, range.height))
        {
            range.intensity = toFloatSamples(*intensityPart);
            range.intensityField = fieldDescriptor(
                *intensityPart,
                "Intensity",
                MeasurementValueDomain::Native);
            range.intensityValidMask = finiteValidityMask(range.intensity);
            range.intensityRaw = rawUint16Samples(*intensityPart);
            range.intensityBits = graphicsBitDepth(*intensityPart);
        }

        if (const FramePart* confidencePart = findPart(frame, FramePartKind::Confidence);
            confidencePart && hasCompatibleDimensions(*confidencePart, range.width, range.height))
        {
            range.confidence = toFloatSamples(*confidencePart);
            range.confidenceField = fieldDescriptor(
                *confidencePart,
                "Confidence",
                MeasurementValueDomain::Native);
            range.confidenceValidMask = finiteValidityMask(range.confidence);
            range.confidenceRaw = rawUint16Samples(*confidencePart);
            range.confidenceBits = graphicsBitDepth(*confidencePart);
        }
    }
    return SelectedRange{std::move(range), *rangeIndex};
}

} // namespace

namespace heliotis {

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertGraphicsFrame(
    const Frame& frame,
    const GraphicsFrameRequest& request) const
{
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
        if (auto selected = selectRange(frame, request))
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
            auto image = intensityImage(part, frame.sequence);
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
