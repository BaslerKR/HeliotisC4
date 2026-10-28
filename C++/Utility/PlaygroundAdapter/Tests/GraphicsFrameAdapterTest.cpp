#include "HeliotisC4GraphicsFrameAdapter.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>

namespace {

[[nodiscard]] const RangeFrame* rangeOf(const std::optional<GraphicsFrame>& frame)
{
    if (!frame)
    {
        return nullptr;
    }
    const GraphicsRangeResource* resource = frame->firstRange();
    return resource ? &resource->payload : nullptr;
}

} // namespace

int main()
{
    heliotis::Frame frame;
    frame.sequence = 7;
    frame.frameId = "h8-7";
    frame.parts = {
        {
            heliotis::FramePartKind::Range,
            "Range",
            "Coord3D_ABC32f",
            2,
            2,
            64,
            std::vector<double>{1.5, 2.5, 3.5, 4.5}
        },
        {
            heliotis::FramePartKind::Intensity,
            "Intensity",
            "Mono16",
            2,
            2,
            16,
            std::vector<std::uint16_t>{10U, 20U, 30U, 40U}
        }
    };
    frame.scan3dGeometry = heliotis::Scan3dGeometry{
        "RectifiedC",
        "um",
        2.0,
        3.0,
        0.5,
        100.0,
        200.0,
        -10.0
    };
    frame.parts.at(1).sampleScale = 0.5;

    heliotis::HeliotisC4GraphicsFrameAdapter adapter;
    const auto scene = adapter.convertFrame(frame, {});
    const RangeFrame* rangePointer = rangeOf(scene);
    if (!rangePointer)
    {
        std::cerr << "A valid H8 range part must produce a GraphicsEngine range scene.\n";
        return 1;
    }

    const auto& range = *rangePointer;
    if (range.width != 2 || range.height != 2
        || range.zValues.size() != 4U || range.intensity.size() != 4U
        || range.rangeField.displayName != "Range"
        || range.intensityField.displayName != "Intensity"
        || std::fabs(range.zValues.at(3) - 4.5F) > 0.0000001F
        || std::fabs(range.intensity.at(1) - 10.0F) > 0.0001F
        || range.intensityBits != 16U
        || range.lengthUnit != GraphicsLengthUnit::Micrometer
        || std::fabs(range.xScale - 2.0) > 0.0000001
        || std::fabs(range.yScale - 3.0) > 0.0000001
        || std::fabs(range.zScale - 0.5) > 0.0000001
        || std::fabs(range.xOffset - 100.0) > 0.0000001
        || std::fabs(range.yOffset - 200.0) > 0.0000001
        || std::fabs(range.zOffset + 10.0) > 0.0000001
        || std::fabs(range.physicalXAt(3, 1, GraphicsLengthUnit::Millimeter) - 0.102F) > 0.0000001F
        || std::fabs(range.physicalYAt(3, 1, GraphicsLengthUnit::Millimeter) - 0.203F) > 0.0000001F
        || std::fabs(range.physicalZAt(3, GraphicsLengthUnit::Millimeter) + 0.00775F) > 0.0000001F
        || scene->metadata.frameIndex != 7U)
    {
        std::cerr << "The GraphicsEngine scene must preserve H8 frame geometry, values, and metadata.\n";
        return 1;
    }

    // Composition mirrors the GenTL adapter: one "0:0" cohort with a range and
    // an intensity image, so get().image() resolves the range by group.
    if (scene->ranges.size() != 1U || scene->images.size() != 1U
        || scene->ranges.front().descriptor.id != "range.0:0.0"
        || scene->ranges.front().descriptor.groupId != "0:0"
        || scene->images.front().descriptor.id != "image.0:0.1"
        || scene->images.front().descriptor.groupId != "0:0"
        || scene->images.front().descriptor.displayName != "Intensity"
        || scene->images.front().descriptor.registration != GraphicsImageRegistration::Unspecified
        || !range.canDerivePointCloud())
    {
        std::cerr << "An H8 frame must publish one grouped range and intensity image.\n";
        return 1;
    }
    const GraphicsImage& intensity = scene->images.front().payload;
    std::uint16_t secondSample = 0;
    std::memcpy(&secondSample, intensity.data() + 2, sizeof(secondSample));
    if (intensity.pixelFormat != GraphicsImagePixelFormat::Mono16
        || intensity.width != 2 || intensity.height != 2 || intensity.stride != 4
        || intensity.declaredMonoSourceBits != 0
        || intensity.bitAlignment != GraphicsImageBitAlignment::Packed
        || intensity.frameSequence != 7U || secondSample != 20U)
    {
        std::cerr << "The intensity image must keep unscaled Mono16 sensor codes.\n";
        return 1;
    }

    frame.parts.at(1).bitsPerSample = 12;
    const auto twelveBit = adapter.convertFrame(frame, {});
    if (!twelveBit || twelveBit->images.size() != 1U
        || twelveBit->images.front().payload.declaredMonoSourceBits != 12U
        || twelveBit->images.front().payload.bitAlignment != GraphicsImageBitAlignment::LeastSignificant)
    {
        std::cerr << "A 12-bit intensity part must declare LSB-aligned Mono16 samples.\n";
        return 1;
    }
    frame.parts.at(1).bitsPerSample = 8;
    frame.parts.at(1).samples = std::vector<std::uint16_t>{10U, 300U, 30U, 40U};
    const auto eightBit = adapter.convertFrame(frame, {});
    if (!eightBit || eightBit->images.size() != 1U
        || eightBit->images.front().payload.pixelFormat != GraphicsImagePixelFormat::Mono8
        || eightBit->images.front().payload.stride != 2
        || eightBit->images.front().payload.data()[1] != 255U)
    {
        std::cerr << "An 8-bit intensity part must become a saturated Mono8 image.\n";
        return 1;
    }
    frame.parts.at(1).bitsPerSample = 16;
    frame.parts.at(1).samples = std::vector<std::uint16_t>{10U, 20U, 30U, 40U};

    GraphicsFrameRequest imageOnlyRequest;
    imageOnlyRequest.components = GraphicsFrameComponent::Image;
    const auto imageOnly = adapter.convertFrame(frame, imageOnlyRequest);
    if (!imageOnly || !imageOnly->ranges.empty() || imageOnly->images.size() != 1U)
    {
        std::cerr << "Image-only requests must omit the range resource.\n";
        return 1;
    }

    auto floatingIntensity = frame;
    floatingIntensity.parts.at(1).samples = std::vector<double>{1.0, 2.0, 3.0, 4.0};
    const auto floatingScene = adapter.convertFrame(floatingIntensity, {});
    if (!floatingScene || !floatingScene->images.empty()
        || floatingScene->ranges.front().payload.intensity.size() != 4U)
    {
        std::cerr << "Floating intensity remains a range channel without an image.\n";
        return 1;
    }

    auto twoImages = frame;
    twoImages.parts.push_back(twoImages.parts.at(1));
    const auto pairedImages = adapter.convertFrame(twoImages, {});
    if (!pairedImages || pairedImages->images.size() != 2U
        || pairedImages->images[0].descriptor.displayName != "Intensity 1"
        || pairedImages->images[1].descriptor.displayName != "Intensity 2"
        || pairedImages->images[1].descriptor.id != "image.0:0.2")
    {
        std::cerr << "Repeated intensity names must stay distinguishable.\n";
        return 1;
    }

    frame.scan3dGeometry->outputMode = "CalibratedC";
    const auto calibratedScene = adapter.convertFrame(frame, {});
    const RangeFrame* calibratedRange = rangeOf(calibratedScene);
    if (!calibratedRange
        || calibratedRange->xyCoordinateMode != RangeFrameXYCoordinateMode::PixelGrid
        || std::isfinite(calibratedRange->xScale)
        || std::isfinite(calibratedRange->yScale)
        || std::isfinite(calibratedRange->physicalXAt(0, 0, GraphicsLengthUnit::Millimeter)))
    {
        std::cerr << "CalibratedC must keep pixel-grid X/Y without a stored pitch.\n";
        return 1;
    }

    frame.scan3dGeometry.reset();
    const auto rawScene = adapter.convertFrame(frame, {});
    const RangeFrame* rawRange = rangeOf(rawScene);
    if (!rawRange
        || rawRange->xyCoordinateMode != RangeFrameXYCoordinateMode::PixelGrid
        || std::isfinite(rawRange->xScale)
        || std::isfinite(rawRange->yScale)
        || !rawRange->canDeriveSurface()
        || rawRange->canDerivePointCloud())
    {
        std::cerr << "Range data without Scan3d geometry must stay a pixel-grid height map.\n";
        return 1;
    }

    frame.scan3dGeometry = heliotis::Scan3dGeometry{
        "RectifiedC",
        "unsupported-unit",
        2.0,
        3.0,
        0.5,
        100.0,
        200.0,
        -10.0
    };
    const auto invalidGeometryScene = adapter.convertFrame(frame, {});
    const RangeFrame* invalidRange = rangeOf(invalidGeometryScene);
    if (!invalidRange
        || invalidRange->rangeField.domain != MeasurementValueDomain::Native
        || invalidRange->xyCoordinateMode != RangeFrameXYCoordinateMode::PixelGrid
        || std::isfinite(invalidRange->xScale)
        || std::isfinite(invalidRange->yScale)
        || !invalidRange->canDeriveSurface())
    {
        std::cerr << "Invalid Scan3d geometry must fall back to a pixel-grid height map.\n";
        return 1;
    }

    GraphicsFrameRequest rangeOnlyRequest;
    rangeOnlyRequest.components = GraphicsFrameComponent::Range;
    rangeOnlyRequest.includeRangeAuxiliaryChannels = false;
    const auto rangeOnlyScene = adapter.convertFrame(frame, rangeOnlyRequest);
    const RangeFrame* rangeOnly = rangeOf(rangeOnlyScene);
    if (!rangeOnly || !rangeOnly->intensity.empty())
    {
        std::cerr << "Auxiliary channels must follow the GraphicsEngine scene request.\n";
        return 1;
    }

    heliotis::Frame rawFrame;
    rawFrame.sequence = 8;
    rawFrame.parts = {
        {
            heliotis::FramePartKind::Unknown,
            {},
            "Mono16",
            2,
            2,
            16,
            std::vector<std::uint16_t>{11U, 22U, 33U, 44U}
        }
    };
    const auto rawPreview = adapter.convertFrame(rawFrame, {});
    const RangeFrame* rawPreviewRange = rangeOf(rawPreview);
    if (!rawPreviewRange
        || rawPreviewRange->rangeField.displayName != "Raw Part"
        || rawPreviewRange->xyCoordinateMode != RangeFrameXYCoordinateMode::PixelGrid
        || !rawPreviewRange->canDeriveSurface()
        || rawPreviewRange->zValues.size() != 4U
        || std::fabs(rawPreviewRange->zValues.at(2) - 33.0F) > 0.0001F)
    {
        std::cerr << "A valid unclassified H8 part must still produce a raw preview.\n";
        return 1;
    }

    rawFrame.parts.push_back({
        heliotis::FramePartKind::Intensity,
        "Intensity",
        "Mono16",
        0,
        0,
        16,
        std::vector<std::uint16_t>{}
    });
    if (rawFrame.isValid() || !adapter.convertFrame(rawFrame, {}).has_value())
    {
        std::cerr << "An invalid auxiliary part must not hide a valid displayable part.\n";
        return 1;
    }

    return 0;
}
