#include "HeliotisC4GraphicsFrameAdapter.h"

#include "DeviceRuntime/Contract/FrameStorage.h"
#include "graphics/data/GraphicsRangeFrameWriter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

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

/** Plans one lease the way the worker does for the adapter: blocks laid out back to back. */
class RecordingProvider final : public IFrameStorageProvider
{
public:
    std::size_t calls = 0U;
    std::size_t singleCalls = 0U;
    std::size_t size = 0U;
    bool refuse = false;
    std::shared_ptr<std::uint8_t> block;
    FrameStorageShape shape;
    FrameStoragePlan plan;

    std::shared_ptr<std::uint8_t> acquire(std::size_t) override
    {
        ++singleCalls;
        return {};
    }

    FrameStoragePlan acquireFrame(const FrameStorageShape& requested) override
    {
        ++calls;
        shape = requested;
        FrameStoragePlan result;
        std::size_t end = 0U;
        for (const auto& image : requested.images)
        {
            end = (end + 63U) / 64U * 64U;
            result.images.push_back({end, image.size});
            end += image.size;
        }
        for (const auto& range : requested.ranges)
        {
            end = (end + 63U) / 64U * 64U;
            const GraphicsRangeFrameWriter measure(
                range.layout, [](std::size_t) { return std::shared_ptr<std::uint8_t>(); });
            result.ranges.push_back({end, measure.blockBytes()});
            end += measure.blockBytes();
        }
        result.totalBytes = end;
        size = end;
        if (refuse)
        {
            result.refusal = FrameStorageRefusal::NoStorage;
            plan = result;
            return result;
        }
        block = std::shared_ptr<std::uint8_t>(new std::uint8_t[end], std::default_delete<std::uint8_t[]>());
        result.storage = block;
        plan = result;
        return result;
    }

    [[nodiscard]] bool contains(const void* pointer) const
    {
        const auto* bytes = static_cast<const std::uint8_t*>(pointer);
        return block && bytes >= block.get() && bytes < block.get() + size;
    }

    [[nodiscard]] const std::uint8_t* imageAt(std::size_t index) const
    {
        return block.get() + plan.images.at(index).offset;
    }
};

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
    const std::array<std::uint8_t, 8> mono16Bytes{
        10U, 0U, 20U, 0U, 30U, 0U, 40U, 0U};
    if (intensity.pixelFormat != GraphicsImagePixelFormat::Mono16
        || intensity.width != 2 || intensity.height != 2 || intensity.stride != 4
        || intensity.size != mono16Bytes.size()
        || intensity.declaredMonoSourceBits != 0
        || intensity.bitAlignment != GraphicsImageBitAlignment::Packed
        || intensity.frameSequence != 7U || secondSample != 20U
        || std::memcmp(intensity.data(), mono16Bytes.data(), mono16Bytes.size()) != 0)
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
    const std::array<std::uint8_t, 4> mono8Bytes{10U, 255U, 30U, 40U};
    const auto eightBit = adapter.convertFrame(frame, {});
    if (!eightBit || eightBit->images.size() != 1U
        || eightBit->images.front().payload.pixelFormat != GraphicsImagePixelFormat::Mono8
        || eightBit->images.front().payload.stride != 2
        || eightBit->images.front().payload.size != mono8Bytes.size()
        || std::memcmp(eightBit->images.front().payload.data(), mono8Bytes.data(),
               mono8Bytes.size()) != 0)
    {
        std::cerr << "An 8-bit intensity part must become a saturated Mono8 image.\n";
        return 1;
    }
    frame.parts.at(1).samples = std::vector<std::uint16_t>{10U, 300U, 30U};
    GraphicsFrameRequest shortPartRequest;
    shortPartRequest.includeRangeAuxiliaryChannels = false;
    const auto shortPart = adapter.convertFrame(frame, shortPartRequest);
    if (!shortPart || shortPart->ranges.size() != 1U || !shortPart->images.empty())
    {
        std::cerr << "A part whose sample count does not match its dimensions must stay image-free.\n";
        return 1;
    }
    frame.parts.at(1).bitsPerSample = 16;
    frame.parts.at(1).samples = std::vector<std::uint16_t>{10U, 20U, 30U, 40U};

    // A provided storage function receives the range channels and the image
    // bytes; every range array is borrowed from the block the writer acquired.
    // Each converted payload takes exactly one acquire: one range block and one
    // intensity image block for this frame.
    auto providedHolder = std::make_shared<RecordingProvider>();
    auto& providedStorage = *providedHolder;
    adapter.setFrameStorageProvider(providedHolder);
    const auto providedScene = adapter.convertFrame(frame, {});
    const RangeFrame* providedRange = rangeOf(providedScene);
    const std::array<float, 4> expectedZ{1.5F, 2.5F, 3.5F, 4.5F};
    const std::array<float, 4> expectedIntensity{5.0F, 10.0F, 15.0F, 20.0F};
    const std::array<std::uint16_t, 4> expectedRaw{10U, 20U, 30U, 40U};
    if (!providedScene || !providedRange || providedScene->images.size() != 1U
        || providedStorage.calls != 1U || providedStorage.singleCalls != 0U
        || providedStorage.shape.images.size() != 1U || providedStorage.shape.ranges.size() != 1U
        || !providedRange->zValues.isBorrowed()
        || !providedRange->intensity.isBorrowed()
        || !providedRange->intensityRaw.isBorrowed()
        || !providedStorage.contains(providedRange->zValues.data())
        || !providedStorage.contains(providedRange->intensity.data())
        || providedRange->validMask.size() != 0U
        || providedRange->intensityValidMask.size() != 0U
        || !std::equal(expectedZ.begin(), expectedZ.end(), providedRange->zValues.cbegin())
        || !std::equal(expectedIntensity.begin(), expectedIntensity.end(),
               providedRange->intensity.cbegin())
        || !std::equal(expectedRaw.begin(), expectedRaw.end(), providedRange->intensityRaw.cbegin())
        || providedScene->images.front().payload.data() != providedStorage.imageAt(0)
        || reinterpret_cast<const std::uint8_t*>(providedRange->zValues.data())
               != providedStorage.block.get() + providedStorage.plan.ranges[0].offset
        || std::memcmp(providedStorage.imageAt(0), mono16Bytes.data(), mono16Bytes.size()) != 0)
    {
        std::cerr << "One provided lease must carry the borrowed range channels and the image bytes.\n";
        return 1;
    }
    {
        // The shape carries exactly the fields of the frame that is then published.
        const auto& rangeShape = providedStorage.shape.ranges.front();
        const auto& imageShape = providedStorage.shape.images.front();
        const auto& published = providedScene->ranges.front();
        const auto& publishedImage = providedScene->images.front();
        if (rangeShape.descriptor.id != published.descriptor.id
            || rangeShape.descriptor.displayName != published.descriptor.displayName
            || rangeShape.descriptor.groupId != published.descriptor.groupId
            || rangeShape.layout.width != providedRange->width || rangeShape.layout.height != providedRange->height
            || rangeShape.lengthUnit != providedRange->lengthUnit
            || rangeShape.xyCoordinateMode != providedRange->xyCoordinateMode
            || rangeShape.sensorType != providedRange->sensorType || rangeShape.frameId != providedRange->frameId
            || rangeShape.rangeBits != providedRange->rangeBits
            || rangeShape.intensityBits != providedRange->intensityBits
            || rangeShape.rangeField.id != providedRange->rangeField.id
            || rangeShape.intensityField.id != providedRange->intensityField.id
            || imageShape.descriptor.id != publishedImage.descriptor.id
            || imageShape.descriptor.displayName != publishedImage.descriptor.displayName
            || imageShape.width != publishedImage.payload.width || imageShape.height != publishedImage.payload.height
            || imageShape.stride != publishedImage.payload.stride || imageShape.size != publishedImage.payload.size
            || imageShape.pixelFormat != publishedImage.payload.pixelFormat
            || imageShape.sourceFormat != publishedImage.payload.sourceFormat
            || providedStorage.shape.metadata.frameId != providedScene->metadata.frameId
            || providedStorage.shape.metadata.frameIndex != providedScene->metadata.frameIndex)
        {
            std::cerr << "The planned shape must equal the published frame.\n";
            return 1;
        }
    }
    {
        auto refusing = std::make_shared<RecordingProvider>();
        refusing->refuse = true;
        adapter.setFrameStorageProvider(refusing);
        if (adapter.convertFrame(frame, {}).has_value() || refusing->calls != 1U || refusing->singleCalls != 0U)
        {
            std::cerr << "A refused frame plan must drop the range and image with one provider call.\n";
            return 1;
        }
    }
    adapter.setFrameStorageProvider({});

    {
        // A range with two intensity images shares one lease and one admission.
        auto multi = frame;
        multi.parts.push_back({
            heliotis::FramePartKind::Intensity,
            "Intensity",
            "Mono16",
            2,
            2,
            16,
            std::vector<std::uint16_t>{50U, 60U, 70U, 80U}
        });
        auto holder = std::make_shared<RecordingProvider>();
        adapter.setFrameStorageProvider(holder);
        const auto converted = adapter.convertFrame(multi, {});
        if (!converted || converted->ranges.size() != 1U || converted->images.size() != 2U
            || holder->calls != 1U || holder->singleCalls != 0U
            || holder->shape.images.size() != 2U || holder->shape.ranges.size() != 1U
            || holder->plan.images.size() != 2U
            || converted->images.front().payload.data() != holder->imageAt(0)
            || converted->images.back().payload.data() != holder->imageAt(1)
            || converted->images.front().descriptor.displayName == converted->images.back().descriptor.displayName
            || holder->shape.images[0].descriptor.displayName != converted->images[0].descriptor.displayName
            || holder->shape.images[1].descriptor.displayName != converted->images[1].descriptor.displayName
            || !holder->contains(converted->ranges.front().payload.zValues.data()))
        {
            std::cerr << "A range with several intensity images must share one planned lease.\n";
            return 1;
        }
        adapter.setFrameStorageProvider({});
    }

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

    // Every converted field of a range + intensity + confidence frame, with a
    // non-finite sample in each floating channel to exercise the masks.
    {
        heliotis::Frame full;
        full.sequence = 9;
        full.frameId = "h8-9";
        const double nan = std::numeric_limits<double>::quiet_NaN();
        full.parts = {
            {
                heliotis::FramePartKind::Range,
                "Range",
                "Coord3D_ABC32f",
                2,
                2,
                64,
                std::vector<double>{1.5, nan, 3.5, 4.5}
            },
            {
                heliotis::FramePartKind::Intensity,
                "Intensity",
                "Mono16",
                2,
                2,
                16,
                std::vector<double>{5.0, nan, 15.0, 20.0}
            },
            {
                heliotis::FramePartKind::Confidence,
                "Confidence",
                "Confidence32f",
                2,
                2,
                32,
                std::vector<std::uint16_t>{1U, 2U, 3U, 4U}
            }
        };
        full.scan3dGeometry = heliotis::Scan3dGeometry{
            "RectifiedC", "um", 2.0, 3.0, 0.5, 100.0, 200.0, -10.0};

        auto storageHolder = std::make_shared<RecordingProvider>();
        auto& storage = *storageHolder;
        adapter.setFrameStorageProvider(storageHolder);
        const auto converted = adapter.convertFrame(full, {});
        const RangeFrame* range = rangeOf(converted);
        if (!converted || !range || converted->ranges.size() != 1U || !converted->images.empty()
            || converted->ranges.front().descriptor.id != "range.0:0.0"
            || converted->ranges.front().descriptor.displayName != "Range"
            || converted->ranges.front().descriptor.groupId != "0:0"
            || converted->metadata.sourceName != "Heliotis C4"
            || converted->metadata.frameId != "h8-9"
            || converted->metadata.frameIndex != 9U
            || !converted->metadata.cohortId.empty()
            || range->width != 2 || range->height != 2
            || range->lengthUnit != GraphicsLengthUnit::Micrometer
            || range->xyCoordinateMode != RangeFrameXYCoordinateMode::UniformPhysical
            || std::fabs(range->xScale - 2.0) > 0.0000001
            || std::fabs(range->yScale - 3.0) > 0.0000001
            || std::fabs(range->zScale - 0.5) > 0.0000001
            || std::fabs(range->xOffset - 100.0) > 0.0000001
            || std::fabs(range->yOffset - 200.0) > 0.0000001
            || std::fabs(range->zOffset + 10.0) > 0.0000001
            || range->sensorType != "Heliotis H8"
            || range->frameId != "h8-9"
            || range->rangeBits != 64U
            || range->intensityBits != 16U
            || range->confidenceBits != 32U
            || range->rangeField.id != "heliotis.range"
            || range->rangeField.displayName != "Range"
            || range->rangeField.unitSymbol != ""
            || range->rangeField.domain != MeasurementValueDomain::Calibrated
            || range->rangeField.sampleKind != MeasurementSampleKind::GridSample
            || range->rangeField.bitsPerSample != 64U
            || range->intensityField.id != "heliotis.intensity"
            || range->intensityField.displayName != "Intensity"
            || range->intensityField.unitSymbol != ""
            || range->intensityField.domain != MeasurementValueDomain::Native
            || range->intensityField.sampleKind != MeasurementSampleKind::GridSample
            || range->intensityField.bitsPerSample != 16U
            || range->confidenceField.id != "heliotis.confidence"
            || range->confidenceField.displayName != "Confidence"
            || range->confidenceField.unitSymbol != ""
            || range->confidenceField.domain != MeasurementValueDomain::Native
            || range->confidenceField.sampleKind != MeasurementSampleKind::GridSample
            || range->confidenceField.bitsPerSample != 32U
            || range->zValues.size() != 4U || range->intensity.size() != 4U
            || range->confidence.size() != 4U
            || range->validMask.size() != 4U || range->intensityValidMask.size() != 4U
            || !range->confidenceValidMask.empty()
            || range->rangeRaw.size() != 0U || range->intensityRaw.size() != 0U
            || range->confidenceRaw.size() != 4U
            || std::fabs(range->zValues.at(0) - 1.5F) > 0.0001F
            || !std::isnan(range->zValues.at(1))
            || range->validMask.at(0) != 1U || range->validMask.at(1) != 0U
            || range->validMask.at(2) != 1U || range->validMask.at(3) != 1U
            || std::fabs(range->intensity.at(0) - 5.0F) > 0.0001F
            || !std::isnan(range->intensity.at(1))
            || range->intensityValidMask.at(0) != 1U || range->intensityValidMask.at(1) != 0U
            || std::fabs(range->confidence.at(3) - 4.0F) > 0.0001F
            || range->confidenceRaw.at(2) != 3U
            || storage.calls != 1U
            || !range->zValues.isBorrowed() || !range->intensity.isBorrowed()
            || !range->confidenceRaw.isBorrowed()
            || !storage.contains(range->zValues.data())
            || !storage.contains(range->confidenceRaw.data()))
        {
            std::cerr << "Every converted range, intensity and confidence field must survive.\n";
            return 1;
        }
        adapter.setFrameStorageProvider({});
    }

    // Integer range + intensity + confidence samples keep every raw channel and
    // take exactly one provided-storage acquire for the range block and one for
    // the intensity image.
    {
        heliotis::Frame raw;
        raw.sequence = 10;
        raw.frameId = "h8-10";
        raw.parts = {
            {
                heliotis::FramePartKind::Range,
                "Range",
                "Mono16",
                2,
                2,
                16,
                std::vector<std::uint16_t>{2U, 4U, 6U, 8U}
            },
            {
                heliotis::FramePartKind::Intensity,
                "Intensity",
                "Mono16",
                2,
                2,
                16,
                std::vector<std::uint16_t>{10U, 20U, 30U, 40U}
            },
            {
                heliotis::FramePartKind::Confidence,
                "Confidence",
                "Mono16",
                2,
                2,
                16,
                std::vector<std::uint16_t>{1U, 2U, 3U, 4U}
            }
        };
        raw.parts.at(0).sampleScale = 0.5;
        raw.parts.at(1).sampleScale = 0.5;

        auto storageHolder = std::make_shared<RecordingProvider>();
        auto& storage = *storageHolder;
        adapter.setFrameStorageProvider(storageHolder);
        const auto converted = adapter.convertFrame(raw, {});
        const RangeFrame* range = rangeOf(converted);
        const std::array<std::uint16_t, 4> expectedRange{2U, 4U, 6U, 8U};
        const std::array<std::uint16_t, 4> expectedIntensity{10U, 20U, 30U, 40U};
        const std::array<std::uint16_t, 4> expectedConfidence{1U, 2U, 3U, 4U};
        if (!converted || !range || converted->ranges.size() != 1U
            || converted->images.size() != 1U)
        {
            std::cerr << "Integer range, intensity and confidence parts must convert.\n";
            return 1;
        }
        if (range->xyCoordinateMode != RangeFrameXYCoordinateMode::PixelGrid
            || std::isfinite(range->xScale) || std::isfinite(range->yScale))
        {
            std::cerr << "An integer range without Scan3d geometry must stay a PixelGrid height map.\n";
            return 1;
        }
        if (!range->rangeRaw.isBorrowed() || !range->intensityRaw.isBorrowed()
            || !range->confidenceRaw.isBorrowed()
            || range->rangeRaw.size() != 4U || range->intensityRaw.size() != 4U
            || range->confidenceRaw.size() != 4U)
        {
            std::cerr << "Every integer raw channel must be a borrowed four-sample array.\n";
            return 1;
        }
        if (!range->validMask.empty() || !range->intensityValidMask.empty()
            || !range->confidenceValidMask.empty())
        {
            std::cerr << "All-finite integer samples must not allocate a validity mask.\n";
            return 1;
        }
        if (!std::equal(expectedRange.begin(), expectedRange.end(), range->rangeRaw.cbegin())
            || !std::equal(expectedIntensity.begin(), expectedIntensity.end(),
                   range->intensityRaw.cbegin())
            || !std::equal(expectedConfidence.begin(), expectedConfidence.end(),
                   range->confidenceRaw.cbegin()))
        {
            std::cerr << "Integer raw channels must preserve their sensor codes.\n";
            return 1;
        }
        if (storage.calls != 1U || storage.singleCalls != 0U
            || !storage.contains(range->rangeRaw.data())
            || !storage.contains(range->intensityRaw.data())
            || !storage.contains(range->confidenceRaw.data())
            || converted->images.front().payload.data() != storage.imageAt(0))
        {
            std::cerr << "One lease must carry the range block and the intensity image block.\n";
            return 1;
        }
        adapter.setFrameStorageProvider({});
    }

    // A requested range can fail while a valid intensity image from the same
    // multipart input survives, and vice versa. Only the first real failed rule
    // is recorded; omissions, raw previews and unrequested parts are silent.
    {
        const auto check = [](const bool condition, const char* message) {
            if (!condition) std::cerr << message << '\n';
            return condition;
        };
        using RangeReason = heliotis::HeliotisRangeRejection::Reason;
        using ImageReason = heliotis::HeliotisImageRejection::Reason;

        heliotis::Frame input;
        input.sequence = 11;
        input.frameId = "h8-11";
        input.parts = {
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
        input.scan3dGeometry = heliotis::Scan3dGeometry{
            "RectifiedC", "um", 2.0, 3.0, 0.5, 100.0, 200.0, -10.0};

        heliotis::HeliotisRangeRejection rangeRejection;
        heliotis::HeliotisImageRejection imageRejection;
        const auto valid = adapter.convertFrameWithRejection(input, {}, rangeRejection, imageRejection);
        if (!check(valid && valid->ranges.size() == 1U && valid->images.size() == 1U,
                   "A valid frame must still publish a range and an intensity image.")
            || !check(rangeRejection.reason == RangeReason::None
                          && imageRejection.reason == ImageReason::None,
                      "A valid frame must produce no rejection evidence."))
        {
            return 1;
        }

        auto shortRange = input;
        std::get<std::vector<double>>(shortRange.parts.front().samples).pop_back();
        const auto shortResult = adapter.convertFrameWithRejection(
            shortRange, {}, rangeRejection, imageRejection);
        if (!check(shortResult && shortResult->hasImage() && shortResult->hasRangeFrame()
                       && shortResult->ranges.front().descriptor.id == "range.0:0.1"
                       && rangeRejection.rangeRetained && rangeRejection.imageRetained,
                   "A malformed typed range must leave the raw preview and intensity image available.")
            || !check(rangeRejection.reason == RangeReason::InvalidLayout
                       && rangeRejection.affectedPart == 0U
                       && rangeRejection.readableBytes == 24U && rangeRejection.requiredBytes == 32U,
                   "Range evidence must compare byte counts for the same part and sample type."))
            return 1;

        // A malformed optional auxiliary channel rejects the requested range,
        // but the separately converted intensity image is still retained.
        auto auxiliaryFailure = input;
        auxiliaryFailure.parts.push_back({
            heliotis::FramePartKind::Confidence,
            "Confidence",
            "Mono16",
            2,
            2,
            16,
            std::vector<std::uint16_t>{1U, 2U, 3U}
        });
        rangeRejection = {};
        imageRejection = {};
        const auto retained = adapter.convertFrameWithRejection(
            auxiliaryFailure, {}, rangeRejection, imageRejection);
        if (!check(rangeRejection.reason == RangeReason::InvalidLayout,
                   "A shape mismatch in a requested auxiliary channel must report the failed rule.")
            || !check(rangeRejection.affectedPart == 2U
                          && rangeRejection.readableBytes == 6U && rangeRejection.requiredBytes == 8U
                          && rangeRejection.partCount == 3U && rangeRejection.width == 2U
                          && rangeRejection.height == 2U && rangeRejection.frameSequence == 11U,
                      "Range evidence must carry the fixed part ordinal and frame sequence.")
            || !check(retained && retained->ranges.empty() && retained->images.size() == 1U,
                      "A rejected range must not hide a valid intensity image.")
            || !check(rangeRejection.rangeRetained == retained->hasRangeFrame()
                          && rangeRejection.imageRetained == retained->hasImage(),
                      "Retention flags must match the assembled result.")
            || !check(imageRejection.reason == ImageReason::None,
                      "A valid intensity image must not be reported as rejected."))
        {
            return 1;
        }

        // A refused frame plan drops the whole frame after one provider call: the range keeps the
        // evidence of its own plan or reports the refusal with the planned block bytes, and the
        // image reports StorageRefused. Nothing is retried component by component.
        auto refusing = std::make_shared<RecordingProvider>();
        refusing->refuse = true;
        adapter.setFrameStorageProvider(refusing);
        rangeRejection = {};
        imageRejection = {};
        const auto refusedPreview = adapter.convertFrameWithRejection(
            shortRange, {}, rangeRejection, imageRejection);
        if (!check(!refusedPreview && refusing->calls == 1U && refusing->singleCalls == 0U
                       && rangeRejection.reason == RangeReason::InvalidLayout
                       && rangeRejection.affectedPart == 0U && rangeRejection.requestedStorageBytes == 0U
                       && imageRejection.reason == ImageReason::StorageRefused
                       && !rangeRejection.imageRetained && !rangeRejection.rangeRetained,
                   "A refused preview plan must keep the original range rejection evidence."))
            return 1;

        refusing->calls = 0U;
        rangeRejection = {};
        imageRejection = {};
        const auto refused = adapter.convertFrameWithRejection(
            input, {}, rangeRejection, imageRejection);
        if (!check(!refused && refusing->calls == 1U && refusing->singleCalls == 0U,
                   "A refused plan must drop the frame with exactly one provider call.")
            || !check(rangeRejection.reason == RangeReason::StorageRefused,
                      "A refused range destination must report StorageRefused.")
            || !check(rangeRejection.requestedStorageBytes > 0U,
                      "A refused range plan must retain its calculated block bytes.")
            || !check(imageRejection.reason == ImageReason::StorageRefused
                          && imageRejection.width == 2U && imageRejection.effectiveStride == 4U
                          && imageRejection.height == 2U && imageRejection.frameSequence == 11U
                          && imageRejection.conversion
                                 == heliotis::HeliotisImageRejection::Conversion::Copy,
                      "Image evidence must carry the effective stride, sequence and conversion.")
            || !check(!rangeRejection.rangeRetained && !rangeRejection.imageRetained,
                      "A refused plan retains no resource."))
        {
            return 1;
        }
        adapter.setFrameStorageProvider({});

        auto optional = input;
        optional.parts.pop_back();
        const auto optionalResult = adapter.convertFrameWithRejection(
            optional, {}, rangeRejection, imageRejection);
        if (!check(optionalResult && rangeRejection.reason == RangeReason::None
                       && imageRejection.reason == ImageReason::None && imageRejection.frameSequence == 0U,
                   "An omitted optional intensity part must clear the preceding image rejection."))
        {
            return 1;
        }

        auto rawPreview = input;
        rawPreview.scan3dGeometry.reset();
        rangeRejection = {};
        imageRejection = {};
        const auto rawResult = adapter.convertFrameWithRejection(
            rawPreview, {}, rangeRejection, imageRejection);
        if (!check(rawResult && rangeRejection.reason == RangeReason::None
                       && imageRejection.reason == ImageReason::None,
                   "A raw preview without calibration must stay silent."))
        {
            return 1;
        }

        auto floating = input;
        floating.parts.at(1).samples = std::vector<double>{1.0, 2.0, 3.0, 4.0};
        rangeRejection = {};
        imageRejection = {};
        const auto floatingResult = adapter.convertFrameWithRejection(
            floating, {}, rangeRejection, imageRejection);
        if (!check(floatingResult && floatingResult->images.empty()
                       && rangeRejection.reason == RangeReason::None
                       && imageRejection.reason == ImageReason::None,
                   "An intentionally skipped floating intensity part must stay silent."))
        {
            return 1;
        }

        auto unrequested = input;
        unrequested.parts.at(0).samples = std::vector<double>{1.0, 2.0, 3.0};
        GraphicsFrameRequest imageOnly;
        imageOnly.components = GraphicsFrameComponent::Image;
        rangeRejection = {};
        imageRejection = {};
        const auto unrequestedResult = adapter.convertFrameWithRejection(
            unrequested, imageOnly, rangeRejection, imageRejection);
        if (!check(unrequestedResult && rangeRejection.reason == RangeReason::None
                       && imageRejection.reason == ImageReason::None,
                   "An unrequested broken range must stay silent."))
        {
            return 1;
        }

        heliotis::Frame missing;
        missing.sequence = 12;
        missing.parts = {
            {heliotis::FramePartKind::Range, "Range", "Mono16", 2, 2, 16,
             std::vector<std::uint16_t>{}}
        };
        rangeRejection = {};
        imageRejection = {};
        static_cast<void>(adapter.convertFrameWithRejection(
            missing, {}, rangeRejection, imageRejection));
        if (!check(rangeRejection.reason == RangeReason::MissingRange
                       && rangeRejection.affectedPart == heliotis::HeliotisRangeRejection::noPart
                       && rangeRejection.partCount == 1U,
                   "A required range with no usable part must report MissingRange."))
        {
            return 1;
        }
        rangeRejection = {};
        imageRejection = {};
        static_cast<void>(adapter.convertFrameWithRejection(
            missing, imageOnly, rangeRejection, imageRejection));
        if (!check(rangeRejection.reason == RangeReason::None,
                   "An unrequested missing range must stay silent."))
        {
            return 1;
        }
    }

    return 0;
}
