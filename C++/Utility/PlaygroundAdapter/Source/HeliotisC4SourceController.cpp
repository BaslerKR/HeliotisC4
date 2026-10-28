#include "HeliotisC4SourceController.h"
#include "engine/GraphicsFrame.h"

#include "Utility/PlaygroundAdapter/HeliotisGraphicsFrameStream.h"
#include "SessionFrame.h"

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
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
        _isGrabbing.store(active);
        emit diagnosticAcquisitionChanged(active, QStringLiteral("heliotis"));
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
}

// --- Device tools --------------------------------------------------------

/** One accepted tool request; delivery happens exactly once. */
struct HeliotisC4SourceController::ToolCall {
    IToolProvider::Completion completion;
    std::atomic<bool> delivered{false};

    void deliver(ApiResult<QJsonValue> result) noexcept
    {
        if (delivered.exchange(true)) return;
        try {
            completion(std::move(result));
        } catch (...) {
            qWarning().noquote() << "[Heliotis C4] Tool consumer callback threw.";
        }
    }
};

namespace {

using ParameterError = DeviceParameterError;
using ParameterKind = DeviceParameterKind;

constexpr const char* kParameterScope = "Device";

QJsonObject schema(QJsonObject properties, QJsonArray required)
{
    return {{"type", "object"}, {"properties", properties}, {"required", required},
            {"additionalProperties", false}};
}

QJsonObject stateSchema()
{
    const QJsonObject text{{"type", "string"}}, flag{{"type", "boolean"}};
    return schema({{"connected", flag}, {"live", flag}, {"connectionId", text}, {"targetId", text},
                   {"deliveredFrames", text}},
                  {"connected", "live", "connectionId", "targetId", "deliveredFrames"});
}

/** Encodes the exact discovery identity that `HeliotisC4Device::open()` re-verifies. */
QString targetId(const heliotis::DeviceDescriptor& device)
{
    const std::string key = device.interfaceName + '\0' + device.deviceName + '\0'
        + std::to_string(device.interfaceIndex) + '\0' + std::to_string(device.deviceIndex);
    return QString::fromLatin1(QByteArray(key.data(), qsizetype(key.size()))
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QJsonObject targets(const std::vector<heliotis::DeviceDescriptor>& devices)
{
    QJsonArray values;
    for (const auto& device : devices) {
        values.append(QJsonObject{{"targetId", targetId(device)},
            {"title", QString::fromStdString(device.deviceName)},
            {"deviceName", QString::fromStdString(device.deviceName)},
            {"interfaceName", QString::fromStdString(device.interfaceName)}});
    }
    return {{"targets", values}};
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

void HeliotisC4SourceController::setControlBackend(HeliotisC4ControlBackend* backend)
{
    Q_ASSERT(QThread::currentThread() == thread());
    _backend = backend;
    if (!backend && _toolCall) {
        const auto call = std::exchange(_toolCall, {});
        call->deliver(ApiResult<QJsonValue>::failure(ApiErrorCode::Unavailable,
            QStringLiteral("The Heliotis session closed before the operation completed.")));
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

QJsonObject HeliotisC4SourceController::controlState() const
{
    const QString connection = connectionId();
    const bool connected = _device && _device->isOpened() && !connection.isEmpty();
    const auto target = connected && _backend ? _backend->connectedTarget() : std::nullopt;
    return {{"connected", connected}, {"live", connected && _isGrabbing.load() && _isContinuous.load()},
            {"connectionId", connected ? connection : QString()},
            {"targetId", target ? targetId(*target) : QString()},
            {"deliveredFrames", QString::number(_deliveredFrames.load())}};
}

QList<ToolDescriptor> HeliotisC4SourceController::toolDescriptors() const
{
    const QJsonObject text{{"type", "string"}, {"minLength", 1}};
    const auto empty = schema({}, {});
    const auto connection = schema({{"connectionId", text}}, {"connectionId"});
    const auto entry = schema({{"targetId", text}, {"title", QJsonObject{{"type", "string"}}},
        {"deviceName", QJsonObject{{"type", "string"}}}, {"interfaceName", QJsonObject{{"type", "string"}}}},
        {"targetId", "title", "deviceName", "interfaceName"});
    const auto inventory = schema({{"targets", QJsonObject{{"type", "array"}, {"items", entry}}}}, {"targets"});
    QList<ToolDescriptor> result{
        {"device.targets", 1, "Device targets", "Copy the exact C4Utility interface/device discovery snapshot.",
            ToolEffect::Observe, true, {}, empty, inventory},
        {"device.connection", 1, "Device connection",
            "Observe the module-owned connection and delivered-frame count, not presentation completion.",
            ToolEffect::Observe, true, {}, empty, stateSchema()},
        {"device.discover", 1, "Discover devices", "Refresh C4Utility discovery while disconnected and idle.",
            ToolEffect::Device, true, {}, empty, inventory},
        {"device.connect", 1, "Connect device",
            "Open an exact discovered target; the device configuration is preserved and Init stays explicit.",
            ToolEffect::Device, true, {}, schema({{"targetId", text}}, {"targetId"}), stateSchema()},
        {"device.disconnect", 1, "Disconnect device", "Stop acquisition when needed and close the exact connection generation.",
            ToolEffect::Device, true, {}, connection, stateSchema()},
        {"device.single", 1, "Acquire one frame",
            "Arm Single acquisition and wait until it stops; query the viewer separately for presentation evidence.",
            ToolEffect::Pipeline, true, {}, connection, stateSchema()},
        {"device.live-start", 1, "Start Live", "Arm Live acquisition on the exact connection; completion is arm acknowledgement.",
            ToolEffect::Pipeline, true, {}, connection, stateSchema()},
        {"device.live-stop", 1, "Stop Live", "Stop acquisition on the exact connection and wait until it has stopped.",
            ToolEffect::Pipeline, true, {}, connection, stateSchema()}};
    if (!_backend) {
        for (auto& descriptor : result) {
            descriptor.available = false;
            descriptor.unavailableReason = QStringLiteral("Device control has no session.");
        }
    }
    return result;
}

ApiResult<void> HeliotisC4SourceController::requestTool(const QString& name, const int version,
                                                        QJsonObject arguments, IToolProvider::Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    using Result = ApiResult<QJsonValue>;
    if (!completion) return ApiResult<void>::failure(ApiErrorCode::InvalidArgument, "Completion is required.");
    bool known = false;
    for (const auto& descriptor : toolDescriptors()) known = known || descriptor.name == name;
    if (!known) return ApiResult<void>::failure(ApiErrorCode::NotFound, "Tool is not declared.");
    if (version != 1) return ApiResult<void>::failure(ApiErrorCode::Unsupported, "Unsupported tool version.");
    if (!_backend) return ApiResult<void>::failure(ApiErrorCode::Unavailable, "Device control has no session.");
    const bool observe = name == "device.targets" || name == "device.connection";
    const bool discover = name == "device.discover";
    const QString field = name == "device.connect" ? "targetId" : "connectionId";
    if (((observe || discover) && !arguments.isEmpty())
        || (!observe && !discover && (arguments.size() != 1 || !arguments.value(field).isString()
            || arguments.value(field).toString().isEmpty() || arguments.value(field).toString().contains(QChar::Null)))) {
        return ApiResult<void>::failure(ApiErrorCode::InvalidArgument, "Exact typed arguments are required.");
    }
    auto call = std::make_shared<ToolCall>();
    call->completion = std::move(completion);
    if (observe) {
        call->deliver(Result::success(name == "device.targets" ? targets(_backend->targets()) : controlState()));
        return ApiResult<void>::success();
    }
    if (externalBusy() || _backend->controlBusy()) {
        return ApiResult<void>::failure(ApiErrorCode::Busy, "Another Heliotis GUI or API operation is active.");
    }

    const QJsonObject state = controlState();
    const bool connected = state.value("connected").toBool();
    std::size_t targetIndex = 0;
    if (discover) {
        if (connected) return ApiResult<void>::failure(ApiErrorCode::Busy, "Disconnect before discovery.");
    } else if (name == "device.connect") {
        if (connected) return ApiResult<void>::failure(ApiErrorCode::Conflict, "Disconnect before connecting another device.");
        const auto devices = _backend->targets();
        bool found = false;
        for (std::size_t index = 0; index < devices.size() && !found; ++index) {
            if (targetId(devices[index]) == arguments.value(field).toString()) {
                targetIndex = index;
                found = true;
            }
        }
        if (!found) return ApiResult<void>::failure(ApiErrorCode::NotFound, "Exact discovered target was not found.");
    } else {
        if (!connected) return ApiResult<void>::failure(ApiErrorCode::Unavailable, "Device is disconnected.");
        if (state.value("connectionId").toString() != arguments.value(field).toString()) {
            return ApiResult<void>::failure(ApiErrorCode::Conflict, "Connection generation changed.");
        }
        if ((name == "device.single" || name == "device.live-start") && _isGrabbing.load()) {
            return ApiResult<void>::failure(ApiErrorCode::Busy, "Stop the active acquisition explicitly first.");
        }
        if (name == "device.live-stop" && !_isGrabbing.load()) {
            call->deliver(Result::success(state));
            return ApiResult<void>::success();
        }
    }

    _toolCall = call;
    emit controlChanged();
    const std::uint64_t framesBefore = _deliveredFrames.load();
    const QPointer<HeliotisC4SourceController> guard(this);
    auto done = [guard, call, name, framesBefore](const QString& error) {
        if (guard) guard->finishTool(call, name, error, framesBefore);
    };
    bool admitted = false;
    if (discover) admitted = _backend->discover(done);
    else if (name == "device.connect") admitted = _backend->connectTarget(targetIndex, done);
    else if (name == "device.disconnect") admitted = _backend->disconnect(done);
    else if (name == "device.single") admitted = _backend->arm(false, done);
    else if (name == "device.live-start") admitted = _backend->arm(true, done);
    else admitted = _backend->stop(done);
    if (!admitted) {
        if (_toolCall == call) _toolCall.reset();
        emit controlChanged();
        return ApiResult<void>::failure(ApiErrorCode::Busy, "The Heliotis session did not admit the operation.");
    }
    return ApiResult<void>::success();
}

void HeliotisC4SourceController::finishTool(const std::shared_ptr<ToolCall>& call, const QString& name,
                                            const QString& error, const std::uint64_t framesBefore)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (_toolCall == call) _toolCall.reset();
    ApiResult<QJsonValue> result = ApiResult<QJsonValue>::failure(ApiErrorCode::Internal, "Device operation did not complete.");
    if (!error.isEmpty()) {
        result = ApiResult<QJsonValue>::failure(ApiErrorCode::Unavailable, error);
    } else if (name == "device.discover") {
        result = ApiResult<QJsonValue>::success(targets(_backend ? _backend->targets() : std::vector<heliotis::DeviceDescriptor>{}));
    } else if (name == "device.single" && _deliveredFrames.load() == framesBefore) {
        result = ApiResult<QJsonValue>::failure(ApiErrorCode::Unavailable,
            "Single acquisition stopped without a converted frame.");
    } else {
        result = ApiResult<QJsonValue>::success(controlState());
    }
    emit controlChanged();
    call->deliver(std::move(result));
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
