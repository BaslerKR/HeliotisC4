#include "HeliotisC4GraphicsFrameAdapter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

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

} // namespace

namespace heliotis {

std::optional<GraphicsFrame> HeliotisC4GraphicsFrameAdapter::convertGraphicsFrame(
    const Frame& frame,
    const GraphicsFrameRequest& request) const
{
    if (frame.parts.empty()
        || (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range)
            && !hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::PointCloud)
            && !hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Surface)))
    {
        return std::nullopt;
    }

    const FramePart* typedRangePart = findPart(frame, FramePartKind::Range);
    const FramePart* rangePart = nullptr;
    bool rawPartPreview = true;
    if (typedRangePart && hasRepresentableSamples(*typedRangePart))
    {
        rangePart = typedRangePart;
        rawPartPreview = frame.scan3dGeometry.has_value()
            && !hasUsableScan3dGeometry(*frame.scan3dGeometry);
    }
    if (!rangePart || !hasRepresentableSamples(*rangePart))
    {
        rangePart = nullptr;
        for (const FramePart& part : frame.parts)
        {
            if (hasRepresentableSamples(part))
            {
                rangePart = &part;
                break;
            }
        }
    }
    if (!rangePart)
    {
        return std::nullopt;
    }
    if (rangePart != typedRangePart) rawPartPreview = true;

    const bool oneRowPreview = !hasRepresentableDimensions(*rangePart);
    rawPartPreview = rawPartPreview || oneRowPreview;

    RangeFrame range;
    range.width = oneRowPreview
        ? static_cast<int>(sampleCount(*rangePart))
        : static_cast<int>(rangePart->width);
    range.height = oneRowPreview ? 1 : static_cast<int>(rangePart->height);
    range.zValues = toFloatSamples(*rangePart);
    range.rangeField = fieldDescriptor(
        *rangePart,
        rawPartPreview ? "Raw Part" : "Range",
        rawPartPreview ? MeasurementValueDomain::Native : MeasurementValueDomain::Calibrated);
    range.validMask = finiteValidityMask(range.zValues);
    range.rangeRaw = rawUint16Samples(*rangePart);
    range.rangeBits = graphicsBitDepth(*rangePart);
    range.zScale = 1.0;
    range.zOffset = 0.0;
    range.xScale = std::numeric_limits<double>::quiet_NaN();
    range.yScale = std::numeric_limits<double>::quiet_NaN();
    range.xOffset = std::numeric_limits<double>::quiet_NaN();
    range.yOffset = std::numeric_limits<double>::quiet_NaN();
    if (!rawPartPreview && frame.scan3dGeometry)
    {
        const Scan3dGeometry& geometry = *frame.scan3dGeometry;
        const std::optional<GraphicsLengthUnit> unit = distanceUnit(geometry.distanceUnit);
        if (!unit.has_value()
            || !std::isfinite(geometry.zScale)
            || !std::isfinite(geometry.zOffset))
        {
            return std::nullopt;
        }
        range.lengthUnit = *unit;
        range.zScale = geometry.zScale;
        range.zOffset = geometry.zOffset;
        if (isRectifiedOutput(geometry.outputMode))
        {
            if (!std::isfinite(geometry.xScale) || !std::isfinite(geometry.yScale)
                || !std::isfinite(geometry.xOffset) || !std::isfinite(geometry.yOffset))
            {
                return std::nullopt;
            }
            range.xScale = geometry.xScale;
            range.yScale = geometry.yScale;
            range.xOffset = geometry.xOffset;
            range.yOffset = geometry.yOffset;
        }
        else
        {
            // CalibratedC intentionally has no physical X/Y information. Keep
            // its physical Z values, but expose a pixel-grid preview instead
            // of inventing a millimetre calibration.
            // heliViewer's CalibratedC surface convention is a 1 µm pixel
            // grid. It exposes relative relief without claiming calibrated
            // object-space X/Y coordinates.
            const double previewPixelPitch = convertGraphicsLength(
                0.001,
                GraphicsLengthUnit::Millimeter,
                range.lengthUnit);
            range.xScale = previewPixelPitch;
            range.yScale = previewPixelPitch;
            range.xOffset = 0.0;
            range.yOffset = 0.0;
            range.xyCoordinateMode = RangeFrameXYCoordinateMode::PixelGrid;
        }
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

    GraphicsFrame result;
    result.rangeFrame = std::move(range);
    result.metadata.sourceName = "Heliotis C4";
    result.metadata.frameId = frame.frameId;
    result.metadata.frameIndex = frame.sequence;
    return result;
}

namespace {

[[nodiscard]] GraphicsFrameRequest heliotisGraphicsFrameRequest() noexcept
{
    GraphicsFrameRequest request;
    request.components = GraphicsFrameComponent::Range | GraphicsFrameComponent::PointCloud;
    request.includeRangeAuxiliaryChannels = true;
    request.includePointCloudColors = false;
    return request;
}

} // namespace

HeliotisGraphicsFrameStream::HeliotisGraphicsFrameStream(
    HeliotisC4Device* device,
    GraphicsFrameCallback callback)
    : _device(device), _callback(std::move(callback))
{
}

HeliotisGraphicsFrameStream::~HeliotisGraphicsFrameStream()
{
    _callbackGate.beginShutdown();
    if (_device)
    {
        _device->requestStopAcquisition();
        try
        {
            _device->stopAcquisition();
        }
        catch (...)
        {
            // Destruction must remain non-throwing; the gate still drains
            // callbacks already admitted before the SDK stop attempt.
        }
    }
    _callbackGate.waitForDrain();
}

bool HeliotisGraphicsFrameStream::start(
    const HeliotisC4Device::AcquisitionMode mode,
    std::string* errorMessage)
{
    if (!_device || !_callback)
    {
        if (errorMessage) *errorMessage = "Heliotis GraphicsFrame stream is unavailable.";
        return false;
    }

    const auto callbackToken = _callbackGate.token();
    return _device->startAcquisition(mode,
        [this, callbackToken](Frame&& sourceFrame) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease) return;
            try
            {
                auto frame = _adapter.convertFrame(sourceFrame, heliotisGraphicsFrameRequest());
                if (frame.has_value()) _callback(std::move(*frame), 0U);
            }
            catch (...)
            {
                // Do not let host conversion or consumer exceptions cross the SDK callback.
            }
        }, errorMessage);
}

void HeliotisGraphicsFrameStream::requestStop() noexcept
{
    if (_device) _device->requestStopAcquisition();
}

void HeliotisGraphicsFrameStream::stop()
{
    if (_device) _device->stopAcquisition();
}

} // namespace heliotis
