#include "HeliotisC4GraphicsFrameAdapter.h"

#include <cmath>
#include <cstdint>
#include <iostream>

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
    if (!scene || !scene->rangeFrame)
    {
        std::cerr << "A valid H8 range part must produce a GraphicsEngine range scene.\n";
        return 1;
    }

    const auto& range = *scene->rangeFrame;
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
        || std::fabs(range.worldXAt(3, 1, GraphicsLengthUnit::Millimeter) - 0.102F) > 0.0000001F
        || std::fabs(range.worldYAt(3, 1, GraphicsLengthUnit::Millimeter) - 0.203F) > 0.0000001F
        || std::fabs(range.worldZAt(3, GraphicsLengthUnit::Millimeter) + 0.00775F) > 0.0000001F
        || scene->metadata.frameIndex != 7U)
    {
        std::cerr << "The GraphicsEngine scene must preserve H8 frame geometry, values, and metadata.\n";
        return 1;
    }

    frame.scan3dGeometry->outputMode = "CalibratedC";
    const auto calibratedScene = adapter.convertFrame(frame, {});
    if (!calibratedScene || !calibratedScene->rangeFrame
        || calibratedScene->rangeFrame->xyCoordinateMode != RangeFrameXYCoordinateMode::PixelGrid
        || std::fabs(calibratedScene->rangeFrame->xScale - 1.0) > 0.0000001
        || std::fabs(calibratedScene->rangeFrame->yScale - 1.0) > 0.0000001)
    {
        std::cerr << "CalibratedC must use the 1 µm pixel-grid preview.\n";
        return 1;
    }

    frame.scan3dGeometry.reset();
    const auto rawScene = adapter.convertFrame(frame, {});
    if (!rawScene || !rawScene->rangeFrame
        || std::isfinite(rawScene->rangeFrame->xScale)
        || std::isfinite(rawScene->rangeFrame->yScale))
    {
        std::cerr << "Range data without Scan3d geometry must remain a 2D-only payload.\n";
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
    if (!invalidGeometryScene || !invalidGeometryScene->rangeFrame
        || invalidGeometryScene->rangeFrame->rangeField.domain != MeasurementValueDomain::Native
        || std::isfinite(invalidGeometryScene->rangeFrame->xScale)
        || std::isfinite(invalidGeometryScene->rangeFrame->yScale))
    {
        std::cerr << "Invalid Scan3d geometry must fall back to a raw range preview.\n";
        return 1;
    }

    GraphicsFrameRequest rangeOnlyRequest;
    rangeOnlyRequest.components = GraphicsFrameComponent::Range;
    rangeOnlyRequest.includeRangeAuxiliaryChannels = false;
    const auto rangeOnlyScene = adapter.convertFrame(frame, rangeOnlyRequest);
    if (!rangeOnlyScene || !rangeOnlyScene->rangeFrame || !rangeOnlyScene->rangeFrame->intensity.empty())
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
    if (!rawPreview || !rawPreview->rangeFrame
        || rawPreview->rangeFrame->rangeField.displayName != "Raw Part"
        || rawPreview->rangeFrame->zValues.size() != 4U
        || std::fabs(rawPreview->rangeFrame->zValues.at(2) - 33.0F) > 0.0001F)
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
