#include "HeliotisC4GraphicsSceneAdapter.h"
#include "engine/range/RangeFramePointCloudBuilder.h"

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

    heliotis::HeliotisC4GraphicsSceneAdapter adapter;
    const auto scene = adapter.convert(frame, {});
    if (!scene || !scene->rangeFrame)
    {
        std::cerr << "A valid H8 range part must produce a GraphicsEngine range scene.\n";
        return 1;
    }

    const auto& range = *scene->rangeFrame;
    if (range.width != 2 || range.height != 2
        || range.zValues.size() != 4U || range.intensity.size() != 4U
        || std::fabs(range.zValues.at(3) + 0.00775F) > 0.0000001F
        || std::fabs(range.intensity.at(1) - 10.0F) > 0.0001F
        || range.intensityBits != 16U
        || std::fabs(range.xScaleMm - 0.002) > 0.0000001
        || std::fabs(range.yScaleMm - 0.003) > 0.0000001
        || std::fabs(range.zScaleMm - 1.0) > 0.0000001
        || std::fabs(range.xOffsetMm - 0.1) > 0.0000001
        || std::fabs(range.yOffsetMm - 0.2) > 0.0000001
        || std::fabs(range.zOffsetMm) > 0.0000001
        || scene->meta.frameIndex != 7U)
    {
        std::cerr << "The GraphicsEngine scene must preserve H8 frame geometry, values, and metadata.\n";
        return 1;
    }

    if (!RangeFramePointCloudBuilder::canBuild(range))
    {
        std::cerr << "Rectified H8 range data must produce calibrated point-cloud geometry.\n";
        return 1;
    }

    frame.scan3dGeometry->outputMode = "CalibratedC";
    const auto calibratedScene = adapter.convert(frame, {});
    if (!calibratedScene || !calibratedScene->rangeFrame
        || RangeFramePointCloudBuilder::canBuild(*calibratedScene->rangeFrame))
    {
        std::cerr << "CalibratedC without X/Y payload axes must not infer 3D geometry.\n";
        return 1;
    }

    GraphicsScene3DRequest rangeOnlyRequest;
    rangeOnlyRequest.includeRangeAuxiliaryChannels = false;
    const auto rangeOnlyScene = adapter.convert(frame, rangeOnlyRequest);
    if (!rangeOnlyScene || !rangeOnlyScene->rangeFrame || !rangeOnlyScene->rangeFrame->intensity.empty())
    {
        std::cerr << "Auxiliary channels must follow the GraphicsEngine scene request.\n";
        return 1;
    }

    return 0;
}
