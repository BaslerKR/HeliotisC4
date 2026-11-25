#include "HeliotisC4SourceController.h"
#include "engine/GraphicsFrame.h"

#include "Utility/PlaygroundAdapter/HeliotisGraphicsFrameStream.h"
#include "Utility/PlaygroundAdapter/HeliotisC4GraphicsFrameAdapter.h"
#include "DeviceRuntime/Contract/SessionFrame.h"

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <QtConcurrentRun>

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <algorithm>

namespace {

/** Maps the Qt-free adapter reason to the exact host range rejection enum. */
DeviceRangeRejection deviceRangeRejection(const heliotis::HeliotisRangeRejection::Reason reason) noexcept
{
    using Reason = heliotis::HeliotisRangeRejection::Reason;
    switch (reason) {
    case Reason::MissingRange: return DeviceRangeRejection::MissingRange;
    case Reason::MissingData: return DeviceRangeRejection::MissingData;
    case Reason::InvalidDimensions: return DeviceRangeRejection::InvalidDimensions;
    case Reason::UnsupportedFormat: return DeviceRangeRejection::UnsupportedFormat;
    case Reason::InvalidLayout: return DeviceRangeRejection::InvalidLayout;
    case Reason::StrideBelowMinimum: return DeviceRangeRejection::StrideBelowMinimum;
    case Reason::TruncatedData: return DeviceRangeRejection::TruncatedData;
    case Reason::MissingCalibration: return DeviceRangeRejection::MissingCalibration;
    case Reason::InvalidCalibration: return DeviceRangeRejection::InvalidCalibration;
    case Reason::UnsupportedGeometry: return DeviceRangeRejection::UnsupportedGeometry;
    case Reason::UnsupportedUnit: return DeviceRangeRejection::UnsupportedUnit;
    case Reason::SizeLimitExceeded: return DeviceRangeRejection::SizeLimitExceeded;
    case Reason::StorageRefused: return DeviceRangeRejection::StorageRefused;
    case Reason::StorageMisaligned: return DeviceRangeRejection::StorageMisaligned;
    case Reason::None: break;
    }
    return DeviceRangeRejection::MissingRange;
}

/** Maps the Qt-free adapter image reason to the exact host enum; None is filtered by the caller. */
DeviceImageRejection deviceImageRejection(const heliotis::HeliotisImageRejection::Reason reason) noexcept
{
    using Reason = heliotis::HeliotisImageRejection::Reason;
    switch (reason) {
    case Reason::MissingBytes: return DeviceImageRejection::MissingBytes;
    case Reason::InvalidDimensions: return DeviceImageRejection::InvalidDimensions;
    case Reason::UnsupportedFormat: return DeviceImageRejection::UnsupportedFormat;
    case Reason::InvalidStride: return DeviceImageRejection::InvalidStride;
    case Reason::StrideBelowMinimum: return DeviceImageRejection::StrideBelowMinimum;
    case Reason::ExtentOverflow: return DeviceImageRejection::ExtentOverflow;
    case Reason::TruncatedBytes: return DeviceImageRejection::TruncatedBytes;
    case Reason::OddWidth: return DeviceImageRejection::OddWidth;
    case Reason::UnsupportedConversion: return DeviceImageRejection::UnsupportedConversion;
    case Reason::StorageRefused: return DeviceImageRejection::StorageRefused;
    case Reason::None: break;
    }
    return DeviceImageRejection::NoImage;
}

/** Maps the Qt-free adapter conversion path to the exact host enum. */
DeviceImageConversion deviceImageConversion(const heliotis::HeliotisImageRejection::Conversion conversion) noexcept
{
    using Conversion = heliotis::HeliotisImageRejection::Conversion;
    switch (conversion) {
    case Conversion::Borrow: return DeviceImageConversion::Borrow;
    case Conversion::Copy: return DeviceImageConversion::Copy;
    case Conversion::SwapByteOrder: return DeviceImageConversion::SwapByteOrder;
    case Conversion::JpegCopy: return DeviceImageConversion::JpegCopy;
    case Conversion::None: break;
    }
    return DeviceImageConversion::None;
}

} // namespace

/** One accepted `heliotis-c4.init` call; delivery happens exactly once. */
struct HeliotisC4SourceController::InitToolCall {
    QString callId;
    IToolProvider::Completion completion;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> delivered{false};

    /** Delivers the single completion unless it already ran or the session dropped it. */
    void deliver(ApiResult<QJsonValue> result) noexcept
    {
        if (delivered.exchange(true)) return;
        try {
            if (completion) completion(std::move(result));
        } catch (...) {
            qWarning().noquote() << "[Heliotis C4] Init tool consumer callback threw.";
        }
    }
};

/** One bounded parameter change subscription and its undelivered queue. */
struct HeliotisC4SourceController::ParameterWatch {
    std::string connectionId;
    std::string subscriptionId;
    IDeviceParameterControl::ChangeListener delivery;
    DeviceParameterChangeQueue queue{deviceParameterChangeQueueLimit};
    std::map<std::string, DeviceParameterDescriptor> delivered;
    std::uint64_t sequence = 0;
    std::atomic<bool> active{true};

    /** Builds the next change of one kind with this subscription's identities. */
    DeviceParameterChange change(DeviceParameterChangeKind kind) const
    {
        DeviceParameterChange value;
        value.kind = kind;
        value.connectionId = connectionId;
        value.subscriptionId = subscriptionId;
        value.sequence = sequence + 1;
        return value;
    }
};

HeliotisC4SourceController::HeliotisC4SourceController(heliotis::HeliotisC4Device* device, QObject* parent,
    DeviceRangeRejectionSink rangeRejectionSink, DeviceImageRejectionSink imageRejectionSink)
    : AbstractSourceController(parent),
      _device(device),
      _rangeRejectionSink(rangeRejectionSink),
      _imageRejectionSink(imageRejectionSink)
{
    if (!_device) return;
    _statusCallbackId = _device->registerStatusCallback([this](const heliotis::HeliotisC4Device::Status status,
                                                                const bool active) {
        if (status == heliotis::HeliotisC4Device::Status::Connection) {
            // Each open is a new parameter generation; stale targets conflict.
            {
                std::lock_guard<std::mutex> lock(_stateMutex);
                _connectionId = active ? QUuid::createUuid().toString(QUuid::WithoutBraces) : QString();
                _observations.clear();
            }
            if (!active) terminateParameterWatches(QStringLiteral("Device is disconnected."));
            QMetaObject::invokeMethod(this, [this] { emit controlChanged(); }, Qt::QueuedConnection);
            return;
        }
        if (status != heliotis::HeliotisC4Device::Status::Acquisition) return;
        const bool wasActive = _isGrabbing.exchange(active);
        if (!active && wasActive && _graphicsStream) {
            const auto diagnostics = _graphicsStream->runtimeDiagnostics();
            qInfo().noquote() << "[Heliotis C4] acquisition diagnostics: delivered="
                << _diagnosticDelivered.load() << "failed=" << diagnostics.failed;
        }
        emit diagnosticAcquisitionChanged(active, QStringLiteral("heliotis"));
        if (active) {
            _framesInCurrentAcquisition.store(0);
            _diagnosticDelivered.store(0);
        }
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
    heliotis::HeliotisGraphicsFrameStream::RejectedRange rejectedRange;
    if (_rangeRejectionSink) {
        rejectedRange = [this](const heliotis::HeliotisRangeRejection& rejection) {
            reportRangeRejection(rejection);
        };
    }
    heliotis::HeliotisGraphicsFrameStream::RejectedImage rejectedImage;
    if (_imageRejectionSink) {
        rejectedImage = [this](const heliotis::HeliotisImageRejection& rejection) {
            reportImageRejection(rejection);
        };
    }
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
        }, std::move(rejectedRange), std::move(rejectedImage));
}

void HeliotisC4SourceController::setFrameStorageProvider(std::shared_ptr<IFrameStorageProvider> provider)
{
    if (!_graphicsStream) return;
    _graphicsStream->setFrameStorageProvider(std::move(provider));
}

HeliotisC4SourceController::~HeliotisC4SourceController()
{
    setControlBackend(nullptr);
    {
        // No listener runs after controller destruction; the transport must
        // release its subscriptions before this point.
        std::lock_guard<std::mutex> lock(_watchMutex);
        for (const auto& entry : _parameterWatches) entry.second->active.store(false);
        _parameterWatches.clear();
    }
    // Accepted parameter work completes before the device facade is released.
    _parameterWork.waitForFinished();
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
    resetRejectionCounters(!continuous);
    std::string error;
    const bool started = _graphicsStream && _graphicsStream->start(mode, &error);
    if (!started) {
        _isContinuous.store(false);
        _softwareTriggerActive.store(false);
        const QString message = QString::fromStdString(error.empty()
            ? std::string("Heliotis acquisition could not be started.")
            : error);
        qWarning().noquote() << "[Heliotis C4]" << message;
        // Publish the observed inactive state before the error. A worker Host learns acquisition
        // state from this diagnostic signal; without it the proxy would keep its optimistic start.
        emit diagnosticAcquisitionChanged(false, QStringLiteral("heliotis"));
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

void HeliotisC4SourceController::resetRejectionCounters(const bool single) noexcept
{
    _singleInFlight.store(single, std::memory_order_relaxed);
    for (auto& counter : _rangeRejections) counter.store(0, std::memory_order_relaxed);
    for (auto& counter : _imageRejections) counter.store(0, std::memory_order_relaxed);
}

void HeliotisC4SourceController::reportRangeRejection(
    const heliotis::HeliotisRangeRejection& rejection) noexcept
{
    if (!_rangeRejectionSink
        || rejection.reason == heliotis::HeliotisRangeRejection::Reason::None) return;
    const auto reason = deviceRangeRejection(rejection.reason);
    if (!_singleInFlight.load(std::memory_order_relaxed)) {
        auto& counter = _rangeRejections[static_cast<std::size_t>(reason)];
        const auto count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count != 1 && count % 100 != 0) return;
    }
    DeviceRangeRejectionObservation observation;
    observation.reason = reason;
    observation.partCount = rejection.partCount;
    observation.affectedPart = rejection.affectedPart;
    observation.width = rejection.width;
    observation.height = rejection.height;
    observation.frameRetained = rejection.rangeRetained || rejection.imageRetained;
    observation.imageRetained = rejection.imageRetained;
    observation.rangeRetained = rejection.rangeRetained;
    observation.frameSequence = rejection.frameSequence;
    observation.readableBytes = rejection.readableBytes;
    observation.requiredBytes = rejection.requiredBytes;
    observation.requestedStorageBytes = rejection.requestedStorageBytes;
    _rangeRejectionSink(observation);
}

void HeliotisC4SourceController::reportImageRejection(
    const heliotis::HeliotisImageRejection& rejection) noexcept
{
    if (!_imageRejectionSink
        || rejection.reason == heliotis::HeliotisImageRejection::Reason::None) return;
    const auto reason = deviceImageRejection(rejection.reason);
    if (reason == DeviceImageRejection::NoImage) return;
    if (!_singleInFlight.load(std::memory_order_relaxed)) {
        auto& counter = _imageRejections[static_cast<std::size_t>(reason)];
        const auto count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count != 1 && count % 100 != 0) return;
    }
    DeviceImageRejectionObservation observation;
    observation.reason = reason;
    observation.conversion = deviceImageConversion(rejection.conversion);
    observation.width = rejection.width;
    observation.height = rejection.height;
    observation.directInput = rejection.directInput;
    observation.frameSequence = rejection.frameSequence;
    observation.pixelFormat = rejection.pixelFormat;
    observation.effectiveStride = rejection.effectiveStride;
    observation.minimumRowBytes = rejection.minimumRowBytes;
    observation.readableBytes = rejection.readableBytes;
    observation.requiredBytes = rejection.requiredBytes;
    _imageRejectionSink(observation);
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
    _deliveredFrames.fetch_add(1);
    _diagnosticDelivered.fetch_add(1);
}

// --- Device tools --------------------------------------------------------

/** One accepted tool request; delivery happens exactly once. */
struct HeliotisC4SourceController::ToolCall {
    IDeviceConnectionControl::Completion completion;
    IDeviceConnectionControl::TargetsCompletion targets;
    std::shared_ptr<QMetaObject::Connection> triggerConnection;
    std::atomic<bool> delivered{false};

    /** Delivers a state result, or a target catalog for discovery, exactly once. */
    void deliver(DeviceConnectionResult result, std::vector<heliotis::DeviceDescriptor> devices = {}) noexcept
    {
        if (delivered.exchange(true)) return;
        try {
            if (targets) {
                DeviceConnectionTargets copied{std::move(result), {}};
                if (copied.result.error == DeviceConnectionError::None) copied.result.state.reset();
                for (const auto& device : devices) copied.targets.push_back(target(device));
                targets(std::move(copied));
            } else {
                completion(std::move(result));
            }
        } catch (...) {
            qWarning().noquote() << "[Heliotis C4] Lifecycle consumer callback threw.";
        }
    }
    static DeviceConnectionTarget target(const heliotis::DeviceDescriptor& device);
};

namespace {

using ParameterError = DeviceParameterError;
using ParameterKind = DeviceParameterKind;

constexpr const char* kParameterScope = "Device";
constexpr auto kInitToolName = "heliotis-c4.init";
constexpr int kInitToolVersion = 1;
constexpr auto kInitConfirmationArgument = "confirmed";
constexpr auto kInitConfirmMessage = "StageInit can move the stage. Confirm that the cell is clear.";

/** Splits the SDK category text into the bounded tree path used by the API. */
std::vector<std::string> categorySegments(const std::string& path)
{
    std::vector<std::string> segments;
    std::size_t begin = 0;
    while (begin <= path.size() && segments.size() < deviceParameterCategoryDepthLimit) {
        const std::size_t end = path.find('/', begin);
        const std::size_t length = end == std::string::npos ? path.size() - begin : end - begin;
        std::string segment = path.substr(begin, length);
        // Trim whitespace and drop empty segments; the tree contract rejects both.
        const auto first = segment.find_first_not_of(" \t\r\n");
        const auto last = segment.find_last_not_of(" \t\r\n");
        segment = first == std::string::npos ? std::string{}
            : segment.substr(first, last - first + 1);
        if (segment.size() > deviceParameterCategorySegmentLimit) {
            std::size_t cut = deviceParameterCategorySegmentLimit;
            while (cut > 0 && (static_cast<unsigned char>(segment[cut]) & 0xC0U) == 0x80U) --cut;
            segment.resize(cut);
        }
        if (!segment.empty() && segment.find('\0') == std::string::npos)
            segments.push_back(std::move(segment));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return segments;
}

/** Derives the preferred editor from the feature kind. */
DeviceParameterRepresentation representationFor(const ParameterKind kind)
{
    switch (kind) {
    case ParameterKind::Integer:
    case ParameterKind::Float: return DeviceParameterRepresentation::Number;
    case ParameterKind::Boolean: return DeviceParameterRepresentation::Toggle;
    case ParameterKind::Enumeration: return DeviceParameterRepresentation::Choice;
    case ParameterKind::String: return DeviceParameterRepresentation::Text;
    case ParameterKind::Command: return DeviceParameterRepresentation::Automatic;
    }
    return DeviceParameterRepresentation::Automatic;
}

/** Encodes the exact discovery identity that `HeliotisC4Device::open()` re-verifies. */
QString targetId(const heliotis::DeviceDescriptor& device)
{
    const std::string key = device.interfaceName + '\0' + device.deviceName + '\0'
        + std::to_string(device.interfaceIndex) + '\0' + std::to_string(device.deviceIndex);
    return QString::fromLatin1(QByteArray(key.data(), qsizetype(key.size()))
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

DeviceConnectionResult refused(DeviceConnectionError error, const char* message)
{
    return {error, message};
}

DeviceParameterResult parameterFailure(const ParameterError error, std::string message)
{
    DeviceParameterResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

std::optional<ParameterKind> parameterKind(const heliotis::FeatureType type)
{
    switch (type) {
    case heliotis::FeatureType::Integer: return ParameterKind::Integer;
    case heliotis::FeatureType::Float: return ParameterKind::Float;
    case heliotis::FeatureType::Boolean: return ParameterKind::Boolean;
    case heliotis::FeatureType::String: return ParameterKind::String;
    case heliotis::FeatureType::Enumeration: return ParameterKind::Enumeration;
    case heliotis::FeatureType::Command: return ParameterKind::Command;
    default: return std::nullopt;
    }
}

/** Parses the SDK value text produced by `HeliotisC4Device::readFeatures()`. */
std::optional<DeviceParameterValue> parameterValue(const ParameterKind kind, const std::string& text)
{
    switch (kind) {
    case ParameterKind::Integer: {
        char* end = nullptr;
        errno = 0;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (text.empty() || errno == ERANGE || end == text.c_str() || *end != '\0') return std::nullopt;
        return DeviceParameterValue{static_cast<std::int64_t>(value)};
    }
    case ParameterKind::Float: {
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(text.c_str(), &end);
        if (text.empty() || errno == ERANGE || end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
            return std::nullopt;
        }
        return DeviceParameterValue{value};
    }
    case ParameterKind::Boolean:
        if (text == "1" || text == "true") return DeviceParameterValue{true};
        if (text == "0" || text == "false") return DeviceParameterValue{false};
        return std::nullopt;
    case ParameterKind::String:
    case ParameterKind::Enumeration:
        return DeviceParameterValue{text};
    default:
        return std::nullopt;
    }
}

/** Encodes a validated typed value as the text accepted by `writeFeature()`. */
std::string parameterText(const DeviceParameterValue& value)
{
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
    if (const auto* flag = std::get_if<bool>(&value)) return *flag ? "1" : "0";
    if (const auto* number = std::get_if<double>(&value)) {
        std::array<char, 64> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *number);
        if (converted.ec != std::errc{}) throw std::runtime_error("Float encoding failed.");
        return std::string(buffer.data(), converted.ptr);
    }
    return std::get<std::string>(value);
}

bool sameObservation(const DeviceParameterDescriptor& a, const DeviceParameterDescriptor& b)
{
    const auto fields = [](const auto& d) {
        return std::tie(d.kind, d.displayName, d.description, d.unit, d.available, d.readable,
            d.writable, d.unavailableReason, d.observationError, d.value, d.integerMinimum,
            d.integerMaximum, d.integerIncrement, d.floatMinimum, d.floatMaximum,
            d.floatIncrement, d.enumEntries, d.categoryPath, d.visibility, d.representation,
            d.selectorRelations, d.producerShared);
    };
    return fields(a) == fields(b);
}

template <class Callback, class Value>
void completeParameter(const Callback& callback, Value value) noexcept
{
    try {
        callback(std::move(value));
    } catch (...) {
        qWarning().noquote() << "[Heliotis C4] Parameter consumer callback threw.";
    }
}

const heliotis::FeatureDescriptor* findFeature(const heliotis::HeliotisC4::FeatureList& features,
                                               const std::string& name)
{
    for (const auto& feature : features) {
        if (feature.displayName == name) return &feature;
    }
    return nullptr;
}

} // namespace

DeviceConnectionTarget HeliotisC4SourceController::ToolCall::target(const heliotis::DeviceDescriptor& device)
{
    return {targetId(device).toStdString(), device.deviceName,
            {{"deviceName", device.deviceName}, {"interfaceName", device.interfaceName}}};
}

void HeliotisC4SourceController::setControlBackend(HeliotisC4ControlBackend* backend)
{
    Q_ASSERT(QThread::currentThread() == thread());
    _backend = backend;
    if (!backend && _toolCall) {
        const auto call = std::exchange(_toolCall, {});
        call->deliver({DeviceConnectionError::Unavailable, "The Heliotis session closed before the operation completed.",
                       DeviceConnectionEffect::Unknown});
        emit controlChanged();
    }
    if (!backend && _initToolCall) {
        const auto call = std::exchange(_initToolCall, {});
        call->deliver(ApiResult<QJsonValue>::failure(ApiErrorCode::Unavailable,
            QStringLiteral("The Heliotis session closed before Init completed."),
            QJsonObject{{QStringLiteral("effect"), QStringLiteral("unknown")}}));
        emit controlChanged();
    }
}

bool HeliotisC4SourceController::externalBusy() const noexcept
{
    return _parameterBusy.load() || _toolCall != nullptr || _initToolCall != nullptr;
}

QString HeliotisC4SourceController::connectionId() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _connectionId;
}

DeviceConnectionState HeliotisC4SourceController::connectionState() const
{
    const QString connection = connectionId();
    const bool connected = _device && _device->isOpened() && !connection.isEmpty();
    const auto target = connected && _backend ? _backend->connectedTarget() : std::nullopt;
    const bool reconnectRequired = connected && _device->requiresReconnect();
    DeviceConnectionState state{connected, connected && _isGrabbing.load() && _isContinuous.load(),
            connected ? connection.toStdString() : std::string(),
            target ? targetId(*target).toStdString() : std::string(),
            target ? target->deviceName : std::string(), _deliveredFrames.load()};
    if (reconnectRequired)
        state.error = "The previous acquisition did not stop cleanly. Disconnect and reconnect before another acquisition or Init.";
    if (connected) {
        const DeviceConnectionAvailability ready{true, {}};
        const DeviceConnectionAvailability acquire = reconnectRequired
            ? DeviceConnectionAvailability{false,
                "Reconnect after the failed acquisition stop before arming acquisition."}
            : ready;
        state.capabilities = DeviceConnectionAvailabilitySet{ready, ready, ready, ready, acquire, acquire};
    }
    return state;
}

DeviceConnectionCapabilities HeliotisC4SourceController::connectionCapabilities() const
{
    const DeviceConnectionAvailability availability = _backend ? DeviceConnectionAvailability{true, {}}
        : DeviceConnectionAvailability{false, "Device control has no session."};
    return {availability, availability, availability, availability, availability, availability};
}

DeviceConnectionResult HeliotisC4SourceController::requestConnectionTargets(TargetsCompletion completion)
{
    auto call = std::make_shared<ToolCall>();
    call->targets = std::move(completion);
    return admitLifecycle(Lifecycle::Targets, {}, std::move(call));
}

DeviceConnectionResult HeliotisC4SourceController::requestConnectionState(IDeviceConnectionControl::Completion completion)
{
    auto call = std::make_shared<ToolCall>();
    call->completion = std::move(completion);
    return admitLifecycle(Lifecycle::Connection, {}, std::move(call));
}

DeviceConnectionResult HeliotisC4SourceController::requestDiscover(TargetsCompletion completion)
{
    auto call = std::make_shared<ToolCall>();
    call->targets = std::move(completion);
    return admitLifecycle(Lifecycle::Discover, {}, std::move(call));
}

DeviceConnectionResult HeliotisC4SourceController::requestConnect(std::string targetId,
                                                                  IDeviceConnectionControl::Completion completion)
{
    auto call = std::make_shared<ToolCall>();
    call->completion = std::move(completion);
    return admitLifecycle(Lifecycle::Connect, std::move(targetId), std::move(call));
}

DeviceConnectionResult HeliotisC4SourceController::requestDisconnect(std::string connectionId,
                                                                     IDeviceConnectionControl::Completion completion)
{
    auto call = std::make_shared<ToolCall>();
    call->completion = std::move(completion);
    return admitLifecycle(Lifecycle::Disconnect, std::move(connectionId), std::move(call));
}

DeviceConnectionResult HeliotisC4SourceController::requestSingle(std::string connectionId,
                                                                 IDeviceConnectionControl::Completion completion)
{
    auto call = std::make_shared<ToolCall>();
    call->completion = std::move(completion);
    return admitLifecycle(Lifecycle::Single, std::move(connectionId), std::move(call));
}

DeviceConnectionResult HeliotisC4SourceController::requestLiveStart(std::string connectionId,
                                                                    IDeviceConnectionControl::Completion completion)
{
    auto call = std::make_shared<ToolCall>();
    call->completion = std::move(completion);
    return admitLifecycle(Lifecycle::LiveStart, std::move(connectionId), std::move(call));
}

DeviceConnectionResult HeliotisC4SourceController::requestLiveStop(std::string connectionId,
                                                                   IDeviceConnectionControl::Completion completion)
{
    auto call = std::make_shared<ToolCall>();
    call->completion = std::move(completion);
    return admitLifecycle(Lifecycle::LiveStop, std::move(connectionId), std::move(call));
}

DeviceConnectionResult HeliotisC4SourceController::admitLifecycle(const Lifecycle operation, const std::string& argument,
                                                                  std::shared_ptr<ToolCall> call)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!call->completion && !call->targets) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
    if (!_backend) return refused(DeviceConnectionError::Unavailable, "Device control has no session.");
    if (operation == Lifecycle::Targets) {
        call->deliver({}, _backend->targets());
        return {};
    }
    if (operation == Lifecycle::Connection) {
        call->deliver({{}, {}, {}, connectionState()});
        return {};
    }
    if (externalBusy() || _backend->controlBusy()) {
        return refused(DeviceConnectionError::Busy, "Another Heliotis GUI or API operation is active.");
    }

    const auto state = connectionState();
    std::size_t targetIndex = 0;
    if (operation == Lifecycle::Discover) {
        if (state.connected) return refused(DeviceConnectionError::Busy, "Disconnect before discovery.");
    } else if (operation == Lifecycle::Connect) {
        if (state.connected) return refused(DeviceConnectionError::Conflict, "Disconnect before connecting another device.");
        const auto devices = _backend->targets();
        bool found = false;
        for (std::size_t index = 0; index < devices.size() && !found; ++index) {
            if (targetId(devices[index]).toStdString() == argument) {
                targetIndex = index;
                found = true;
            }
        }
        if (!found) return refused(DeviceConnectionError::NotFound, "Exact discovered target was not found.");
    } else {
        if (!state.connected) return refused(DeviceConnectionError::Disconnected, "Device is disconnected.");
        if (state.connectionId != argument) {
            return refused(DeviceConnectionError::Conflict, "The connection changed; observe it again.");
        }
        if ((operation == Lifecycle::Single || operation == Lifecycle::LiveStart) && _isGrabbing.load()) {
            return refused(DeviceConnectionError::Busy, "Stop the active acquisition explicitly first.");
        }
        if (operation == Lifecycle::LiveStop && !_isGrabbing.load()) {
            call->deliver({{}, {}, {}, state});
            return {};
        }
    }

    _toolCall = call;
    emit controlChanged();
    const std::uint64_t framesBefore = _deliveredFrames.load();
    const QPointer<HeliotisC4SourceController> guard(this);
    // Single owns its software trigger: the device needs one after arming, and
    // the API call must complete only after the delivered frame (owner Q7).
    auto triggerFailure = std::make_shared<QString>();
    auto done = [guard, call, operation, framesBefore, triggerFailure](const QString& error) {
        QString resolved = error;
        if (resolved.isEmpty() && !triggerFailure->isEmpty()) resolved = *triggerFailure;
        if (guard) guard->finishTool(call, operation, resolved, framesBefore);
    };
    bool admitted = false;
    if (operation == Lifecycle::Discover) admitted = _backend->discover(done);
    else if (operation == Lifecycle::Connect) admitted = _backend->connectTarget(targetIndex, done);
    else if (operation == Lifecycle::Disconnect) admitted = _backend->disconnect(done);
    else if (operation == Lifecycle::Single) {
        admitted = _backend->arm(false, done);
        if (admitted) {
            auto triggered = std::make_shared<bool>(false);
            auto issueTrigger = [this, triggered, triggerFailure]() {
                if (*triggered) return;
                *triggered = true;
                if (!_device || !_device->isSoftwareTriggeredAcquisition()) return;
                std::string triggerError;
                if (_device->triggerSoftware(&triggerError)) return;
                *triggerFailure = triggerError.empty()
                    ? QStringLiteral("The Heliotis software trigger was refused.")
                    : QString::fromStdString(triggerError);
                _device->requestStopAcquisition();
            };
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = QObject::connect(this, &HeliotisC4SourceController::acquisitionStateChanged, this,
                [this, call, connection, issueTrigger](const bool active, const bool, const bool) {
                    if (!active) return;
                    QObject::disconnect(*connection);
                    if (_toolCall != call) return;
                    issueTrigger();
                });
            call->triggerConnection = connection;
            // The arm may already have reported active while this handler was
            // being installed; the observed device state is authoritative.
            if (_isGrabbing.load()) issueTrigger();
        }
    }
    else if (operation == Lifecycle::LiveStart) admitted = _backend->arm(true, done);
    else admitted = _backend->stop(done);
    if (!admitted) {
        if (_toolCall == call) _toolCall.reset();
        emit controlChanged();
        return refused(DeviceConnectionError::Busy, "The Heliotis session did not admit the operation.");
    }
    return {};
}

void HeliotisC4SourceController::finishTool(const std::shared_ptr<ToolCall>& call, const Lifecycle operation,
                                            const QString& error, const std::uint64_t framesBefore)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (_toolCall == call) _toolCall.reset();
    if (call->triggerConnection) {
        QObject::disconnect(*call->triggerConnection);
        call->triggerConnection.reset();
    }
    DeviceConnectionResult result{{}, {}, DeviceConnectionEffect::Applied, connectionState()};
    if (!error.isEmpty()) {
        // Discovery keeps its previous snapshot; device commands leave an uncertain effect.
        result = {DeviceConnectionError::BackendFailure, error.toStdString(),
                  operation == Lifecycle::Discover ? DeviceConnectionEffect::None : DeviceConnectionEffect::Unknown,
                  connectionState()};
    } else if (operation == Lifecycle::Single && _deliveredFrames.load() == framesBefore) {
        result = {DeviceConnectionError::BackendFailure, "Single acquisition stopped without a converted frame.",
                  DeviceConnectionEffect::Applied, connectionState()};
    }
    emit controlChanged();
    call->deliver(std::move(result), operation == Lifecycle::Discover && _backend ? _backend->targets()
                                                                                : std::vector<heliotis::DeviceDescriptor>{});
}

// --- Declared module actions ---------------------------------------------

std::optional<std::pair<ApiErrorCode, QString>> HeliotisC4SourceController::initUnavailable() const
{
    if (!_backend)
        return std::pair{ApiErrorCode::Unavailable, QStringLiteral("Device control has no session.")};
    if (!_device || !_device->isOpened() || connectionId().isEmpty())
        return std::pair{ApiErrorCode::Unavailable, QStringLiteral("Connect a device before Init.")};
    if (_device->requiresReconnect())
        return std::pair{ApiErrorCode::Unavailable,
            QStringLiteral("Reconnect after the failed acquisition stop before Init.")};
    if (_isGrabbing.load())
        return std::pair{ApiErrorCode::Busy, QStringLiteral("Stop the active acquisition before Init.")};
    if (_parameterBusy.load() || _initToolCall || _backend->controlBusy())
        return std::pair{ApiErrorCode::Busy, QStringLiteral("Another Heliotis operation is active.")};
    return std::nullopt;
}

ToolDescriptor HeliotisC4SourceController::initToolDescriptor() const
{
    ToolDescriptor descriptor;
    descriptor.name = QString::fromLatin1(kInitToolName);
    descriptor.version = kInitToolVersion;
    descriptor.title = QStringLiteral("Initialize H8");
    descriptor.description = QStringLiteral(
        "Apply the complete C4Utility H8 surface capture profile and run StageInit. "
        "This can move the stage; the call requires an explicit confirm. "
        "It is refused while acquisition is armed, while another operation is active, "
        "or before a device is connected.");
    descriptor.effect = ToolEffect::Device;
    descriptor.egress = ToolEgress::Metadata;
    descriptor.inputSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QString::fromLatin1(kInitConfirmationArgument), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("format"), QStringLiteral("confirm")},
                {QStringLiteral("description"), QString::fromLatin1(kInitConfirmMessage)}}},
        }},
        {QStringLiteral("additionalProperties"), false},
    };
    descriptor.outputSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("initialized"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("initialized")}},
        {QStringLiteral("additionalProperties"), false},
    };
    descriptor.presentation = {QString::fromLatin1(kInitToolName), QStringLiteral("setting"),
        QStringLiteral("device"), 35};
    // The top-row button is omitted by owner decision; the flow starts from this explicit
    // confirmation prompt, so the package declares the omission instead of the Host naming it.
    descriptor.presentation.panelOmitted = true;
    descriptor.cancellable = true;
    const auto unavailable = initUnavailable();
    descriptor.available = !unavailable.has_value();
    descriptor.unavailableReason = unavailable ? unavailable->second : QString{};
    return descriptor;
}

QList<ToolDescriptor> HeliotisC4SourceController::toolDescriptors() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return {initToolDescriptor()};
}

ApiResult<void> HeliotisC4SourceController::requestTool(const QString& name, const int version,
                                                       QJsonObject arguments, IToolProvider::Completion completion)
{
    return requestToolWithProgress({}, name, version, std::move(arguments), {}, std::move(completion));
}

ApiResult<void> HeliotisC4SourceController::requestToolWithProgress(const QString& callId, const QString& name,
    const int version, QJsonObject arguments, IToolExecution::Progress progress,
    IToolProvider::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (name != QLatin1String(kInitToolName))
        return ApiResult<void>::failure(ApiErrorCode::NotFound, QStringLiteral("The Heliotis module does not declare this tool."));
    if (version != kInitToolVersion)
        return ApiResult<void>::failure(ApiErrorCode::Unsupported, QStringLiteral("The Heliotis init tool version is unsupported."));
    if (!completion)
        return ApiResult<void>::failure(ApiErrorCode::InvalidArgument, QStringLiteral("A copied completion is required."));
    const auto confirmation = arguments.value(QString::fromLatin1(kInitConfirmationArgument));
    if (!confirmation.isUndefined() && !confirmation.isBool())
        return ApiResult<void>::failure(ApiErrorCode::InvalidArgument,
            QStringLiteral("The Init confirmation must be a boolean."));
    // A retry with the accepted call identity is a conflict even while the
    // first call is active; a different identity is refused as busy below.
    if (!callId.isEmpty() && _initToolCall && _initToolCall->callId == callId)
        return ApiResult<void>::failure(ApiErrorCode::Conflict,
            QStringLiteral("The same Init call identity is already active."));
    // Availability is checked before the prompt so an armed or absent device
    // is refused without asking the operator to confirm anything.
    const auto unavailable = initUnavailable();
    if (unavailable)
        return ApiResult<void>::failure(unavailable->first, unavailable->second);
    if (!confirmation.toBool()) {
        // The call made no changes; the Host renders the prompt and retries.
        completion(argumentRequiredFailure(QString::fromLatin1(kInitConfirmationArgument),
            QStringLiteral("confirm"), QString::fromLatin1(kInitConfirmMessage)));
        return ApiResult<void>::success();
    }
    if (!_backend)
        return ApiResult<void>::failure(ApiErrorCode::Unavailable, QStringLiteral("Device control has no session."));

    auto call = std::make_shared<InitToolCall>();
    call->callId = callId;
    call->completion = std::move(completion);
    _initToolCall = call;
    emit controlChanged();
    const QPointer<HeliotisC4SourceController> guard(this);
    auto done = [guard, call](const QString& error) {
        if (guard) guard->finishInitTool(call, error);
    };
    if (!_backend->initialize(done, [call] { return call->cancelled.load(); })) {
        if (_initToolCall == call) _initToolCall.reset();
        emit controlChanged();
        return ApiResult<void>::failure(ApiErrorCode::Busy,
            QStringLiteral("The Heliotis session did not admit Init."));
    }
    if (progress) {
        progress(ApiProgress{QStringLiteral("applying"),
            QStringLiteral("Applying the H8 defaults; StageInit can move the stage."), 0, 0, true});
    }
    return ApiResult<void>::success();
}

ApiResult<void> HeliotisC4SourceController::cancelTool(const QString& callId)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (callId.isEmpty() || !validApiIdentifier(callId))
        return ApiResult<void>::failure(ApiErrorCode::InvalidArgument,
            QStringLiteral("The accepted-call identity is invalid."));
    if (!_initToolCall || _initToolCall->callId != callId)
        return ApiResult<void>::failure(ApiErrorCode::NotFound,
            QStringLiteral("The Heliotis session holds no accepted Init call with that identity."));
    if (!_initToolCall->cancelled.exchange(true) && _device)
        _device->requestCancelInitialization();
    return ApiResult<void>::success();
}

void HeliotisC4SourceController::finishInitTool(const std::shared_ptr<InitToolCall>& call, const QString& error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (_initToolCall == call) _initToolCall.reset();
    emit controlChanged();
    if (error.isEmpty()) {
        call->deliver(ApiResult<QJsonValue>::success(QJsonObject{{QStringLiteral("initialized"), true}}));
        return;
    }
    if (call->cancelled.load()) {
        call->deliver(ApiResult<QJsonValue>::failure(ApiErrorCode::Cancelled, error,
            QJsonObject{{QStringLiteral("effect"), QStringLiteral("unknown")}}));
        return;
    }
    call->deliver(ApiResult<QJsonValue>::failure(ApiErrorCode::Internal, error,
        QJsonObject{{QStringLiteral("effect"), QStringLiteral("unknown")}}));
}

// --- Device parameters ---------------------------------------------------

DeviceParameterResult HeliotisC4SourceController::parameterAdmission(const bool softwareTrigger)
{
    Q_ASSERT(QThread::currentThread() == thread());
    // A pending device.single may wait for exactly this FrameStart command.
    if (_parameterBusy.load() || (!softwareTrigger && _toolCall) || (_backend && _backend->controlBusy())) {
        return parameterFailure(ParameterError::Busy, "A GUI or API control operation is active.");
    }
    if (!_device || !_device->isOpened() || connectionId().isEmpty()) {
        return parameterFailure(ParameterError::Disconnected, "Device is disconnected.");
    }
    if (_device->requiresReconnect()) {
        return parameterFailure(ParameterError::Unavailable, "Reconnect after the failed acquisition stop.");
    }
    if (_isGrabbing.load() && !softwareTrigger) {
        // C4 feature access shares the serialized SDK channel with buffer polling.
        return parameterFailure(ParameterError::Busy, "Parameter access is unavailable while acquisition is armed.");
    }
    return {};
}

std::optional<DeviceParameterDescriptor> HeliotisC4SourceController::observeParameter(
    const heliotis::FeatureDescriptor& feature, const std::string& connection)
{
    const auto kind = parameterKind(feature.type);
    if (!kind) return std::nullopt;
    DeviceParameterDescriptor result;
    result.target = {connection, kParameterScope, feature.displayName, {}};
    result.kind = *kind;
    result.displayName = feature.displayName;
    result.description = feature.description;
    const bool readableAccess = feature.access == heliotis::FeatureAccess::ReadOnly
        || feature.access == heliotis::FeatureAccess::ReadWrite;
    result.writable = feature.access == heliotis::FeatureAccess::WriteOnly
        || feature.access == heliotis::FeatureAccess::ReadWrite;
    result.readable = readableAccess && *kind != ParameterKind::Command;
    result.available = readableAccess || result.writable;
    if (!result.available) result.unavailableReason = "Feature is not currently accessible.";
    if (result.readable) {
        result.value = parameterValue(*kind, feature.valueText);
        if (!result.value) result.observationError = "The feature value could not be read.";
    }
    result.integerMinimum = feature.integerMinimum;
    result.integerMaximum = feature.integerMaximum;
    result.integerIncrement = feature.integerIncrement;
    result.floatMinimum = feature.floatMinimum;
    result.floatMaximum = feature.floatMaximum;
    result.floatIncrement = feature.floatIncrement;
    if (*kind == ParameterKind::Enumeration) result.enumEntries = feature.enumEntries;
    // The SDK-neutral feature contract exposes one category text; the API tree
    // path is its slash-separated form, bounded to the contract limits.
    result.categoryPath = categorySegments(feature.categoryPath);
    result.representation = representationFor(*kind);
    result.visibility = DeviceParameterVisibility::Primary;
    // C4 selects and refreshes dependent features through its SDK node map; the
    // neutral feature contract has no selector identity, so the consumer sees
    // structural changes through revisioned snapshots and deltas instead.
    result.selectorRelations.clear();
    result.producerShared = false;

    std::lock_guard<std::mutex> lock(_stateMutex);
    const auto key = connection + "/" + kParameterScope + "/" + feature.displayName;
    const auto previous = _observations.find(key);
    result.revision = previous != _observations.end() && sameObservation(previous->second, result)
        ? previous->second.revision : ++_parameterRevision;
    _observations[key] = result;
    return result;
}

DeviceParameterResult HeliotisC4SourceController::requestParameters(InventoryCompletion completion)
{
    if (!completion) return parameterFailure(ParameterError::InvalidArgument, "A completion is required.");
    const auto admission = parameterAdmission(false);
    if (admission.error != ParameterError::None) return admission;
    const std::string connection = connectionId().toStdString();
    _parameterBusy.store(true);
    emit controlChanged();
    _parameterWork = QtConcurrent::run([this, connection, completion = std::move(completion)] {
        DeviceParameterInventory inventory;
        try {
            std::string error;
            const auto features = _device->readFeatures(&error);
            if (!error.empty()) {
                inventory.result = parameterFailure(ParameterError::BackendFailure, error);
            } else {
                for (const auto& feature : features) {
                    if (auto descriptor = observeParameter(feature, connection)) {
                        inventory.parameters.push_back(std::move(*descriptor));
                    }
                }
            }
        } catch (const std::exception& error) {
            inventory.result = parameterFailure(ParameterError::BackendFailure, error.what());
            inventory.parameters.clear();
        } catch (...) {
            inventory.result = parameterFailure(ParameterError::BackendFailure, "Unexpected feature inventory failure.");
            inventory.parameters.clear();
        }
        _parameterBusy.store(false);
        emit controlChanged();
        completeParameter(completion, std::move(inventory));
    });
    return {};
}

DeviceParameterResult HeliotisC4SourceController::requestReadParameter(
    DeviceParameterTarget target, IDeviceParameterControl::Completion completion)
{
    return requestParameter(std::move(target), std::nullopt, std::nullopt, false, std::move(completion));
}

DeviceParameterResult HeliotisC4SourceController::requestWriteParameter(
    DeviceParameterTarget target, DeviceParameterValue value, const std::optional<std::uint64_t> expectedRevision,
    IDeviceParameterControl::Completion completion)
{
    return requestParameter(std::move(target), std::move(value), expectedRevision, true, std::move(completion));
}

DeviceParameterResult HeliotisC4SourceController::requestExecuteParameter(
    DeviceParameterTarget target, const std::optional<std::uint64_t> expectedRevision,
    IDeviceParameterControl::Completion completion)
{
    return requestParameter(std::move(target), std::nullopt, expectedRevision, true, std::move(completion));
}

DeviceParameterResult HeliotisC4SourceController::requestParameter(
    DeviceParameterTarget target, std::optional<DeviceParameterValue> value,
    const std::optional<std::uint64_t> expectedRevision, const bool mutate,
    IDeviceParameterControl::Completion completion)
{
    if (!completion) return parameterFailure(ParameterError::InvalidArgument, "A completion is required.");
    for (const auto* text : {&target.connectionId, &target.scope, &target.name}) {
        if (text->empty() || text->find('\0') != std::string::npos) {
            return parameterFailure(ParameterError::InvalidArgument, "Exact target fields are required.");
        }
    }
    if (target.scope != kParameterScope) {
        return parameterFailure(ParameterError::Unsupported, "Exact Device scope is required.");
    }
    if (!target.selectors.empty()) {
        return parameterFailure(ParameterError::Conflict, "Heliotis features have no selector context.");
    }
    // TriggerSoftware is the acquisition command; it is the only call admitted
    // while armed and uses the FrameStart hot path without feature reads.
    const bool softwareTrigger = mutate && !value && target.name == "TriggerSoftware" && _isGrabbing.load();
    const auto admission = parameterAdmission(softwareTrigger);
    if (admission.error != ParameterError::None) return admission;
    if (target.connectionId != connectionId().toStdString()) {
        return parameterFailure(ParameterError::Conflict, "Connection generation is not active.");
    }
    if (const auto* text = value ? std::get_if<std::string>(&*value) : nullptr;
        text && (text->empty() || text->find_first_not_of(" \t\r\n") != 0
            || text->find_last_not_of(" \t\r\n") != text->size() - 1)) {
        return parameterFailure(ParameterError::InvalidArgument,
            "Heliotis features accept only non-empty string values without surrounding whitespace.");
    }

    _parameterBusy.store(true);
    emit controlChanged();
    _parameterWork = QtConcurrent::run([this, target = std::move(target), value = std::move(value),
                                        expectedRevision, mutate, softwareTrigger,
                                        completion = std::move(completion)] {
        DeviceParameterResult result;
        bool dispatched = false;
        bool applied = false;
        try {
            if (softwareTrigger) {
                std::string error;
                dispatched = true;
                applied = _device->triggerSoftware(&error);
                result.effect = applied ? DeviceParameterEffect::Applied : DeviceParameterEffect::None;
                if (!applied) result = parameterFailure(ParameterError::Unavailable, error);
            } else {
                std::string error;
                const auto features = _device->readFeatures(&error);
                const auto* feature = error.empty() ? findFeature(features, target.name) : nullptr;
                if (!error.empty()) result = parameterFailure(ParameterError::BackendFailure, error);
                else if (!feature) result = parameterFailure(ParameterError::NotFound, "Parameter was not found.");
                else if (!parameterKind(feature->type)) {
                    result = parameterFailure(ParameterError::Unsupported, "Feature is not a typed parameter.");
                } else {
                    const auto current = observeParameter(*feature, target.connectionId);
                    result.descriptor = current;
                    if (!mutate) {
                        if (!current->available) result.error = ParameterError::Unavailable;
                        else if (!current->readable) result.error = ParameterError::Unreadable;
                        else if (!current->observationError.empty()) result.error = ParameterError::BackendFailure;
                    } else {
                        result.error = validateDeviceParameterWrite(*current, target, value, expectedRevision);
                        if (result.error == ParameterError::None) {
                            dispatched = true;
                            applied = value
                                ? _device->writeFeature(target.name, parameterText(*value), &error)
                                : _device->executeCommand(target.name, &error);
                            result.descriptor.reset();
                            if (!applied) {
                                // C4Utility reports one failure for refusal and SDK errors.
                                result.error = ParameterError::BackendFailure;
                                result.effect = DeviceParameterEffect::Unknown;
                                result.message = error;
                            } else {
                                result.effect = DeviceParameterEffect::Applied;
                                std::string readbackError;
                                const auto readback = _device->readFeatures(&readbackError);
                                if (readbackError.empty()) {
                                    // A selector write can change other features;
                                    // publish the complete re-observed tree.
                                    publishParameterSnapshot(readback);
                                }
                                if (const auto* observed = readbackError.empty()
                                        ? findFeature(readback, target.name) : nullptr) {
                                    result.descriptor = observeParameter(*observed, target.connectionId);
                                }
                            }
                        }
                    }
                }
            }
            if (result.error != ParameterError::None && result.message.empty()) {
                result.message = "Feature access, value or revision validation failed.";
            }
        } catch (const std::exception& error) {
            result.error = ParameterError::BackendFailure;
            result.message = error.what();
            if (dispatched && !applied) {
                result.effect = DeviceParameterEffect::Unknown;
                result.descriptor.reset();
            }
        } catch (...) {
            result.error = ParameterError::BackendFailure;
            result.message = "Unexpected parameter backend failure.";
            if (dispatched && !applied) {
                result.effect = DeviceParameterEffect::Unknown;
                result.descriptor.reset();
            }
        }
        _parameterBusy.store(false);
        emit controlChanged();
        if (applied && !softwareTrigger) emit parametersApplied();
        completeParameter(completion, std::move(result));
    });
    return {};
}

// --- Parameter change subscriptions --------------------------------------

bool HeliotisC4SourceController::parameterWatchesActive() const
{
    std::lock_guard<std::mutex> lock(_watchMutex);
    return !_parameterWatches.empty();
}

void HeliotisC4SourceController::scheduleParameterDrain()
{
    {
        std::lock_guard<std::mutex> lock(_watchMutex);
        if (_parameterDrainScheduled) return;
        _parameterDrainScheduled = true;
    }
    QMetaObject::invokeMethod(this, [this] { drainParameterChanges(); }, Qt::QueuedConnection);
}

void HeliotisC4SourceController::enqueueParameterChange(const std::shared_ptr<ParameterWatch>& watch,
    DeviceParameterChange change, std::string overflowReason)
{
    if (!watch || !watch->active.load()) return;
    const bool queued = watch->queue.push(std::move(change));
    if (!queued) {
        // The bounded queue refuses the new delta; replace every pending delta
        // with one explicit resync snapshot so no sequence gap is silent.
        DeviceParameterChange snapshot;
        snapshot.kind = DeviceParameterChangeKind::Snapshot;
        snapshot.connectionId = watch->connectionId;
        snapshot.subscriptionId = watch->subscriptionId;
        snapshot.sequence = ++watch->sequence;
        std::uint64_t revision = 0;
        for (const auto& entry : watch->delivered) {
            snapshot.descriptors.push_back(entry.second);
            revision = std::max(revision, entry.second.revision);
        }
        snapshot.revision = revision;
        watch->queue.replaceWithSnapshot(std::move(snapshot), std::move(overflowReason));
    }
}

void HeliotisC4SourceController::drainParameterChanges()
{
    {
        std::lock_guard<std::mutex> lock(_watchMutex);
        _parameterDrainScheduled = false;
    }
    std::vector<std::shared_ptr<ParameterWatch>> watches;
    {
        std::lock_guard<std::mutex> lock(_watchMutex);
        watches.reserve(_parameterWatches.size());
        for (const auto& entry : _parameterWatches) watches.push_back(entry.second);
    }
    for (const auto& watch : watches) {
        if (!watch->active.load()) continue;
        while (auto change = watch->queue.pop()) {
            if (!watch->active.load()) break;
            if (!watch->delivery) continue;
            try {
                watch->delivery(std::move(*change));
            } catch (...) {
                qWarning().noquote() << "[Heliotis C4] Parameter change consumer callback threw.";
                watch->active.store(false);
                break;
            }
        }
    }
}

void HeliotisC4SourceController::terminateParameterWatches(const QString& message)
{
    std::vector<std::shared_ptr<ParameterWatch>> watches;
    {
        std::lock_guard<std::mutex> lock(_watchMutex);
        watches.reserve(_parameterWatches.size());
        for (const auto& entry : _parameterWatches) watches.push_back(entry.second);
        _parameterWatches.clear();
    }
    for (const auto& watch : watches) {
        watch->active.store(false);
        while (watch->queue.pop()) {}
        if (!watch->delivery) continue;
        DeviceParameterChange terminal;
        terminal.kind = DeviceParameterChangeKind::Delta;
        terminal.connectionId = watch->connectionId;
        terminal.subscriptionId = watch->subscriptionId;
        terminal.sequence = ++watch->sequence;
        terminal.error = DeviceParameterError::Disconnected;
        terminal.message = message.left(int(deviceParameterChangeTextLimit)).toStdString();
        try {
            watch->delivery(std::move(terminal));
        } catch (...) {
            qWarning().noquote() << "[Heliotis C4] Parameter change consumer callback threw.";
        }
    }
}

DeviceParameterResult HeliotisC4SourceController::requestParameterChanges(
    std::string connectionId, std::string subscriptionId, ChangeListener delivery)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (validateDeviceParameterSubscription(connectionId, subscriptionId) != DeviceParameterError::None
        || !delivery) {
        return parameterFailure(ParameterError::InvalidArgument,
            "The parameter change subscription identity is invalid.");
    }
    if (!_backend)
        return parameterFailure(ParameterError::Unavailable, "Device control has no session.");
    if (!_device || !_device->isOpened() || this->connectionId().isEmpty())
        return parameterFailure(ParameterError::Disconnected, "Device is disconnected.");
    if (connectionId != this->connectionId().toStdString())
        return parameterFailure(ParameterError::Conflict, "Connection generation is not active.");
    if (_device->requiresReconnect())
        return parameterFailure(ParameterError::Unavailable,
            "Reconnect after the failed acquisition stop.");
    if (_isGrabbing.load())
        return parameterFailure(ParameterError::Busy,
            "Parameter observation is unavailable while acquisition is armed.");
    if (_parameterBusy.load() || _toolCall || _initToolCall
        || (_backend && _backend->controlBusy())) {
        return parameterFailure(ParameterError::Busy, "A GUI or API control operation is active.");
    }
    auto watch = std::make_shared<ParameterWatch>();
    watch->connectionId = connectionId;
    watch->subscriptionId = subscriptionId;
    watch->delivery = std::move(delivery);
    {
        std::lock_guard<std::mutex> lock(_watchMutex);
        if (_parameterWatches.count(subscriptionId))
            return parameterFailure(ParameterError::Busy,
                "The parameter change subscription is already active.");
        if (_parameterWatches.size() >= deviceParameterSubscriptionLimit)
            return parameterFailure(ParameterError::Busy,
                "The parameter change subscription bound is reached.");
        _parameterWatches.emplace(subscriptionId, watch);
    }

    _parameterBusy.store(true);
    emit controlChanged();
    _parameterWork = QtConcurrent::run([this, watch] {
        heliotis::HeliotisC4::FeatureList features;
        QString failure;
        try {
            std::string error;
            features = _device->readFeatures(&error);
            if (!error.empty()) failure = QString::fromStdString(error);
        } catch (const std::exception& error) {
            failure = QString::fromLocal8Bit(error.what());
        } catch (...) {
            failure = QStringLiteral("Unexpected Heliotis feature read failure.");
        }
        if (!watch->active.load()) {
            _parameterBusy.store(false);
            emit controlChanged();
            return;
        }
        if (!failure.isEmpty()) {
            watch->active.store(false);
            DeviceParameterChange terminal;
            terminal.kind = DeviceParameterChangeKind::Delta;
            terminal.connectionId = watch->connectionId;
            terminal.subscriptionId = watch->subscriptionId;
            terminal.sequence = ++watch->sequence;
            terminal.error = DeviceParameterError::BackendFailure;
            terminal.message = failure.left(int(deviceParameterChangeTextLimit)).toStdString();
            try {
                watch->delivery(std::move(terminal));
            } catch (...) {
                qWarning().noquote() << "[Heliotis C4] Parameter change consumer callback threw.";
            }
            _parameterBusy.store(false);
            emit controlChanged();
            return;
        }
        std::vector<DeviceParameterDescriptor> descriptors;
        std::uint64_t revision = 0;
        for (const auto& feature : features) {
            if (auto descriptor = observeParameter(feature, watch->connectionId)) {
                revision = std::max(revision, descriptor->revision);
                descriptors.push_back(std::move(*descriptor));
            }
        }
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(_watchMutex);
            if (!watch->active.load()) {
                // Subscription was released while the snapshot read ran.
            } else {
                watch->delivered.clear();
                for (const auto& descriptor : descriptors)
                    watch->delivered[descriptor.target.name] = descriptor;
                DeviceParameterChange snapshot = watch->change(DeviceParameterChangeKind::Snapshot);
                watch->sequence = snapshot.sequence;
                snapshot.revision = revision;
                snapshot.descriptors = std::move(descriptors);
                queued = watch->queue.push(std::move(snapshot));
                if (!queued) {
                    DeviceParameterChange replacement = watch->change(DeviceParameterChangeKind::Snapshot);
                    watch->sequence = replacement.sequence;
                    replacement.revision = revision;
                    for (const auto& entry : watch->delivered)
                        replacement.descriptors.push_back(entry.second);
                    watch->queue.replaceWithSnapshot(std::move(replacement),
                        "The Heliotis parameter change queue overflowed.");
                    queued = true;
                }
            }
        }
        if (queued) scheduleParameterDrain();
        _parameterBusy.store(false);
        emit controlChanged();
    });
    return {};
}

DeviceParameterResult HeliotisC4SourceController::releaseParameterChanges(
    std::string connectionId, std::string subscriptionId, IDeviceParameterControl::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion)
        return parameterFailure(ParameterError::InvalidArgument, "A completion is required.");
    if (validateDeviceParameterSubscription(connectionId, subscriptionId) != DeviceParameterError::None)
        return parameterFailure(ParameterError::InvalidArgument,
            "The parameter change subscription identity is invalid.");
    std::shared_ptr<ParameterWatch> watch;
    {
        std::lock_guard<std::mutex> lock(_watchMutex);
        const auto found = _parameterWatches.find(subscriptionId);
        if (found == _parameterWatches.end() || found->second->connectionId != connectionId)
            return parameterFailure(ParameterError::NotFound,
                "The parameter change subscription is not active.");
        watch = found->second;
        _parameterWatches.erase(found);
    }
    watch->active.store(false);
    while (watch->queue.pop()) {}
    completeParameter(completion, DeviceParameterResult{});
    return {};
}

void HeliotisC4SourceController::publishParameterSnapshot(
    const heliotis::HeliotisC4::FeatureList& features)
{
    const std::string connection = connectionId().toStdString();
    if (connection.empty()) return;
    std::vector<DeviceParameterDescriptor> descriptors;
    std::uint64_t revision = 0;
    for (const auto& feature : features) {
        if (auto descriptor = observeParameter(feature, connection)) {
            revision = std::max(revision, descriptor->revision);
            descriptors.push_back(std::move(*descriptor));
        }
    }

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(_watchMutex);
        for (auto it = _parameterWatches.cbegin(); it != _parameterWatches.cend(); ++it) {
            const auto& watch = it->second;
            if (!watch->active.load() || watch->connectionId != connection) continue;
            std::vector<DeviceParameterDescriptor> changed;
            std::vector<DeviceParameterTarget> invalidated;
            for (const auto& descriptor : descriptors) {
                const auto previous = watch->delivered.find(descriptor.target.name);
                if (previous == watch->delivered.end() || !sameObservation(previous->second, descriptor))
                    changed.push_back(descriptor);
            }
            for (const auto& entry : watch->delivered) {
                const auto current = std::find_if(descriptors.begin(), descriptors.end(),
                    [&entry](const DeviceParameterDescriptor& descriptor) {
                        return descriptor.target.name == entry.first;
                    });
                if (current == descriptors.end()) invalidated.push_back(entry.second.target);
            }
            if (changed.empty() && invalidated.empty()) continue;
            if (changed.size() > deviceParameterDeltaDescriptorLimit
                || invalidated.size() > deviceParameterDeltaInvalidationLimit) {
                DeviceParameterChange snapshot = watch->change(DeviceParameterChangeKind::Snapshot);
                watch->sequence = snapshot.sequence;
                snapshot.revision = revision;
                snapshot.descriptors = descriptors;
                watch->delivered.clear();
                for (const auto& descriptor : descriptors)
                    watch->delivered[descriptor.target.name] = descriptor;
                enqueueParameterChange(watch, std::move(snapshot),
                    "The Heliotis parameter change queue overflowed.");
                queued = true;
                continue;
            }
            DeviceParameterChange delta = watch->change(DeviceParameterChangeKind::Delta);
            watch->sequence = delta.sequence;
            delta.revision = revision;
            delta.descriptors = std::move(changed);
            delta.invalidated = std::move(invalidated);
            for (const auto& descriptor : delta.descriptors)
                watch->delivered[descriptor.target.name] = descriptor;
            for (const auto& target : delta.invalidated) watch->delivered.erase(target.name);
            enqueueParameterChange(watch, std::move(delta),
                "The Heliotis parameter change queue overflowed.");
            queued = true;
        }
    }
    if (queued) scheduleParameterDrain();
}

std::optional<SourceRuntimeDiagnostics> HeliotisC4SourceController::sourceRuntimeDiagnostics(unsigned sourceIndex) const
{
    if (sourceIndex != 0 || !_graphicsStream) return std::nullopt;
    const auto value = _graphicsStream->runtimeDiagnostics();
    return SourceRuntimeDiagnostics{_isGrabbing.load(), _diagnosticDelivered.load(), value.failed};
}
