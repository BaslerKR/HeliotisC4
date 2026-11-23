#include "HeliotisC4SourceController.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include "Tools/ToolProvider.h"

#include <atomic>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fake {
bool opened = false;
bool acquiring = false;
heliotis::HeliotisC4::FeatureList features;
std::vector<std::pair<std::string, std::string>> writes;
std::vector<std::string> commands;
std::atomic<int> triggers{0};
std::atomic<int> initializations{0};
std::atomic<bool> initSucceeds{true};
std::atomic<bool> initCancelled{false};
heliotis::HeliotisC4Device::StatusCallback status;
heliotis::HeliotisC4Device::FrameCallback frame;
} // namespace fake

// Replace only the SDK-owned device facade; exercise the real controller contracts.
namespace heliotis {
HeliotisC4Device::HeliotisC4Device(HeliotisC4System* system) : _system(system) {}
HeliotisC4Device::~HeliotisC4Device() = default;
bool HeliotisC4Device::isOpened() const { return fake::opened; }
bool HeliotisC4Device::isAcquiring() const { return fake::acquiring; }
HeliotisC4::FeatureList HeliotisC4Device::readFeatures(std::string* errorMessage) const
{
    if (fake::acquiring) {
        if (errorMessage) *errorMessage = "armed";
        return {};
    }
    return fake::features;
}
bool HeliotisC4Device::writeFeature(const std::string& name, const std::string& value, std::string*)
{
    fake::writes.emplace_back(name, value);
    for (auto& feature : fake::features) {
        if (feature.displayName == name) feature.valueText = value;
    }
    return true;
}
bool HeliotisC4Device::executeCommand(const std::string& name, std::string*)
{
    fake::commands.push_back(name);
    return true;
}
bool HeliotisC4Device::triggerSoftware(std::string*)
{
    ++fake::triggers;
    return true;
}
bool HeliotisC4Device::configureH8SurfaceExample(std::string* errorMessage,
    const std::function<bool()>& externallyCancelled)
{
    if (externallyCancelled && externallyCancelled()) {
        if (errorMessage) *errorMessage = "H8 reference-profile initialization was cancelled before it started.";
        return false;
    }
    ++fake::initializations;
    fake::initCancelled.store(false);
    for (int step = 0; step < 500 && !fake::initCancelled.load()
        && !(externallyCancelled && externallyCancelled()); ++step) QThread::msleep(1);
    if (fake::initCancelled.load() || (externallyCancelled && externallyCancelled())) {
        if (errorMessage) *errorMessage = "H8 reference-profile initialization was cancelled during the fixture run.";
        return false;
    }
    if (!fake::initSucceeds.load()) {
        if (errorMessage) *errorMessage = "The fixture H8 reference profile failed.";
        return false;
    }
    return true;
}
void HeliotisC4Device::requestCancelInitialization() noexcept { fake::initCancelled.store(true); }
bool HeliotisC4Device::isSoftwareTriggeredAcquisition() const { return fake::acquiring; }
bool HeliotisC4Device::isSoftwareTriggerAvailable() const { return true; }
bool HeliotisC4Device::startAcquisition(AcquisitionMode, FrameCallback callback, std::string*) {
    fake::frame = std::move(callback);
    fake::acquiring = true;
    if (fake::status) fake::status(Status::Acquisition, true);
    return true;
}
void HeliotisC4Device::requestStopAcquisition() noexcept {}
void HeliotisC4Device::stopAcquisition() {}
bool HeliotisC4Device::requiresReconnect() const { return false; }
std::string HeliotisC4Device::lastAcquisitionError() const { return {}; }
HeliotisC4Device::CallbackId HeliotisC4Device::registerStatusCallback(StatusCallback callback)
{
    fake::status = std::move(callback);
    return 1;
}
bool HeliotisC4Device::deregisterStatusCallback(CallbackId)
{
    fake::status = {};
    return true;
}
} // namespace heliotis

namespace {

std::vector<DeviceRangeRejectionObservation> observedRanges;

void rememberRange(const DeviceRangeRejectionObservation& observation) noexcept
{
    observedRanges.push_back(observation);
}

int failures = 0;

void require(const bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

/** Spins the controller thread until an asynchronous completion arrives. */
bool waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    QCoreApplication::processEvents();
    return condition();
}

class FakeBackend final : public HeliotisC4ControlBackend {
public:
    bool controlBusy() const override { return busy; }
    std::vector<heliotis::DeviceDescriptor> targets() const override { return devices; }
    std::optional<heliotis::DeviceDescriptor> connectedTarget() const override { return connected; }
    bool discover(Completion completion) override { return store("discover", std::move(completion)); }
    bool connectTarget(const std::size_t index, Completion completion) override
    {
        connectIndex = index;
        return store("connect", std::move(completion));
    }
    bool disconnect(Completion completion) override { return store("disconnect", std::move(completion)); }
    bool arm(const bool continuous, Completion completion) override
    {
        return store(continuous ? "live" : "single", std::move(completion));
    }
    bool stop(Completion completion) override { return store("stop", std::move(completion)); }
    bool initialize(Completion completion, std::function<bool()> cancelled) override
    {
        ++initializations;
        cancelledPredicate = std::move(cancelled);
        return store("initialize", std::move(completion));
    }

    bool store(const char* name, Completion completion)
    {
        last = name;
        pending = std::move(completion);
        return admit;
    }

    bool busy = false;
    bool admit = true;
    int initializations = 0;
    std::function<bool()> cancelledPredicate;
    std::vector<heliotis::DeviceDescriptor> devices;
    std::optional<heliotis::DeviceDescriptor> connected;
    std::size_t connectIndex = 0;
    std::string last;
    Completion pending;
};

heliotis::FeatureDescriptor feature(const char* name, const heliotis::FeatureType type, const char* value,
                                    const heliotis::FeatureAccess access = heliotis::FeatureAccess::ReadWrite)
{
    heliotis::FeatureDescriptor result;
    result.categoryPath = "Category";
    result.displayName = name;
    result.valueText = value;
    result.type = type;
    result.access = access;
    return result;
}

std::optional<DeviceParameterDescriptor> findParameter(const std::vector<DeviceParameterDescriptor>& parameters,
                                                       const std::string& name)
{
    for (const auto& parameter : parameters) {
        if (parameter.target.name == name) return parameter;
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    using Error = DeviceParameterError;

    auto exposure = feature("ExposureTime", heliotis::FeatureType::Integer, "4");
    exposure.integerMinimum = 1;
    exposure.integerMaximum = 10;
    exposure.integerIncrement = 1;
    auto gain = feature("Gain", heliotis::FeatureType::Float, "1.5");
    gain.floatMinimum = 0.0;
    gain.floatMaximum = 4.0;
    auto mode = feature("TriggerMode", heliotis::FeatureType::Enumeration, "Off");
    mode.enumEntries = {"Off", "On"};
    fake::features = {exposure, gain, mode,
        feature("ReverseX", heliotis::FeatureType::Boolean, "0"),
        feature("DeviceUserID", heliotis::FeatureType::String, "h8"),
        feature("StageInit", heliotis::FeatureType::Command, "Execute"),
        feature("TriggerSoftware", heliotis::FeatureType::Command, "Execute"),
        feature("Opaque", heliotis::FeatureType::Unknown, "")};

    heliotis::HeliotisC4Device device(nullptr);
    {
        HeliotisC4SourceController controller(&device);
        auto* lifecycle = qobject_cast<IDeviceConnectionControl*>(&controller);
        auto* parameters = qobject_cast<IDeviceParameterControl*>(&controller);
        auto* moduleTools = qobject_cast<IToolProvider*>(&controller);
        auto* execution = qobject_cast<IToolExecution*>(&controller);
        require(lifecycle && parameters, "The controller must declare both host capability interfaces.");
        require(moduleTools && execution, "The controller must declare its module tools and execution contract.");
        const auto tools = moduleTools->toolDescriptors();
        require(tools.size() == 1 && tools.front().name == "heliotis-c4.init"
                    && tools.front().effect == ToolEffect::Device
                    && tools.front().egress == ToolEgress::Metadata
                    && tools.front().presentation.actionId == "heliotis-c4.init"
                    && tools.front().presentation.iconId == "setting"
                    && tools.front().presentation.group == "device"
                    && tools.front().cancellable
                    && tools.front().inputSchema.value("properties").toObject().contains("confirmed"),
                "The module must declare heliotis-c4.init as a cancellable device action with a confirm prompt.");

        const auto unbound = controller.connectionCapabilities();
        require(!unbound.connect.available && !unbound.connect.reason.empty(),
                "Lifecycle operations without a session must be declared unavailable.");
        FakeBackend backend;
        backend.devices = {{0, 0, "H8 Interface", "H8-0001"}, {0, 1, "H8 Interface", "H8-0002"}};
        controller.setControlBackend(&backend);
        const auto bound = controller.connectionCapabilities();
        require(bound.targets.available && bound.connect.available && bound.single.available && bound.live.available,
                "A bound session must enable every lifecycle operation.");

        using Lifecycle = DeviceConnectionError;
        std::optional<DeviceConnectionResult> toolResult;
        std::optional<DeviceConnectionTargets> targetsResult;
        const auto onState = [&toolResult](DeviceConnectionResult result) { toolResult = std::move(result); };
        const auto onTargets = [&targetsResult](DeviceConnectionTargets result) { targetsResult = std::move(result); };
        const auto tool = [&](const std::string& name, const std::string& argument = {}) {
            toolResult.reset();
            targetsResult.reset();
            if (name == "targets") return lifecycle->requestConnectionTargets(onTargets);
            if (name == "discover") return lifecycle->requestDiscover(onTargets);
            if (name == "connect") return lifecycle->requestConnect(argument, onState);
            if (name == "disconnect") return lifecycle->requestDisconnect(argument, onState);
            if (name == "single") return lifecycle->requestSingle(argument, onState);
            if (name == "live-start") return lifecycle->requestLiveStart(argument, onState);
            if (name == "live-stop") return lifecycle->requestLiveStop(argument, onState);
            return lifecycle->requestConnectionState(onState);
        };

        // Discovery snapshot and exact target admission.
        require(tool("targets").error == Lifecycle::None && targetsResult && targetsResult->result.error == Lifecycle::None
                    && targetsResult->targets.size() == 2 && targetsResult->targets[1].displayName == "H8-0002",
                "Targets must copy the discovery snapshot.");
        const std::string secondTarget = targetsResult->targets[1].targetId;
        require(tool("connect", "missing").error == Lifecycle::NotFound,
                "An unknown target must be refused before any session work.");
        require(tool("single", "x").error == Lifecycle::Disconnected,
                "Acquisition requires a connection.");

        require(tool("connect", secondTarget).error == Lifecycle::None && backend.last == "connect"
                    && backend.connectIndex == 1 && !toolResult,
                "Connect must route the exact target through the session.");
        require(controller.externalBusy(), "A pending lifecycle call must hold external admission.");
        require(tool("discover").error == Lifecycle::Busy, "Lifecycle calls are serialized.");
        fake::opened = true;
        fake::status(heliotis::HeliotisC4Device::Status::Connection, true);
        backend.connected = backend.devices[1];
        backend.pending({});
        require(toolResult && toolResult->error == Lifecycle::None && toolResult->state
                    && toolResult->state->connected && toolResult->state->targetId == secondTarget,
                "Connect must complete with the new connection state.");
        const QString connection = controller.connectionId();
        require(!connection.isEmpty() && !controller.externalBusy(), "A connection must mint a generation.");
        require(toolResult->state->connectionId == connection.toStdString(), "The state must carry the connection id.");
        require(tool("single", "stale").error == Lifecycle::Conflict,
                "A stale connection id must conflict.");

        // Parameter inventory, typed values, bounds and stable revisions.
        std::optional<DeviceParameterInventory> inventory;
        require(parameters->requestParameters([&](DeviceParameterInventory value) { inventory = std::move(value); })
                    .error == Error::None, "Connected inventory must be admitted.");
        require(waitFor([&] { return inventory.has_value(); }) && inventory->result.error == Error::None
                    && inventory->parameters.size() == 7, "Inventory must contain every typed feature.");
        const auto exposureParameter = findParameter(inventory->parameters, "ExposureTime");
        require(exposureParameter && exposureParameter->kind == DeviceParameterKind::Integer
                    && exposureParameter->target.scope == "Device"
                    && exposureParameter->target.connectionId == connection.toStdString()
                    && exposureParameter->value == DeviceParameterValue{std::int64_t{4}}
                    && exposureParameter->integerMaximum == std::optional<std::int64_t>{10}
                    && exposureParameter->revision != 0,
                "Integer parameters must carry typed values and SDK bounds.");
        const auto reverse = findParameter(inventory->parameters, "ReverseX");
        const auto stage = findParameter(inventory->parameters, "StageInit");
        require(reverse && reverse->value == DeviceParameterValue{false}, "Booleans must be typed.");
        require(stage && stage->kind == DeviceParameterKind::Command && !stage->readable && stage->writable,
                "Commands are writable and not readable.");
        require(exposureParameter->categoryPath == std::vector<std::string>{"Category"}
                    && exposureParameter->visibility == DeviceParameterVisibility::Primary
                    && exposureParameter->representation == DeviceParameterRepresentation::Number
                    && reverse->representation == DeviceParameterRepresentation::Toggle
                    && findParameter(inventory->parameters, "TriggerMode")->representation
                        == DeviceParameterRepresentation::Choice,
                "Descriptors must carry the tree path, visibility and preferred editor.");
        std::optional<DeviceParameterInventory> repeated;
        require(parameters->requestParameters([&](DeviceParameterInventory value) { repeated = std::move(value); })
                    .error == Error::None && waitFor([&] { return repeated.has_value(); })
                    && findParameter(repeated->parameters, "ExposureTime")->revision == exposureParameter->revision,
                "An unchanged observation must keep its revision.");

        std::optional<DeviceParameterResult> result;
        const auto completion = [&](DeviceParameterResult value) { result = std::move(value); };
        auto target = exposureParameter->target;
        require(parameters->requestWriteParameter(target, std::int64_t{11}, std::nullopt, completion).error == Error::None
                    && waitFor([&] { return result.has_value(); }) && result->error != Error::None
                    && result->effect == DeviceParameterEffect::None && fake::writes.empty(),
                "An out-of-range write must be refused before the SDK write.");

        int applied = 0;
        QObject::connect(&controller, &HeliotisC4SourceController::parametersApplied, [&applied] { ++applied; });
        result.reset();
        require(parameters->requestWriteParameter(target, std::int64_t{8}, exposureParameter->revision, completion)
                    .error == Error::None && waitFor([&] { return result.has_value(); }),
                "A valid write must complete.");
        require(result->error == Error::None && result->effect == DeviceParameterEffect::Applied
                    && fake::writes.size() == 1 && fake::writes.back().second == "8"
                    && result->descriptor && result->descriptor->value == DeviceParameterValue{std::int64_t{8}}
                    && result->descriptor->revision != exposureParameter->revision,
                "A write must report its readback with a new revision.");
        require(waitFor([&] { return applied == 1; }), "An applied write must mark the feature view stale.");

        result.reset();
        require(parameters->requestWriteParameter(target, std::int64_t{5}, exposureParameter->revision, completion)
                    .error == Error::None && waitFor([&] { return result.has_value(); })
                    && result->error == Error::Conflict && fake::writes.size() == 1,
                "A stale expected revision must conflict without writing.");

        auto stale = target;
        stale.connectionId = "previous";
        require(parameters->requestReadParameter(stale, completion).error == Error::Conflict,
                "A previous connection generation must conflict at admission.");
        auto user = findParameter(inventory->parameters, "DeviceUserID")->target;
        require(parameters->requestWriteParameter(user, std::string(" padded"), std::nullopt, completion).error
                    == Error::InvalidArgument, "Padded strings cannot round-trip through C4Utility.");

        result.reset();
        require(parameters->requestExecuteParameter(stage->target, std::nullopt, completion).error == Error::None
                    && waitFor([&] { return result.has_value(); })
                    && result->effect == DeviceParameterEffect::Applied
                    && fake::commands == std::vector<std::string>{"StageInit"},
                "Commands must execute through the feature channel.");

        // Bounded change notifications: initial snapshot, write delta, overflow resync.
        std::mutex changeMutex;
        std::vector<DeviceParameterChange> changes;
        const auto collect = [&](DeviceParameterChange change) {
            std::lock_guard<std::mutex> lock(changeMutex);
            changes.push_back(std::move(change));
        };
        require(parameters->requestParameterChanges(connection.toStdString(), "watch-1", collect)
                    .error == Error::None && controller.parameterWatchesActive(),
                "A connected controller must admit a bounded change subscription.");
        require(waitFor([&] {
                    std::lock_guard<std::mutex> lock(changeMutex);
                    return !changes.empty();
                }),
                "A subscription must deliver its initial snapshot.");
        {
            std::lock_guard<std::mutex> lock(changeMutex);
            require(changes.front().kind == DeviceParameterChangeKind::Snapshot
                        && changes.front().sequence == 1
                        && !changes.front().descriptors.empty()
                        && changes.front().descriptors.front().categoryPath
                            == std::vector<std::string>{"Category"},
                    "The initial snapshot must carry the complete tree descriptors.");
            changes.clear();
        }
        result.reset();
        require(parameters->requestWriteParameter(target, std::int64_t{9}, std::nullopt, completion)
                    .error == Error::None && waitFor([&] { return result.has_value(); }),
                "A subscribed parameter write must complete.");
        require(waitFor([&] {
                    std::lock_guard<std::mutex> lock(changeMutex);
                    return !changes.empty();
                }),
                "An applied write must publish one delta.");
        {
            std::lock_guard<std::mutex> lock(changeMutex);
            require(changes.front().kind == DeviceParameterChangeKind::Delta
                        && changes.front().sequence == 2
                        && !changes.front().descriptors.empty(),
                    "A write delta must be revision-stamped and sequenced.");
            changes.clear();
        }
        // A bounded queue never loses changes silently: twenty observations
        // without an event loop turn must produce one explicit resync snapshot.
        for (int step = 0; step < 20; ++step) {
            for (auto& feature : fake::features) {
                if (feature.displayName == "ExposureTime")
                    feature.valueText = std::to_string(40 + step);
            }
            controller.publishParameterSnapshot(fake::features);
        }
        QCoreApplication::processEvents();
        require(waitFor([&] {
                    std::lock_guard<std::mutex> lock(changeMutex);
                    return !changes.empty();
                }),
                "The bounded queue must drain after overflow.");
        {
            std::lock_guard<std::mutex> lock(changeMutex);
            bool sawResync = false;
            std::uint64_t previousSequence = 2;
            for (const auto& change : changes) {
                require(change.sequence > previousSequence,
                    "Delivered change sequences must keep increasing.");
                previousSequence = change.sequence;
                if (!change.resync) continue;
                sawResync = true;
                require(change.kind == DeviceParameterChangeKind::Snapshot
                            && !change.overflowReason.empty(),
                    "An overflow must deliver one explicit resync snapshot.");
            }
            require(sawResync, "The bounded change queue must report its overflow as a resync.");
            changes.clear();
        }
        bool released = false;
        require(parameters->releaseParameterChanges(connection.toStdString(), "watch-1",
                    [&](DeviceParameterResult) { released = true; })
                    .error == Error::None && released && !controller.parameterWatchesActive(),
                "Release must stop the subscription and complete.");

        // While armed only the FrameStart command is admitted, without feature reads.
        fake::acquiring = true;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, true);
        require(parameters->requestReadParameter(target, completion).error == Error::Busy,
                "Feature reads are refused while acquisition is armed.");
        const auto trigger = findParameter(inventory->parameters, "TriggerSoftware")->target;
        result.reset();
        require(parameters->requestExecuteParameter(trigger, std::nullopt, completion).error == Error::None
                    && waitFor([&] { return result.has_value(); })
                    && result->effect == DeviceParameterEffect::Applied && fake::triggers == 1,
                "TriggerSoftware must reach the armed acquisition.");

        require(tool("live-start", connection.toStdString()).error == Lifecycle::Busy,
                "Live cannot start while another acquisition is armed.");
        require(tool("live-stop", connection.toStdString()).error == Lifecycle::None && backend.last == "stop",
                "Live stop must use the session stop path.");
        fake::acquiring = false;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);
        backend.pending({});
        require(toolResult && toolResult->error == Lifecycle::None && !toolResult->state->live,
                "Live stop must complete after acquisition stops.");

        require(tool("single", connection.toStdString()).error == Lifecycle::None && backend.last == "single",
                "Single must use the session Single path.");
        backend.pending({});
        require(toolResult && toolResult->error != Lifecycle::None,
                "Single must fail when no frame was converted.");

        // Declared Init: typed confirm, shared session path, completion and error.
        std::optional<ApiResult<QJsonValue>> initResult;
        int initProgress = 0;
        const auto finishInit = [&initResult](ApiResult<QJsonValue> result) {
            initResult = std::move(result);
        };
        const auto progress = [&initProgress](const ApiProgress& report) {
            ++initProgress;
            require(report.cancellable, "Init progress must report that cancellation is available.");
        };
        const auto invokeInit = [&](const std::string& callId, const bool confirmed) {
            QJsonObject arguments;
            if (confirmed) arguments.insert(QStringLiteral("confirmed"), true);
            return execution->requestToolWithProgress(QString::fromStdString(callId),
                QStringLiteral("heliotis-c4.init"), 1, arguments, progress, finishInit);
        };

        initResult.reset();
        require(invokeInit("init-confirm", false).succeeded() && initResult
                    && !initResult->succeeded()
                    && initResult->error()->code == ApiErrorCode::ArgumentRequired
                    && backend.initializations == 0,
                "Init without confirmation must complete with a typed confirm prompt and no effect.");
        initResult.reset();
        initProgress = 0;
        require(invokeInit("init-error", true).succeeded() && backend.initializations == 1
                    && initProgress == 1 && !initResult,
                "A confirmed Init must be admitted through the session path with one progress report.");
        const auto duplicate = invokeInit("init-error", true);
        require(!duplicate.succeeded() && duplicate.error()
                    && duplicate.error()->code == ApiErrorCode::Conflict,
                "A retry with the accepted call identity must conflict.");
        backend.pending(QStringLiteral("The fixture H8 profile failed."));
        require(initResult && !initResult->succeeded()
                    && initResult->error()->details.value("effect") == QLatin1String("unknown"),
                "An Init failure must complete with an explicit unknown effect.");
        initResult.reset();
        initProgress = 0;
        require(invokeInit("init-ok", true).succeeded() && backend.initializations == 2,
                "A repeated Init after failure must be admitted.");
        backend.pending({});
        require(initResult && initResult->succeeded()
                    && initResult->value().toObject().value("initialized").toBool(),
                "A completed Init must report its terminal result.");
        initResult.reset();
        fake::initCancelled.store(false);
        require(invokeInit("init-cancel", true).succeeded() && backend.initializations == 3,
                "A cancellable Init must be admitted with its call identity.");
        require(execution->cancelTool(QStringLiteral("init-cancel")).succeeded()
                    && fake::initCancelled.load()
                    && backend.cancelledPredicate && backend.cancelledPredicate(),
                "Cancellation must reach the device and the pre-start predicate.");
        backend.pending(QStringLiteral(
            "H8 reference-profile initialization was cancelled during the fixture run."));
        require(initResult && !initResult->succeeded()
                    && initResult->error()->code == ApiErrorCode::Cancelled,
                "A cancelled Init must complete as cancelled.");

        fake::acquiring = true;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, true);
        initResult.reset();
        require(invokeInit("init-armed", true).succeeded() == false && !initResult,
                "Init must be refused while acquisition is armed.");
        require(invokeInit("init-armed-no-confirm", false).succeeded() == false && !initResult,
                "An armed device must be refused before any confirmation prompt.");
        fake::acquiring = false;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);
        QCoreApplication::processEvents();

        require(parameters->requestParameterChanges(connection.toStdString(), "watch-terminal", collect)
                    .error == Error::None && waitFor([&] {
                        std::lock_guard<std::mutex> lock(changeMutex);
                        return !changes.empty();
                    }),
                "A terminal-change subscription must snapshot before disconnect.");
        {
            std::lock_guard<std::mutex> lock(changeMutex);
            changes.clear();
        }

        backend.admit = false;
        require(tool("disconnect", connection.toStdString()).error == Lifecycle::Busy
                    && !controller.externalBusy(),
                "A session refusal must release lifecycle admission.");
        backend.admit = true;
        require(tool("disconnect", connection.toStdString()).error == Lifecycle::None, "Disconnect is admitted.");
        controller.setControlBackend(nullptr);
        require(toolResult && toolResult->error == Lifecycle::Unavailable
                    && toolResult->effect == DeviceConnectionEffect::Unknown,
                "Unbinding the session must complete a pending lifecycle call.");
        fake::opened = false;
        fake::status(heliotis::HeliotisC4Device::Status::Connection, false);
        require(waitFor([&] {
                    std::lock_guard<std::mutex> lock(changeMutex);
                    for (const auto& change : changes)
                        if (change.error == DeviceParameterError::Disconnected) return true;
                    return false;
                }),
                "A disconnect must end every parameter subscription explicitly.");
        require(!controller.parameterWatchesActive(),
                "A terminal change must remove the parameter subscription.");
    }
    {
        heliotis::HeliotisC4Device device(nullptr);
        HeliotisC4SourceController controller(&device);
        AbstractSourceController& callable = controller;
        controller.start();
        require(callable.sourceRuntimeDiagnostics(0)->grabbing, "An armed source reports grabbing without a widget.");
        require(callable.sourceRuntimeDiagnostics(0)->failed == 0, "Trigger waits do not add failed frames.");
        fake::frame(heliotis::Frame{});
        require(callable.sourceRuntimeDiagnostics(0)->failed == 1, "A conversion failure reaches the source query.");
        heliotis::Frame frame;
        frame.parts = {{heliotis::FramePartKind::Range, "Range", "Mono16", 1, 1, 16,
                        std::vector<std::uint16_t>{42U}}};
        fake::frame(heliotis::Frame(frame));
        require(callable.sourceRuntimeDiagnostics(0)->delivered == 0, "No consumer means no source delivery.");
        controller.setFrameConsumer([](SessionFrame&&, unsigned) {});
        fake::frame(heliotis::Frame(frame));
        require(callable.sourceRuntimeDiagnostics(0)->delivered == 1, "A successful consumer reaches the source query.");
        controller.setFrameConsumer([](SessionFrame&&, unsigned) { throw std::runtime_error("consumer failure"); });
        fake::frame(heliotis::Frame(frame));
        const auto failed = callable.sourceRuntimeDiagnostics(0);
        require(failed->delivered == 1 && failed->failed == 2, "A failed consumer increments only failures.");
        fake::acquiring = false;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);
        const auto stopped = callable.sourceRuntimeDiagnostics(0);
        require(!stopped->grabbing && stopped->delivered == 1 && stopped->failed == 2,
                "Stopped counters remain observable.");
        controller.start();
        require(callable.sourceRuntimeDiagnostics(0)->delivered == 0 && callable.sourceRuntimeDiagnostics(0)->failed == 0,
                "A new run resets diagnostics.");
        require(!callable.sourceRuntimeDiagnostics(9), "Unknown source indices are unsupported.");
        fake::acquiring = false;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);
    }
    {
        // The owner sink is detail-independent: Single reports every observed
        // failure, while Live keeps the first and every 100th per acquisition.
        heliotis::HeliotisC4Device device(nullptr);
        observedRanges.clear();
        HeliotisC4SourceController controller(&device, nullptr, rememberRange, nullptr);
        controller.start();
        heliotis::Frame rejected;
        rejected.sequence = 1;
        rejected.parts = {{heliotis::FramePartKind::Range, "Range", "Mono16", 2, 2, 16,
                           std::vector<std::uint16_t>{}}};
        fake::frame(heliotis::Frame(rejected));
        require(observedRanges.size() == 1U
                    && observedRanges.front().reason == DeviceRangeRejection::MissingRange
                    && observedRanges.front().frameSequence == 1U
                    && !observedRanges.front().rangeRetained
                    && !observedRanges.front().imageRetained,
                "A Live run must report the first range rejection with fixed evidence.");
        for (unsigned frame = 2; frame < 100; ++frame) {
            rejected.sequence = frame;
            fake::frame(heliotis::Frame(rejected));
        }
        require(observedRanges.size() == 1U,
                "A Live run must coalesce intermediate failures of the same reason.");
        rejected.sequence = 100;
        fake::frame(heliotis::Frame(rejected));
        require(observedRanges.size() == 2U && observedRanges.back().frameSequence == 100U,
                "A Live run must report every 100th failure of a reason.");
        fake::acquiring = false;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);

        // A new acquisition resets the per-reason counts.
        controller.start();
        rejected.sequence = 101;
        fake::frame(heliotis::Frame(rejected));
        require(observedRanges.size() == 3U && observedRanges.back().frameSequence == 101U,
                "A new Live acquisition must report its first failure again.");
        fake::acquiring = false;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);

        // Single reports every failure without coalescing.
        require(controller.grabOne(), "A Single run must be admitted.");
        for (unsigned frame = 102; frame < 105; ++frame) {
            rejected.sequence = frame;
            fake::frame(heliotis::Frame(rejected));
        }
        require(observedRanges.size() == 6U && observedRanges.back().frameSequence == 104U,
                "A Single run must report every observed failure.");
        fake::acquiring = false;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);
    }
    if (failures == 0) std::cout << "Heliotis device lifecycle and parameter contracts passed.\n";
    return failures == 0 ? 0 : 1;
}
