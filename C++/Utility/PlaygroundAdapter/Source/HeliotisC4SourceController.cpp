#include "HeliotisC4SourceController.h"
#include "engine/GraphicsFrame.h"

#include "Utility/PlaygroundAdapter/HeliotisGraphicsFrameStream.h"
#include "SessionFrame.h"

#include <QDebug>
#include <QMetaObject>

HeliotisC4SourceController::HeliotisC4SourceController(heliotis::HeliotisC4Device* device, QObject* parent)
    : AbstractSourceController(parent),
      _device(device)
{
    if (!_device) return;
    _statusCallbackId = _device->registerStatusCallback([this](const heliotis::HeliotisC4Device::Status status,
                                                                const bool active) {
        if (status != heliotis::HeliotisC4Device::Status::Acquisition) return;
        _isGrabbing.store(active);
        if (active) _framesInCurrentAcquisition.store(0);
        const bool continuous = _isContinuous.load();
        if (!active) _isContinuous.store(false);
        const std::uint64_t generation = _statusGeneration.fetch_add(1) + 1;
        // TriggerSoftware is available only when FrameStart is On/Software.
        // Keep the UI disabled for free-run or hardware-triggered acquisition.
        const bool softwareTriggerAvailable = active && _device->isSoftwareTriggerAvailable();
        _softwareTriggerActive.store(softwareTriggerAvailable);
        const QString error = active ? QString{} : QString::fromStdString(_device->lastAcquisitionError());
        QMetaObject::invokeMethod(this, [this, active, continuous, softwareTriggerAvailable, error, generation]() {
            if (_statusGeneration.load() != generation) {
                qInfo().noquote() << "[Heliotis C4] Ignored stale queued acquisition state:"
                                  << (active ? "armed" : "stopped")
                                  << "generation=" << generation
                                  << "currentGeneration=" << _statusGeneration.load();
                return;
            }
            qInfo().noquote() << "[Heliotis C4] Acquisition state:" << (active ? "armed" : "stopped")
                              << "mode=" << (continuous ? "Live" : "Single")
                              << "softwareTrigger=" << softwareTriggerAvailable
                              << "generation=" << generation;
            emit acquisitionStateChanged(active, continuous, softwareTriggerAvailable);
            if (!active && !error.isEmpty()) emit acquisitionError(error);
        }, Qt::QueuedConnection);
    });
    _graphicsStream = std::make_unique<heliotis::HeliotisGraphicsFrameStream>(
        _device,
        [this](GraphicsFrame&& frame, const unsigned int sourceIndex) {
            handleFrame(std::move(frame), sourceIndex);
        },
        [this]() {
            // Single disarms after receipt; only Live may enable another trigger.
            if (_softwareTriggerActive.load() && _isContinuous.load()) {
                emit acquisitionFrameReceived();
            }
        });
}

HeliotisC4SourceController::~HeliotisC4SourceController()
{
    stop();
    if (_device && _statusCallbackId != 0) {
        _device->deregisterStatusCallback(_statusCallbackId);
        _statusCallbackId = 0;
    }
}

void HeliotisC4SourceController::start()
{
    static_cast<void>(startLive());
}

bool HeliotisC4SourceController::startLive()
{
    qInfo().noquote() << "[Heliotis C4] Live acquisition requested.";
    return startAcquisition(heliotis::HeliotisC4Device::AcquisitionMode::Continuous);
}

void HeliotisC4SourceController::stop()
{
    qInfo().noquote() << "[Heliotis C4] Synchronous acquisition stop requested for teardown.";
    if (_graphicsStream) _graphicsStream->stop();
    else if (_device) _device->stopAcquisition();
    _isGrabbing.store(false);
    _isContinuous.store(false);
    _softwareTriggerActive.store(false);
}

void HeliotisC4SourceController::requestStop()
{
    qInfo().noquote() << "[Heliotis C4] Non-blocking acquisition stop requested from the UI.";
    if (_graphicsStream) _graphicsStream->requestStop();
    else if (_device) _device->requestStopAcquisition();
}

bool HeliotisC4SourceController::isGrabbing() const
{
    return _isGrabbing.load();
}

bool HeliotisC4SourceController::grabOne()
{
    qInfo().noquote() << "[Heliotis C4] Single acquisition requested.";
    return startAcquisition(heliotis::HeliotisC4Device::AcquisitionMode::SingleFrame);
}

void HeliotisC4SourceController::setFrameConsumer(FrameConsumer consumer)
{
    _frameConsumer = std::move(consumer);
}

bool HeliotisC4SourceController::supports3D() const
{
    return true;
}

bool HeliotisC4SourceController::startAcquisition(const heliotis::HeliotisC4Device::AcquisitionMode mode)
{
    if (!_device || _isGrabbing.load()) {
        qWarning().noquote() << "[Heliotis C4] Acquisition request ignored because the device is unavailable or already armed.";
        return false;
    }

    const bool continuous = mode == heliotis::HeliotisC4Device::AcquisitionMode::Continuous;
    _isContinuous.store(continuous);
    _framesInCurrentAcquisition.store(0);
    std::string error;
    const bool started = _graphicsStream && _graphicsStream->start(mode, &error);
    if (!started) {
        _isContinuous.store(false);
        _softwareTriggerActive.store(false);
        const QString message = QString::fromStdString(error.empty()
            ? std::string("Heliotis acquisition could not be started.")
            : error);
        qWarning().noquote() << "[Heliotis C4]" << message;
        emit acquisitionStateChanged(false, false, false);
        emit acquisitionError(message);
    }
    else {
        const bool softwareTriggerAvailable = _device->isSoftwareTriggerAvailable();
        qInfo().noquote() << "[Heliotis C4] Acquisition arm completed."
                          << (continuous ? "Live" : "Single")
                          << (softwareTriggerAvailable
                              ? "FrameStart waits for an explicit TriggerSoftware command; the arm action issues no trigger."
                              : "Frames are automatic or externally triggered; the host arm action issues no trigger.");
    }
    return started;
}

void HeliotisC4SourceController::handleFrame(GraphicsFrame&& payload, const unsigned int sourceIndex)
{
    const std::uint64_t armFrameCount = _framesInCurrentAcquisition.fetch_add(1) + 1;
    const bool sampledDiagnostics = !_isContinuous.load()
        || armFrameCount <= 3 || armFrameCount % 100 == 0;
    if (sampledDiagnostics) {
        qInfo().noquote() << "[Heliotis C4] Delivering GraphicsFrame"
                          << payload.metadata.frameIndex
                          << "armFrameCount=" << armFrameCount
                          << "range=" << payload.hasRangeFrame();
    }
    if (!_frameConsumer) return;
    SessionFrame frame;
    frame.payload = std::move(payload);
    frame.frameSeq = frame.payload.metadata.frameIndex;
    _frameConsumer(std::move(frame), sourceIndex);
}
