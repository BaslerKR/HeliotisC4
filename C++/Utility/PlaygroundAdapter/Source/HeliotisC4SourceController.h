#pragma once

#include "AbstractSourceController.h"
#include "HeliotisC4System.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace heliotis {
class HeliotisGraphicsFrameStream;
}

/** Adapts worker-owned Heliotis frames and status into the host imaging contract. */
class HeliotisC4SourceController final : public AbstractSourceController {
    Q_OBJECT

public:
    explicit HeliotisC4SourceController(heliotis::HeliotisC4Device* device, QObject* parent = nullptr);
    ~HeliotisC4SourceController() override;

    /** Arms continuous acquisition through the common controller contract. */
    void start() override;
    /** Arms continuous acquisition and reports whether the SDK accepted it. */
    [[nodiscard]] bool startLive();
    /** Requests non-blocking worker shutdown for interactive UI controls. */
    void requestStop();
    /** Stops and joins acquisition for session teardown. */
    void stop() override;
    [[nodiscard]] bool isGrabbing() const override;
    [[nodiscard]] bool grabOne();
    void setFrameConsumer(FrameConsumer consumer) override;
    [[nodiscard]] bool supports3D() const override;

signals:
    /** Reports that one continuous SDK frame was received and the next software trigger may be queued. */
    void acquisitionFrameReceived();
    /** Reports worker arm/disarm state and whether FrameStart software control is available. */
    void acquisitionStateChanged(bool acquiring, bool continuous, bool softwareTriggerAvailable);
    /** Reports an acquisition failure that is safe to present to the operator. */
    void acquisitionError(const QString& message);

private:
    /** Applies one controller mode and reports synchronous arm acceptance. */
    [[nodiscard]] bool startAcquisition(heliotis::HeliotisC4Device::AcquisitionMode mode);
    /** Forwards one module-owned GraphicsFrame to the session consumer. */
    void handleFrame(GraphicsFrame&& frame, unsigned int sourceIndex);

    heliotis::HeliotisC4Device* _device = nullptr;
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};
    std::atomic<bool> _isContinuous{false};
    std::atomic<bool> _softwareTriggerActive{false};
    std::atomic<std::uint64_t> _statusGeneration{0};
    std::atomic<std::uint64_t> _framesInCurrentAcquisition{0};
    heliotis::HeliotisC4Device::CallbackId _statusCallbackId = 0;
    std::unique_ptr<heliotis::HeliotisGraphicsFrameStream> _graphicsStream;
};
