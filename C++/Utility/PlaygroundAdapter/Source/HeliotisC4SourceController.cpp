#include "HeliotisC4SourceController.h"
#include "engine/GraphicsFrame.h"

#include "Utility/PlaygroundAdapter/HeliotisGraphicsFrameStream.h"
#include "SessionFrame.h"

#include <QByteArray>
#include <QDebug>
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

HeliotisC4SourceController::HeliotisC4SourceController(heliotis::HeliotisC4Device* device, QObject* parent)
    : AbstractSourceController(parent),
      _device(device)
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
    setControlBackend(nullptr);
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
    _deliveredFrames.fetch_add(1);
    _diagnosticDelivered.fetch_add(1);
}

// --- Device tools --------------------------------------------------------

/** One accepted tool request; delivery happens exactly once. */
struct HeliotisC4SourceController::ToolCall {
    IDeviceConnectionControl::Completion completion;
    IDeviceConnectionControl::TargetsCompletion targets;
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
            d.floatIncrement, d.enumEntries);
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
}

bool HeliotisC4SourceController::externalBusy() const noexcept
{
    return _parameterBusy.load() || _toolCall != nullptr;
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
    return {connected, connected && _isGrabbing.load() && _isContinuous.load(),
            connected ? connection.toStdString() : std::string(),
            target ? targetId(*target).toStdString() : std::string(),
            target ? target->deviceName : std::string(), _deliveredFrames.load()};
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
    auto done = [guard, call, operation, framesBefore](const QString& error) {
        if (guard) guard->finishTool(call, operation, error, framesBefore);
    };
    bool admitted = false;
    if (operation == Lifecycle::Discover) admitted = _backend->discover(done);
    else if (operation == Lifecycle::Connect) admitted = _backend->connectTarget(targetIndex, done);
    else if (operation == Lifecycle::Disconnect) admitted = _backend->disconnect(done);
    else if (operation == Lifecycle::Single) admitted = _backend->arm(false, done);
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

std::optional<SourceRuntimeDiagnostics> HeliotisC4SourceController::sourceRuntimeDiagnostics(unsigned sourceIndex) const
{
    if (sourceIndex != 0 || !_graphicsStream) return std::nullopt;
    const auto value = _graphicsStream->runtimeDiagnostics();
    return SourceRuntimeDiagnostics{_isGrabbing.load(), _diagnosticDelivered.load(), value.failed};
}
