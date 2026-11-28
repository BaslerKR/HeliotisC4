#pragma once

#include "HeliotisC4.h"
#include "engine/GraphicsFrameAdapter.h"
#include "engine/GraphicsImageWriter.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

class GraphicsFrameContextSource;

namespace heliotis {

// HeliotisRangeRejection and HeliotisFrameAnomalies live in HeliotisFrameAnomalies.h.

/**
 * @brief First failed rule observed while writing a requested integer intensity image.
 *
 * The reason and conversion sets mirror the host `DeviceImageRejection` and
 * `DeviceImageConversion` enums. This adapter stays Qt-free; the runtime owner
 * publishes the exact host enums from these values.
 */
struct HeliotisImageRejection {
    enum class Reason : std::uint32_t {
        None,
        MissingBytes,
        InvalidDimensions,
        UnsupportedFormat,
        InvalidStride,
        StrideBelowMinimum,
        ExtentOverflow,
        TruncatedBytes,
        OddWidth,
        UnsupportedConversion,
        StorageRefused
    };
    enum class Conversion : std::uint32_t {
        None,
        Borrow,
        Copy,
        SwapByteOrder,
        JpegCopy
    };

    Reason reason = Reason::None;
    Conversion conversion = Conversion::None;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frameSequence = 0;
    /** Producer-defined numeric pixel format is unavailable in this SDK-neutral frame. */
    std::uint64_t pixelFormat = 0;
    std::uint64_t effectiveStride = 0;
    std::uint64_t minimumRowBytes = 0;
    std::uint64_t readableBytes = 0;
    std::uint64_t requiredBytes = 0;
    bool directInput = false;
};

class HeliotisC4GraphicsFrameAdapter final
    : public GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>
{
public:
    HeliotisC4GraphicsFrameAdapter() = default;
    ~HeliotisC4GraphicsFrameAdapter() = default;

    using GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>::convertFrame;

    /**
     * Converts one frame and records the first failed rule of each requested component.
     * Both rejection outputs and @p anomalies are reset for each input.
     *
     * A part is never rejected because its data looks wrong: a short payload is
     * zero-filled to its declared shape, extra samples are ignored, a part whose shape
     * does not match the range grid or whose format has no GraphicsEngine equivalent is
     * published as its own image (raw Mono8 when no format fits). Each such deviation is
     * counted in @p anomalies, and every other part still publishes.
     *
     * @note The rejection outputs describe only a requested component that could not be
     *       produced at all: nothing to publish, a size limit or a storage refusal. An
     *       omitted optional part, a raw preview without calibration, a floating intensity
     *       part and unrequested components leave both reasons at None.
     */
    [[nodiscard]] std::optional<GraphicsFrame> convertFrameWithRejection(
        const Frame& frame,
        const GraphicsFrameRequest& request,
        HeliotisRangeRejection& rangeRejection,
        HeliotisImageRejection& imageRejection,
        HeliotisFrameAnomalies* anomalies = nullptr) const;

    /** Converts using the source and cancellation fence bound to one acquisition. */
    [[nodiscard]] std::optional<GraphicsFrame> convertForRun(
        const Frame& frame, const GraphicsFrameRequest& request,
        GraphicsFrameContextSource& source, const std::atomic<bool>& cancelled,
        GraphicsFrameError& storageError, HeliotisRangeRejection& rangeRejection,
        HeliotisImageRejection& imageRejection,
        HeliotisFrameAnomalies* anomalies = nullptr) const;

    /** Installs the source that admits one complete frame transaction. */
    void setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source);

private:
    friend class GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Frame& frame,
        const GraphicsFrameRequest& request,
        HeliotisRangeRejection* rangeRejection = nullptr,
        HeliotisImageRejection* imageRejection = nullptr,
        GraphicsFrameContextSource* runSource = nullptr,
        const std::atomic<bool>* cancelled = nullptr,
        GraphicsFrameError* storageError = nullptr,
        HeliotisFrameAnomalies* anomalies = nullptr) const;

    mutable std::mutex _storageMutex;
    std::shared_ptr<GraphicsFrameContextSource> _contextSource;
};

} // namespace heliotis
