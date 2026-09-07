#pragma once

/**
 * @file HeliotisGraphicsFrameStream.h
 * @brief Owns Heliotis acquisition callbacks and publishes owned GraphicsFrame values.
 */

#include "HeliotisC4.h"
#include "engine/GraphicsFrameAdapter.h"

#include <functional>
#include <memory>
#include <string>

namespace heliotis {

class HeliotisC4Device;

/** Owns Heliotis SDK frame callback conversion and emits only owned GraphicsFrame values. */
class HeliotisGraphicsFrameStream final
{
public:
    /** Receives one acquisition-worker notification before any frame conversion. */
    using FrameReceiptCallback = std::function<void()>;

    HeliotisGraphicsFrameStream(HeliotisC4Device* device, GraphicsFrameCallback callback);
    /** Notifies receipt even when conversion fails; neither callback may destroy the stream. */
    HeliotisGraphicsFrameStream(
        HeliotisC4Device* device,
        GraphicsFrameCallback callback,
        FrameReceiptCallback receiptCallback);
    ~HeliotisGraphicsFrameStream();

    HeliotisGraphicsFrameStream(const HeliotisGraphicsFrameStream&) = delete;
    HeliotisGraphicsFrameStream& operator=(const HeliotisGraphicsFrameStream&) = delete;

    [[nodiscard]] bool start(
        AcquisitionMode mode,
        std::string* errorMessage = nullptr);
    void requestStop() noexcept;
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace heliotis
