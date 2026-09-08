#include "HeliotisGraphicsFrameStream.h"

#include "HeliotisC4GraphicsFrameAdapter.h"
#include "HeliotisC4System.h"

#include <memory>
#include <utility>

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

namespace heliotis {

class HeliotisGraphicsFrameStream::Impl final
{
public:
    Impl(
        HeliotisC4Device* device,
        GraphicsFrameCallback callback,
        FrameReceiptCallback receiptCallback);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool start(
        AcquisitionMode mode,
        std::string* errorMessage);
    void requestStop() noexcept;
    void stop();

private:
    HeliotisC4Device* _device = nullptr;
    GraphicsFrameCallback _callback;
    FrameReceiptCallback _receiptCallback;
    HeliotisC4GraphicsFrameAdapter _adapter;
    GraphicsFrameCallbackGate _callbackGate;
};

HeliotisGraphicsFrameStream::Impl::Impl(
    HeliotisC4Device* device,
    GraphicsFrameCallback callback,
    FrameReceiptCallback receiptCallback)
    : _device(device), _callback(std::move(callback)), _receiptCallback(std::move(receiptCallback))
{
}

HeliotisGraphicsFrameStream::Impl::~Impl()
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

bool HeliotisGraphicsFrameStream::Impl::start(
    const AcquisitionMode mode,
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
            if (_receiptCallback) {
                try
                {
                    _receiptCallback();
                }
                catch (...)
                {
                    // Receipt notification must not affect frame conversion.
                }
            }
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

void HeliotisGraphicsFrameStream::Impl::requestStop() noexcept
{
    if (_device) _device->requestStopAcquisition();
}

void HeliotisGraphicsFrameStream::Impl::stop()
{
    if (_device) _device->stopAcquisition();
}

HeliotisGraphicsFrameStream::HeliotisGraphicsFrameStream(
    HeliotisC4Device* device,
    GraphicsFrameCallback callback)
    : HeliotisGraphicsFrameStream(device, std::move(callback), {})
{
}

HeliotisGraphicsFrameStream::HeliotisGraphicsFrameStream(
    HeliotisC4Device* device,
    GraphicsFrameCallback callback,
    FrameReceiptCallback receiptCallback)
    : _impl(std::make_unique<Impl>(device, std::move(callback), std::move(receiptCallback)))
{
}

HeliotisGraphicsFrameStream::~HeliotisGraphicsFrameStream() = default;

bool HeliotisGraphicsFrameStream::start(
    const AcquisitionMode mode,
    std::string* errorMessage)
{
    return _impl->start(mode, errorMessage);
}

void HeliotisGraphicsFrameStream::requestStop() noexcept
{
    _impl->requestStop();
}

void HeliotisGraphicsFrameStream::stop()
{
    _impl->stop();
}

} // namespace heliotis
