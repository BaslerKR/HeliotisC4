#include "HeliotisC4GraphicsSceneAdapter.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace {

[[nodiscard]] const heliotis::FramePart* findPart(
    const heliotis::Frame& frame,
    const heliotis::FramePartKind kind) noexcept
{
    const auto it = std::find_if(frame.parts.cbegin(), frame.parts.cend(), [kind](const auto& part) {
        return part.kind == kind;
    });
    return it == frame.parts.cend() ? nullptr : &*it;
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

[[nodiscard]] std::uint8_t graphicsBitDepth(const heliotis::FramePart& part) noexcept
{
    return part.bitsPerSample == 0
        ? 0
        : static_cast<std::uint8_t>(std::min<std::uint16_t>(part.bitsPerSample, 255U));
}

[[nodiscard]] std::string lowerCase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] double distanceUnitToMillimeters(const std::string& distanceUnit) noexcept
{
    const std::string value = lowerCase(distanceUnit);
    if (value == "nm") return 0.000001;
    if (value == "um") return 0.001;
    if (value == "mm") return 1.0;
    return std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] bool isRectifiedOutput(const std::string& outputMode) noexcept
{
    return lowerCase(outputMode) == "rectifiedc";
}

} // namespace

namespace heliotis {

std::optional<GraphicsScene3D> HeliotisC4GraphicsSceneAdapter::convertScene3D(
    const Frame& frame,
    const GraphicsScene3DRequest& request) const
{
    if (!frame.isValid()
        || (!hasScene3DContent(request.content, GraphicsScene3DContent::RangeFrame)
            && !hasScene3DContent(request.content, GraphicsScene3DContent::PointCloud)
            && !hasScene3DContent(request.content, GraphicsScene3DContent::SurfaceMesh)))
    {
        return std::nullopt;
    }

    const FramePart* rangePart = findPart(frame, FramePartKind::Range);
    if (!rangePart
        || rangePart->width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
        || rangePart->height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    {
        return std::nullopt;
    }

    if (!frame.scan3dGeometry)
    {
        return std::nullopt;
    }
    const Scan3dGeometry& geometry = *frame.scan3dGeometry;
    const double unitToMillimeters = distanceUnitToMillimeters(geometry.distanceUnit);
    if (!std::isfinite(unitToMillimeters)
        || !std::isfinite(geometry.zScale)
        || !std::isfinite(geometry.zOffset))
    {
        return std::nullopt;
    }

    RangeFrame range;
    range.width = static_cast<int>(rangePart->width);
    range.height = static_cast<int>(rangePart->height);
    range.zValues = toFloatSamples(*rangePart);
    const double zScaleMm = geometry.zScale * unitToMillimeters;
    const double zOffsetMm = geometry.zOffset * unitToMillimeters;
    for (float& value : range.zValues)
    {
        if (std::isfinite(value)) value = static_cast<float>(static_cast<double>(value) * zScaleMm + zOffsetMm);
    }
    range.zScaleMm = 1.0;
    range.zOffsetMm = 0.0;
    if (isRectifiedOutput(geometry.outputMode))
    {
        if (!std::isfinite(geometry.xScale) || !std::isfinite(geometry.yScale)
            || !std::isfinite(geometry.xOffset) || !std::isfinite(geometry.yOffset))
        {
            return std::nullopt;
        }
        range.xScaleMm = geometry.xScale * unitToMillimeters;
        range.yScaleMm = geometry.yScale * unitToMillimeters;
        range.xOffsetMm = geometry.xOffset * unitToMillimeters;
        range.yOffsetMm = geometry.yOffset * unitToMillimeters;
    }
    else
    {
        // CalibratedC does not include X/Y axes in its 2.5D payload.  Do not
        // infer a uniform grid for a non-rectified surface.
        range.xScaleMm = std::numeric_limits<double>::quiet_NaN();
        range.yScaleMm = std::numeric_limits<double>::quiet_NaN();
        range.xOffsetMm = std::numeric_limits<double>::quiet_NaN();
        range.yOffsetMm = std::numeric_limits<double>::quiet_NaN();
    }
    range.sensorType = "Heliotis H8";
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
            range.intensityBits = graphicsBitDepth(*intensityPart);
        }

        if (const FramePart* confidencePart = findPart(frame, FramePartKind::Confidence);
            confidencePart && hasCompatibleDimensions(*confidencePart, range.width, range.height))
        {
            range.confidence = toFloatSamples(*confidencePart);
            range.confidenceBits = graphicsBitDepth(*confidencePart);
        }
    }

    GraphicsScene3D scene;
    scene.content = GraphicsScene3DContent::RangeFrame;
    scene.rangeFrame = std::move(range);
    scene.meta.sourceName = "Heliotis C4";
    scene.meta.frameId = frame.frameId;
    scene.meta.frameIndex = frame.sequence;
    scene.meta.retainSurfaceMesh = request.retainSurfaceMesh;
    return scene;
}

} // namespace heliotis
