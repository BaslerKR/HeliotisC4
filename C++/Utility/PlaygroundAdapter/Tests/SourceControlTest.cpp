#include "HeliotisC4SourceController.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fake {
bool opened = false;
bool acquiring = false;
heliotis::HeliotisC4::FeatureList features;
std::vector<std::pair<std::string, std::string>> writes;
std::vector<std::string> commands;
int triggers = 0;
heliotis::HeliotisC4Device::StatusCallback status;
} // namespace fake

// Replace only the SDK-owned device facade; exercise the real controller contracts.
namespace heliotis {
HeliotisC4Device::HeliotisC4Device(HeliotisC4System* system) : _system(system) {}
HeliotisC4Device::~HeliotisC4Device() = default;
bool HeliotisC4Device::isOpened() const { return fake::opened; }
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
bool HeliotisC4Device::startAcquisition(AcquisitionMode, FrameCallback, std::string*) { return true; }
void HeliotisC4Device::requestStopAcquisition() noexcept {}
void HeliotisC4Device::stopAcquisition() {}
bool HeliotisC4Device::isSoftwareTriggerAvailable() const { return true; }
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

    bool store(const char* name, Completion completion)
    {
        last = name;
        pending = std::move(completion);
        return admit;
    }

    bool busy = false;
    bool admit = true;
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
        auto* tools = qobject_cast<IToolProvider*>(&controller);
        auto* parameters = qobject_cast<IDeviceParameterControl*>(&controller);
        require(tools && parameters, "The controller must declare both host capability interfaces.");

        const auto unbound = controller.toolDescriptors();
        require(unbound.size() == 8 && !unbound.front().available,
                "Tools without a session must be declared unavailable.");
        FakeBackend backend;
        backend.devices = {{0, 0, "H8 Interface", "H8-0001"}, {0, 1, "H8 Interface", "H8-0002"}};
        controller.setControlBackend(&backend);
        require(controller.toolDescriptors().front().available, "A bound session must enable the tools.");

        std::optional<ApiResult<QJsonValue>> toolResult;
        const auto tool = [&](const QString& name, QJsonObject arguments = {}) {
            toolResult.reset();
            return tools->requestTool(name, 1, std::move(arguments),
                                      [&toolResult](ApiResult<QJsonValue> result) { toolResult = std::move(result); });
        };

        // Discovery snapshot and exact target admission.
        require(tool("device.targets").succeeded() && toolResult && toolResult->succeeded()
                    && toolResult->value().toObject().value("targets").toArray().size() == 2,
                "device.targets must copy the discovery snapshot.");
        const QString secondTarget = toolResult->value().toObject().value("targets").toArray()
            .at(1).toObject().value("targetId").toString();
        require(tool("device.connect", {{"targetId", "missing"}}).error()->code == ApiErrorCode::NotFound,
                "An unknown target must be refused before any session work.");
        require(tool("device.connect", {{"targetId", 1}}).error()->code == ApiErrorCode::InvalidArgument,
                "Tool arguments must be exactly typed.");
        require(tool("device.single", {{"connectionId", "x"}}).error()->code == ApiErrorCode::Unavailable,
                "Acquisition tools require a connection.");

        require(tool("device.connect", {{"targetId", secondTarget}}).succeeded() && backend.last == "connect"
                    && backend.connectIndex == 1 && !toolResult,
                "device.connect must route the exact target through the session.");
        require(controller.externalBusy(), "A pending tool call must hold external admission.");
        require(tool("device.discover").error()->code == ApiErrorCode::Busy, "Tool calls are serialized.");
        fake::opened = true;
        fake::status(heliotis::HeliotisC4Device::Status::Connection, true);
        backend.connected = backend.devices[1];
        backend.pending({});
        require(toolResult && toolResult->succeeded()
                    && toolResult->value().toObject().value("connected").toBool()
                    && toolResult->value().toObject().value("targetId").toString() == secondTarget,
                "device.connect must complete with the new connection state.");
        const QString connection = controller.connectionId();
        require(!connection.isEmpty() && !controller.externalBusy(), "A connection must mint a generation.");
        require(tool("device.single", {{"connectionId", "stale"}}).error()->code == ApiErrorCode::Conflict,
                "A stale connection generation must conflict.");

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

        require(tool("device.live-start", {{"connectionId", connection}}).error()->code == ApiErrorCode::Busy,
                "Live cannot start while another acquisition is armed.");
        require(tool("device.live-stop", {{"connectionId", connection}}).succeeded() && backend.last == "stop",
                "device.live-stop must use the session stop path.");
        fake::acquiring = false;
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);
        backend.pending({});
        require(toolResult && toolResult->succeeded() && !toolResult->value().toObject().value("live").toBool(),
                "device.live-stop must complete after acquisition stops.");

        require(tool("device.single", {{"connectionId", connection}}).succeeded() && backend.last == "single",
                "device.single must use the session Single path.");
        backend.pending({});
        require(toolResult && !toolResult->succeeded(),
                "device.single must fail when no frame was converted.");

        backend.admit = false;
        require(tool("device.disconnect", {{"connectionId", connection}}).error()->code == ApiErrorCode::Busy
                    && !controller.externalBusy(),
                "A session refusal must release tool admission.");
        backend.admit = true;
        require(tool("device.disconnect", {{"connectionId", connection}}).succeeded(), "Disconnect is admitted.");
        controller.setControlBackend(nullptr);
        require(toolResult && toolResult->error() && toolResult->error()->code == ApiErrorCode::Unavailable,
                "Unbinding the session must complete a pending tool call.");
    }
    if (failures == 0) std::cout << "Heliotis device tools and parameter contracts passed.\n";
    return failures == 0 ? 0 : 1;
}
