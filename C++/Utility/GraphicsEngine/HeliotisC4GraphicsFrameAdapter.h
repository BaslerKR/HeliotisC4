#pragma once

#include "HeliotisC4.h"
#include "HeliotisC4System.h"
#include "engine/GraphicsFrameAdapter.h"

#include <functional>
#include <optional>
#include <string>

namespace heliotis {

class HeliotisC4GraphicsFrameAdapter final
    : public GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>
{
public:
    HeliotisC4GraphicsFrameAdapter() = default;
    ~HeliotisC4GraphicsFrameAdapter() = default;

    using GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>::convertFrame;

private:
    friend class GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Frame& frame,
        const GraphicsFrameRequest& request) const;
};

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
        HeliotisC4Device::AcquisitionMode mode,
        std::string* errorMessage = nullptr);
    void requestStop() noexcept;
    void stop();

private:
    HeliotisC4Device* _device = nullptr;
    GraphicsFrameCallback _callback;
    FrameReceiptCallback _receiptCallback;
    HeliotisC4GraphicsFrameAdapter _adapter;
    GraphicsFrameCallbackGate _callbackGate;
};

} // namespace heliotis
