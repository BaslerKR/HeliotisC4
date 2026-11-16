#include "HeliotisC4PluginSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fake {
bool opened = false;
bool acquiring = false;
std::string deviceName = "H8-0001";
heliotis::HeliotisC4::FeatureList features;
heliotis::HeliotisC4Device::StatusCallback status;
heliotis::HeliotisC4Device::FrameCallback frame;
} // namespace fake

// Replace only the SDK-owned device facade; exercise the real session flows headlessly.
namespace heliotis {
HeliotisC4System::HeliotisC4System(const std::string&) {}
HeliotisC4System::~HeliotisC4System() = default;
std::vector<DeviceDescriptor> HeliotisC4System::discoverDevices(std::string* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    return {};
}
std::unique_ptr<HeliotisC4Device> HeliotisC4System::createDevice()
{
    return std::make_unique<HeliotisC4Device>(this);
}
HeliotisC4Device::HeliotisC4Device(HeliotisC4System* system) : _system(system) {}
HeliotisC4Device::~HeliotisC4Device() = default;
bool HeliotisC4Device::open(const DeviceDescriptor& descriptor, std::string* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    fake::opened = true;
    fake::deviceName = descriptor.deviceName;
    if (fake::status) fake::status(Status::Connection, true);
    return true;
}
void HeliotisC4Device::close()
{
    fake::acquiring = false;
    fake::opened = false;
    if (fake::status) fake::status(Status::Connection, false);
}
bool HeliotisC4Device::isOpened() const { return fake::opened; }
std::string HeliotisC4Device::connectedDeviceName() const { return fake::deviceName; }
bool HeliotisC4Device::configureH8SurfaceExample(std::string*) { return fake::opened; }
void HeliotisC4Device::requestCancelInitialization() noexcept {}
bool HeliotisC4Device::initializeMotion(std::string*) { return fake::opened; }
HeliotisC4::FeatureList HeliotisC4Device::readFeatures(std::string* errorMessage) const
{
    if (errorMessage) errorMessage->clear();
    return fake::features;
}
bool HeliotisC4Device::writeFeature(const std::string&, const std::string&, std::string*) { return false; }
bool HeliotisC4Device::executeCommand(const std::string&, std::string*) { return false; }
bool HeliotisC4Device::triggerSoftware(std::string*) { return fake::acquiring; }
bool HeliotisC4Device::startAcquisition(AcquisitionMode, FrameCallback callback, std::string* errorMessage)
{
    if (!fake::opened) {
        if (errorMessage) *errorMessage = "The device is not open.";
        return false;
    }
    fake::frame = std::move(callback);
    fake::acquiring = true;
    if (fake::status) fake::status(Status::Acquisition, true);
    return true;
}
void HeliotisC4Device::requestStopAcquisition() noexcept
{
    if (!fake::acquiring) return;
    fake::acquiring = false;
    if (fake::status) fake::status(Status::Acquisition, false);
}
void HeliotisC4Device::stopAcquisition()
{
    if (!fake::acquiring) return;
    fake::acquiring = false;
    if (fake::status) fake::status(Status::Acquisition, false);
}
bool HeliotisC4Device::isAcquiring() const { return fake::acquiring; }
bool HeliotisC4Device::isSoftwareTriggeredAcquisition() const { return false; }
bool HeliotisC4Device::isSoftwareTriggerAvailable() const { return false; }
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

/** Spins the session thread until an asynchronous completion arrives. */
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

heliotis::Frame validFrame()
{
    heliotis::Frame frame;
    frame.sequence = 1;
    frame.frameId = "h8-1";
    frame.parts = {{
        heliotis::FramePartKind::Range,
        "Range",
        "Coord3D_ABC32f",
        2,
        2,
        64,
        std::vector<double>{1.0, 2.0, 3.0, 4.0}
    }};
    return frame;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);

    heliotis::FeatureDescriptor rangeFeature;
    rangeFeature.categoryPath = "ImageFormatControl";
    rangeFeature.displayName = "Range";
    rangeFeature.valueText = "1";
    rangeFeature.type = heliotis::FeatureType::Integer;
    rangeFeature.access = heliotis::FeatureAccess::ReadOnly;
    fake::features = {rangeFeature};

    QString title;
    int delivered = 0;
    {
        HeliotisC4PluginSession session(nullptr,
            std::make_unique<heliotis::HeliotisC4Device>(nullptr),
            {{0, 0, "H8 Interface", "H8-0001"}}, QString{}, nullptr);
        session.setTitleChangedCallback([&](const QString& value) { title = value; });
        auto* controller = static_cast<HeliotisC4SourceController*>(session.sourceController());
        require(controller != nullptr, "The session must expose its source controller.");
        controller->setFrameConsumer([&](SessionFrame&&, unsigned) { ++delivered; });

        // A refused start must publish the observed inactive state so a worker proxy does not
        // keep its optimistic active state (the same rule as Gocator).
        bool observedInactive = false;
        QObject::connect(controller, &AbstractSourceController::diagnosticAcquisitionChanged,
            [&](const bool active, const QString&) {
                if (!active) observedInactive = true;
            });
        controller->start();
        QCoreApplication::processEvents();
        require(observedInactive, "A refused start must report the observed inactive state.");
        require(!controller->isGrabbing(), "A refused start must not report an active source.");

        // Acquisition before connect is refused at the session without a widget.
        bool refusedCompletion = false;
        require(controller->requestSingle("missing-connection", [&](DeviceConnectionResult) {
                    refusedCompletion = true;
                }).error == DeviceConnectionError::Disconnected,
            "Single without a connection must report Disconnected.");
        QCoreApplication::processEvents();
        require(!refusedCompletion, "A refused Single must not call its completion.");

        std::optional<DeviceConnectionTargets> targets;
        require(controller->requestConnectionTargets(
                    [&](DeviceConnectionTargets value) { targets = std::move(value); })
                    .error == DeviceConnectionError::None
                && targets && targets->targets.size() == 1,
            "The session must copy its discovery snapshot.");
        const std::string targetId = targets->targets.front().targetId;

        // Connect completes without a widget; the worker and API-only sessions rely on this.
        bool connected = false;
        DeviceConnectionResult connectResult;
        require(controller->requestConnect(targetId, [&](DeviceConnectionResult result) {
                    connectResult = std::move(result);
                    connected = true;
                }).error == DeviceConnectionError::None,
            "Connect must be admitted while disconnected.");
        require(waitFor([&] { return connected; }), "Connect must complete without a widget.");
        require(connectResult.error == DeviceConnectionError::None && connectResult.state
                && connectResult.state->connected,
            "Connect must report a connected state.");
        require(fake::opened, "Connect must open the device.");
        const QString connection = controller->connectionId();
        require(!connection.isEmpty(), "Connect must mint a connection id.");
        require(title == QStringLiteral("Heliotis C4 - H8-0001"),
            "Connect must publish the session title without a widget.");

        // Single completes after the acquisition it armed stops, without a widget.
        bool singleDone = false;
        DeviceConnectionResult singleResult;
        require(controller->requestSingle(connection.toStdString(),
                    [&](DeviceConnectionResult result) {
                        singleResult = std::move(result);
                        singleDone = true;
                    }).error == DeviceConnectionError::None,
            "Single must be admitted on the open connection.");
        require(waitFor([&] { return fake::acquiring; }), "Single must arm the device.");
        fake::frame(validFrame());
        require(waitFor([&] { return delivered == 1; }), "Single must deliver its converted frame.");
        fake::status(heliotis::HeliotisC4Device::Status::Acquisition, false);
        fake::acquiring = false;
        require(waitFor([&] { return singleDone; }), "Single must complete after acquisition stops.");
        require(singleResult.error == DeviceConnectionError::None,
            "Single with one converted frame must succeed.");
        require(!controller->isGrabbing(), "Single must leave acquisition stopped.");

        // Live completes on arm; stop completes through the same headless state signal.
        bool liveDone = false;
        DeviceConnectionResult liveResult;
        require(controller->requestLiveStart(connection.toStdString(),
                    [&](DeviceConnectionResult result) {
                        liveResult = std::move(result);
                        liveDone = true;
                    }).error == DeviceConnectionError::None,
            "Live must be admitted on the open connection.");
        require(waitFor([&] { return liveDone; }), "Live must complete on arm.");
        require(liveResult.error == DeviceConnectionError::None, "Live must not report an error.");
        bool stopDone = false;
        DeviceConnectionResult stopResult;
        require(controller->requestLiveStop(connection.toStdString(),
                    [&](DeviceConnectionResult result) {
                        stopResult = std::move(result);
                        stopDone = true;
                    }).error == DeviceConnectionError::None,
            "Stop must be admitted while armed.");
        require(waitFor([&] { return stopDone; }), "Stop must complete after acquisition stops.");
        require(stopResult.error == DeviceConnectionError::None, "Stop must not report an error.");
        require(!controller->isGrabbing() && !fake::acquiring, "Stop must disarm the device.");

        // Disconnect completes without a widget and clears the connection generation.
        bool disconnected = false;
        DeviceConnectionResult disconnectResult;
        require(controller->requestDisconnect(connection.toStdString(),
                    [&](DeviceConnectionResult result) {
                        disconnectResult = std::move(result);
                        disconnected = true;
                    }).error == DeviceConnectionError::None,
            "Disconnect must be admitted while connected.");
        require(waitFor([&] { return disconnected; }), "Disconnect must complete without a widget.");
        require(disconnectResult.error == DeviceConnectionError::None,
            "Disconnect must not report an error.");
        require(!fake::opened && controller->connectionId().isEmpty(),
            "Disconnect must close the device and clear the connection id.");

        // Close/reopen three times through the same headless flows.
        for (int cycle = 1; cycle <= 3; ++cycle) {
            bool reopened = false;
            DeviceConnectionResult reopenResult;
            require(controller->requestConnect(targetId, [&](DeviceConnectionResult result) {
                        reopenResult = std::move(result);
                        reopened = true;
                    }).error == DeviceConnectionError::None,
                "A reconnect must be admitted.");
            require(waitFor([&] { return reopened; }), "A reconnect must complete.");
            require(reopenResult.error == DeviceConnectionError::None && fake::opened,
                "A reconnect must reopen the device.");
            const QString reopenedConnection = controller->connectionId();
            require(!reopenedConnection.isEmpty() && reopenedConnection != connection,
                "Every reconnect must mint a new connection id.");
            bool closed = false;
            require(controller->requestDisconnect(reopenedConnection.toStdString(),
                        [&](DeviceConnectionResult) { closed = true; })
                        .error == DeviceConnectionError::None,
                "A disconnect after reconnect must be admitted.");
            require(waitFor([&] { return closed; }), "A disconnect after reconnect must complete.");
        }

        require(!controller->isGrabbing() && !fake::opened, "The final state must be disconnected.");
    }

    if (failures == 0) std::cout << "Heliotis headless session lifecycle passed.\n";
    return failures == 0 ? 0 : 1;
}
