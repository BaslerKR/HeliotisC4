#pragma once

#include "HeliotisC4.h"
#include "engine/GraphicsFrameAdapter.h"
#include "engine/GraphicsImageWriter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

class IFrameStorageProvider;
class GraphicsFrameContextSource;

namespace heliotis {

/**
 * @brief First failed rule observed while assembling a requested range.
 *
 * The reason set mirrors `DeviceRangeRejection` from the host contract so the
 * runtime owner can publish the exact enum without this adapter including Qt.
 * Only fixed numeric evidence crosses this boundary; no SDK data or strings
 * are retained.
 */
struct HeliotisRangeRejection {
    enum class Reason : std::uint32_t {
        None,
        MissingRange,
        MissingData,
        InvalidDimensions,
        UnsupportedFormat,
        InvalidLayout,
        StrideBelowMinimum,
        TruncatedData,
        MissingCalibration,
        InvalidCalibration,
        UnsupportedGeometry,
        UnsupportedUnit,
        SizeLimitExceeded,
        StorageRefused
    };

    static constexpr auto noPart = (std::numeric_limits<std::uint32_t>::max)();

    Reason reason = Reason::None;
    std::uint32_t partCount = 0;
    std::uint32_t affectedPart = noPart;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frameSequence = 0;
    std::uint64_t readableBytes = 0;
    std::uint64_t requiredBytes = 0;
    /** Destination bytes calculated by GraphicsRangeFrameWriter, even on refusal. */
    std::uint64_t requestedStorageBytes = 0;
    /** Resources assembled from the same input, independent of the rejected range. */
    bool imageRetained = false;
    bool rangeRetained = false;
};

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
     * Both rejection outputs are reset for each input.
     *
     * @note Rejections are recorded only for components the request actually asked
     *       for and only when a real failure rule was observed. An omitted optional
     *       part, a raw preview without calibration, a floating intensity part and
     *       unrequested components leave both reasons at None.
     */
    [[nodiscard]] std::optional<GraphicsFrame> convertFrameWithRejection(
        const Frame& frame,
        const GraphicsFrameRequest& request,
        HeliotisRangeRejection& rangeRejection,
        HeliotisImageRejection& imageRejection) const;

    /**
     * @brief Sets the provider that leases one slot for every stored component of a frame.
     *
     * The range and every intensity image of a frame are planned and leased together through one
     * request and written at the planned offsets of that single lease. A refused plan drops the
     * frame; the adapter never retries component by component. A null provider restores ordinary
     * per-component allocation.
     */
    void setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source);
    void setFrameStorageProvider(std::shared_ptr<IFrameStorageProvider> provider);

private:
    friend class GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Frame& frame,
        const GraphicsFrameRequest& request,
        HeliotisRangeRejection* rangeRejection = nullptr,
        HeliotisImageRejection* imageRejection = nullptr) const;

    mutable std::mutex _storageMutex;
    std::shared_ptr<IFrameStorageProvider> _provider;
    std::shared_ptr<GraphicsFrameContextSource> _contextSource;
};

} // namespace heliotis
