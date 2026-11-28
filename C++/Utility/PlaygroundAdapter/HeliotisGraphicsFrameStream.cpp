#include "HeliotisGraphicsFrameStream.h"

#include "HeliotisC4GraphicsFrameAdapter.h"
#include "HeliotisC4System.h"

#include <memory>
#include <atomic>
#include <optional>
#include <mutex>
#include <utility>

namespace {

[[nodiscard]] GraphicsFrameRequest heliotisGraphicsFrameRequest() noexcept
{
    GraphicsFrameRequest request;
    request.components = GraphicsFrameComponent::Image | GraphicsFrameComponent::Range
        | GraphicsFrameComponent::PointCloud;
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
        FrameReceiptCallback receiptCallback,
        RejectedRange rejectedRange,
        RejectedImage rejectedImage);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool start(
        AcquisitionMode mode,
        std::string* errorMessage);
    void requestStop() noexcept;
    void stop();
    bool acquiring() const { return _device && _device->isAcquiring(); }
    struct AcquisitionRun {
        std::shared_ptr<GraphicsFrameContextSource> source;
        std::atomic<bool> cancelled{false};
        std::recursive_mutex deliveryMutex;
        std::atomic<bool> refusalSurfaced{false};
    };
    void setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source)
    {
        cancelRun();
        std::lock_guard<std::mutex> lock(_runMutex);
        _source = std::move(source);
    }
    void setStorageFailureHandler(StorageFailure handler) { _storageFailure = std::move(handler); }
    bool canStart(std::string* error) const {
        std::lock_guard<std::mutex> lock(_runMutex);
        if (!_source) {
            if (error) *error = "Frame context source is not configured.";
            return false;
        }
        return true;
    }
    void cancelRun() noexcept {
        std::shared_ptr<AcquisitionRun> run;
        { std::lock_guard<std::mutex> lock(_runMutex); run = _run; }
        if (run) {
            run->cancelled.store(true, std::memory_order_release);
            std::lock_guard<std::recursive_mutex> lock(run->deliveryMutex);
        }
    }
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> dropped{0};

private:
    HeliotisC4Device* _device = nullptr;
    GraphicsFrameCallback _callback;
    FrameReceiptCallback _receiptCallback;
    RejectedRange _rejectedRange;
    RejectedImage _rejectedImage;
    HeliotisC4GraphicsFrameAdapter _adapter;
    mutable std::mutex _runMutex;
    std::shared_ptr<GraphicsFrameContextSource> _source;
    std::shared_ptr<AcquisitionRun> _run;
    StorageFailure _storageFailure;
    GraphicsFrameCallbackGate _callbackGate;
};

HeliotisGraphicsFrameStream::Impl::Impl(
    HeliotisC4Device* device,
    GraphicsFrameCallback callback,
    FrameReceiptCallback receiptCallback,
    RejectedRange rejectedRange,
    RejectedImage rejectedImage)
    : _device(device), _callback(std::move(callback)), _receiptCallback(std::move(receiptCallback)),
      _rejectedRange(std::move(rejectedRange)), _rejectedImage(std::move(rejectedImage))
{
}

HeliotisGraphicsFrameStream::Impl::~Impl()
{
    cancelRun();
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

    if (!canStart(errorMessage)) return false;
    if (_device->isAcquiring()) {
        if (errorMessage) *errorMessage = "Heliotis acquisition is already active.";
        return false;
    }
    cancelRun();
    auto run = std::make_shared<AcquisitionRun>();
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        if (!_source) {
            if (errorMessage) *errorMessage = "Frame context source is not configured.";
            return false;
        }
        if (_run) _run->cancelled.store(true, std::memory_order_release);
        run->source = _source;
        _run = run;
    }
    delivered.store(0);
    failed.store(0);
    dropped.store(0);
    const auto callbackToken = _callbackGate.token();
    const bool started = _device->startAcquisition(mode,
        [this, callbackToken, run](Frame&& sourceFrame) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease || run->cancelled.load(std::memory_order_acquire)) return;
            {
                std::lock_guard<std::recursive_mutex> delivery(run->deliveryMutex);
                if (run->cancelled.load(std::memory_order_acquire)) return;
                if (_receiptCallback) {
                    try { _receiptCallback(); } catch (...) {}
                }
            }
            try {
                HeliotisRangeRejection rangeRejection;
                HeliotisImageRejection imageRejection;
                GraphicsFrameError error = GraphicsFrameError::None;
                auto frame = _adapter.convertForRun(sourceFrame, heliotisGraphicsFrameRequest(),
                    *run->source, run->cancelled, error, rangeRejection, imageRejection);
                std::lock_guard<std::recursive_mutex> delivery(run->deliveryMutex);
                if (run->cancelled.load(std::memory_order_acquire)) return;
                if (error == GraphicsFrameError::Disconnected) return;
                if (error == GraphicsFrameError::NoReadyCredit) {
                    dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (error == GraphicsFrameError::ClassRefused || error == GraphicsFrameError::Capacity) {
                    // Only this frame is dropped and counted; the run keeps acquiring and the
                    // next frame asks the source again. The message is surfaced once per run.
                    if (!run->refusalSurfaced.exchange(true, std::memory_order_acq_rel) && _storageFailure)
                        _storageFailure();
                }
                if (_rejectedRange && rangeRejection.reason != HeliotisRangeRejection::Reason::None)
                    _rejectedRange(rangeRejection);
                if (_rejectedImage && imageRejection.reason != HeliotisImageRejection::Reason::None)
                    _rejectedImage(imageRejection);
                if (frame) {
                    _callback(std::move(*frame), 0U);
                    delivered.fetch_add(1, std::memory_order_relaxed);
                } else {
                    failed.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                if (!run->cancelled.load(std::memory_order_acquire))
                    failed.fetch_add(1, std::memory_order_relaxed);
            }
        }, errorMessage);
    if (!started) run->cancelled.store(true, std::memory_order_release);
    return started;
}

void HeliotisGraphicsFrameStream::Impl::requestStop() noexcept
{
    cancelRun();
    if (_device) _device->requestStopAcquisition();
}

void HeliotisGraphicsFrameStream::Impl::stop()
{
    cancelRun();
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
    : HeliotisGraphicsFrameStream(device, std::move(callback), std::move(receiptCallback), {}, {})
{
}

HeliotisGraphicsFrameStream::HeliotisGraphicsFrameStream(
    HeliotisC4Device* device,
    GraphicsFrameCallback callback,
    FrameReceiptCallback receiptCallback,
    RejectedRange rejectedRange,
    RejectedImage rejectedImage)
    : _impl(std::make_unique<Impl>(device, std::move(callback), std::move(receiptCallback),
                                   std::move(rejectedRange), std::move(rejectedImage)))
{
}

HeliotisGraphicsFrameStream::~HeliotisGraphicsFrameStream() = default;

void HeliotisGraphicsFrameStream::setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source)
{
    if (_impl) _impl->setFrameContextSource(std::move(source));
}

void HeliotisGraphicsFrameStream::setStorageFailureHandler(StorageFailure handler)
{
    _impl->setStorageFailureHandler(std::move(handler));
}

bool HeliotisGraphicsFrameStream::canStart(std::string* errorMessage) const
{
    return _impl->canStart(errorMessage);
}

std::uint64_t HeliotisGraphicsFrameStream::droppedFrames() const noexcept
{
    return _impl->dropped.load(std::memory_order_relaxed);
}

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

RuntimeDiagnostics HeliotisGraphicsFrameStream::runtimeDiagnostics() const noexcept
{
    return {_impl->acquiring(), _impl->delivered.load(), _impl->failed.load()};
}

} // namespace heliotis
