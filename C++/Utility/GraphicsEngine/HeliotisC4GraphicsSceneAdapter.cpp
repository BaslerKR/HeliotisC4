#include "HeliotisC4GraphicsSceneAdapter.h"

#include <algorithm>
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
    return std::visit([](const auto& values) {
        std::vector<float> converted;
        converted.reserve(values.size());
        for (const auto value : values)
        {
            converted.push_back(static_cast<float>(value));
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

    RangeFrame range;
    range.width = static_cast<int>(rangePart->width);
    range.height = static_cast<int>(rangePart->height);
    range.zValues = toFloatSamples(*rangePart);
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
