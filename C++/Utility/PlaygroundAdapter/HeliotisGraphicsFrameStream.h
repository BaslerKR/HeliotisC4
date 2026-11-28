#pragma once

/**
 * @file HeliotisGraphicsFrameStream.h
 * @brief Owns Heliotis acquisition callbacks and publishes owned GraphicsFrame values.
 */

#include "HeliotisC4.h"
#include "engine/GraphicsFrameAdapter.h"
#include "engine/GraphicsImageWriter.h"

#include <functional>
#include <memory>
#include <string>

class GraphicsFrameContextSource;

namespace heliotis {

class HeliotisC4Device;
struct HeliotisRangeRejection;
struct HeliotisImageRejection;

/** Owns Heliotis SDK frame callback conversion and emits only owned GraphicsFrame values. */
class HeliotisGraphicsFrameStream final
{
public:
    /** Receives one acquisition-worker notification before any frame conversion. */
    using FrameReceiptCallback = std::function<void()>;
    /** Receives the first failed range rule of a frame whose range was requested. */
    using RejectedRange = std::function<void(const HeliotisRangeRejection&)>;
    /** Receives the first failed integer image writer rule of a requested image. */
    using RejectedImage = std::function<void(const HeliotisImageRejection&)>;

    HeliotisGraphicsFrameStream(HeliotisC4Device* device, GraphicsFrameCallback callback);
    /** Notifies receipt even when conversion fails; neither callback may destroy the stream. */
    HeliotisGraphicsFrameStream(
        HeliotisC4Device* device,
        GraphicsFrameCallback callback,
        FrameReceiptCallback receiptCallback);
    /** Carries neutral assembly evidence to the runtime owner; callbacks may be empty. */
    HeliotisGraphicsFrameStream(
        HeliotisC4Device* device,
        GraphicsFrameCallback callback,
        FrameReceiptCallback receiptCallback,
        RejectedRange rejectedRange,
        RejectedImage rejectedImage);
    ~HeliotisGraphicsFrameStream();

    /** Forwards the storage intensity images are written into; callable while acquiring. */
    void setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source);

    HeliotisGraphicsFrameStream(const HeliotisGraphicsFrameStream&) = delete;
    HeliotisGraphicsFrameStream& operator=(const HeliotisGraphicsFrameStream&) = delete;

    [[nodiscard]] bool start(
        AcquisitionMode mode,
        std::string* errorMessage = nullptr);
    void requestStop() noexcept;
    void stop();
    /** @brief Copies conversion and consumer failures without touching the SDK. */
    [[nodiscard]] RuntimeDiagnostics runtimeDiagnostics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace heliotis
