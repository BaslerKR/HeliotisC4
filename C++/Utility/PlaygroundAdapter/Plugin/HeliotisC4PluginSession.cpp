#include "HeliotisC4PluginSession.h"

#include "HeliotisC4SourceController.h"

#include <QDebug>
#include <QTimer>
#include <QtConcurrentRun>

#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

HeliotisC4PluginSession::HeliotisC4PluginSession(
    std::shared_ptr<heliotis::HeliotisC4System> system,
    std::unique_ptr<heliotis::HeliotisC4Device> device,
    std::vector<heliotis::DeviceDescriptor> devices,
    QString settingsFile,
    QObject* parent)
    : QObject(parent),
      _system(std::move(system)),
      _device(std::move(device)),
      _devices(std::move(devices)),
      _settingsFile(std::move(settingsFile)),
      _controller(std::make_unique<HeliotisC4SourceController>(_device.get()))
{
    // Device tools, the worker proxy and the Host panel share this session's SDK admission.
    // The controller signals are session-owned so Single, Stop and Disconnect complete without a
    // view; diagnostic sampling stays on the session thread as well.
    _controller->setControlBackend(this);
    connectController();
    _diagnosticsTimer.setInterval(1000);
    QObject::connect(&_diagnosticsTimer, &QTimer::timeout, this,
        &HeliotisC4PluginSession::sampleDiagnostics);
    _diagnosticsTimer.start();
}

HeliotisC4PluginSession::~HeliotisC4PluginSession()
{
    // Pending tool calls fail before the watchers they wait on are dropped.
    if (_controller) _controller->setControlBackend(nullptr);
    if (_device) _device->requestCancelInitialization();
    if (_discoveryWatcher) _discoveryWatcher->waitForFinished();
    if (_connectionWatcher) _connectionWatcher->waitForFinished();
    if (_disconnectionWatcher) _disconnectionWatcher->waitForFinished();
    if (_initializationWatcher) _initializationWatcher->waitForFinished();
    if (_featureWatcher) _featureWatcher->waitForFinished();
    if (_featureReadWatcher) _featureReadWatcher->waitForFinished();
    if (_armWatcher) _armWatcher->waitForFinished();
    _controller.reset();
    if (_device) _device->close();
}

void HeliotisC4PluginSession::connectController()
{
    if (!_controller) return;
    QObject::connect(_controller.get(), &HeliotisC4SourceController::acquisitionStateChanged, this,
        [this](const bool acquiring, const bool, const bool) {
            if (!acquiring) finishCompletion(_acquisitionEndCompletion, {});
            if (!acquiring && _disconnectPending) {
                completeDisconnect();
                return;
            }
            if (!acquiring && _device && _device->isOpened() && !_device->requiresReconnect()
                && !_reconnectRequired && _controller && _controller->parameterWatchesActive()) {
                // Descriptor writability is observed from the device, never
                // cached from the locked state held while acquisition was armed.
                refreshFeatureTree();
            }
        });
    QObject::connect(_controller.get(), &HeliotisC4SourceController::acquisitionError, this,
        [this](const QString& message) {
            // This routing was widget-only; it now always runs on the session so a
            // worker or API-only consumer keeps the same error lifecycle.
            if (_disconnectPending || _disconnectionWatcher) {
                qWarning().noquote() << "[Heliotis C4] Acquisition error retained in the log during disconnect:"
                                     << message;
                return;
            }
            if (!_device || !_device->isOpened()) {
                qWarning().noquote() << "[Heliotis C4] Acquisition error reported after disconnect:" << message;
                return;
            }
            if (_device->requiresReconnect()) {
                // Latch the flag the widget used to clear by disabling its controls;
                // acquisition stays unavailable until a new connection is opened.
                _reconnectRequired = true;
            }
            qWarning().noquote() << "[Heliotis C4] Acquisition error:" << message;
        });
}

void HeliotisC4PluginSession::sampleDiagnostics()
{
    if (!_controller) return;
    const auto value = _controller->sourceRuntimeDiagnostics(0);
    if (!value) return;
    if (!value->grabbing) _diagnosticActive = false;
    if (!_diagnosticActive) {
        _diagnosticClock.start();
        _diagnosticSampleMs = 0;
        _diagnosticLogMs = 0;
        _diagnosticDelivered = value->delivered;
        _diagnosticActive = true;
    }
    const qint64 elapsed = _diagnosticClock.elapsed();
    const qint64 interval = elapsed - _diagnosticSampleMs;
    const double fps = interval > 0 && value->delivered >= _diagnosticDelivered
        ? double(value->delivered - _diagnosticDelivered) * 1000.0 / double(interval) : 0.0;
    _diagnosticDelivered = value->delivered;
    _diagnosticSampleMs = elapsed;
    if (elapsed - _diagnosticLogMs >= 5000) {
        qInfo().noquote() << QStringLiteral("[Heliotis C4] diagnostics: delivered=%1 failed=%2 fps=%3")
            .arg(qulonglong(value->delivered)).arg(qulonglong(value->failed)).arg(fps, 0, 'f', 1);
        _diagnosticLogMs = elapsed;
    }
}

QString HeliotisC4PluginSession::title() const
{
    return _title;
}

DeviceSessionMetadata HeliotisC4PluginSession::sessionMetadata() const
{
    return {QStringLiteral("Plugins/HeliotisC4/Favorites/Features")};
}

std::vector<DevicePluginDock> HeliotisC4PluginSession::createDockWidgets(QWidget* parent)
{
    Q_UNUSED(parent)
    return {};
}

AbstractSourceController* HeliotisC4PluginSession::sourceController() const
{
    return _controller.get();
}

unsigned int HeliotisC4PluginSession::capabilities() const noexcept
{
    return DevicePluginSessionCapability::GraphicsEngine
        | DevicePluginSessionCapability::ScriptEditor;
}

void HeliotisC4PluginSession::setTitleChangedCallback(std::function<void(const QString&)> callback)
{
    _titleChanged = std::move(callback);
}

void HeliotisC4PluginSession::finishCompletion(Completion& completion, const QString& error)
{
    if (auto pending = std::exchange(completion, {})) pending(error);
}

bool HeliotisC4PluginSession::sdkOperationPending() const
{
    // Device tools run through these session flows and their watchers; only
    // API parameter work holds the SDK feature channel outside the session.
    return controlBusy() || (_controller && _controller->parameterWorkActive());
}

// HeliotisC4ControlBackend: device tools reuse the GUI flows below.
bool HeliotisC4PluginSession::controlBusy() const
{
    return _discoveryWatcher || _connectionWatcher || _disconnectionWatcher || _initializationWatcher
        || _featureWatcher || _featureReadWatcher || _armWatcher || _disconnectPending;
}

std::vector<heliotis::DeviceDescriptor> HeliotisC4PluginSession::targets() const
{
    return _devices;
}

std::optional<heliotis::DeviceDescriptor> HeliotisC4PluginSession::connectedTarget() const
{
    return _connectedTarget;
}

bool HeliotisC4PluginSession::discover(Completion completion)
{
    return refreshDevices(std::move(completion));
}

bool HeliotisC4PluginSession::connectTarget(const std::size_t index, Completion completion)
{
    if (index > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    return connectDevice(static_cast<int>(index), std::move(completion));
}

bool HeliotisC4PluginSession::disconnect(Completion completion)
{
    return requestDisconnect(std::move(completion));
}

bool HeliotisC4PluginSession::arm(const bool continuous, Completion completion)
{
    return requestAcquisitionArm(continuous, std::move(completion));
}

bool HeliotisC4PluginSession::stop(Completion completion)
{
    if (!_controller || !_controller->isGrabbing() || _acquisitionEndCompletion) return false;
    _acquisitionEndCompletion = std::move(completion);
    requestAcquisitionStop();
    return true;
}

bool HeliotisC4PluginSession::initialize(Completion completion, std::function<bool()> cancelled)
{
    return initializeH8ReferenceProfile(std::move(completion), std::move(cancelled));
}

void HeliotisC4PluginSession::requestAcquisitionStop()
{
    if (_controller) _controller->requestStop();
}

bool HeliotisC4PluginSession::refreshDevices(Completion completion)
{
    if (!_system || sdkOperationPending() || (_device && _device->isOpened())
        || (_controller && _controller->isGrabbing())) {
        qWarning().noquote() << "[Heliotis C4] Device discovery ignored because another SDK operation is pending.";
        return false;
    }

    qInfo().noquote() << "[Heliotis C4] Asynchronous device discovery requested.";
    _discoveryCompletion = std::move(completion);
    _discoveryWatcher = std::make_unique<QFutureWatcher<HeliotisDiscoveryResult>>();
    auto* watcher = _discoveryWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisDiscoveryResult>::finished, this,
        [this, watcher]() {
            if (!_discoveryWatcher || _discoveryWatcher.get() != watcher) return;
            const HeliotisDiscoveryResult result = watcher->result();
            _discoveryWatcher.reset();
            if (!result.error.isEmpty()) {
                qWarning().noquote() << "[Heliotis C4] Device discovery failed after"
                                     << result.elapsedMs << "ms:" << result.error;
                finishCompletion(_discoveryCompletion, result.error);
                return;
            }
            _devices = result.devices;
            qInfo().noquote() << "[Heliotis C4] Device discovery completed with"
                              << _devices.size() << "device(s) in" << result.elapsedMs << "ms.";
            finishCompletion(_discoveryCompletion, {});
        });
    watcher->setFuture(QtConcurrent::run([system = _system] {
        const auto started = std::chrono::steady_clock::now();
        std::string error;
        std::vector<heliotis::DeviceDescriptor> devices;
        try {
            devices = system->discoverDevices(&error);
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "An unknown exception occurred while discovering Heliotis devices.";
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return HeliotisDiscoveryResult{
            std::move(devices),
            QString::fromStdString(error),
            static_cast<qint64>(elapsed.count()),
        };
    }));
    return true;
}

bool HeliotisC4PluginSession::connectDevice(const int index, Completion completion)
{
    if (sdkOperationPending()) {
        qWarning().noquote() << "[Heliotis C4] Connection ignored because another SDK operation is pending.";
        return false;
    }
    if (_device && _device->isOpened()) {
        qWarning().noquote() << "[Heliotis C4] Connection ignored because the session already owns an open device.";
        return false;
    }
    if (index < 0 || index >= static_cast<int>(_devices.size())) {
        qWarning().noquote() << "[Heliotis C4] The selected Heliotis device is no longer available.";
        return false;
    }

    const heliotis::DeviceDescriptor descriptor = _devices[static_cast<std::size_t>(index)];
    qInfo().noquote() << "[Heliotis C4] Connecting to" << QString::fromStdString(descriptor.deviceName)
                      << "interface" << descriptor.interfaceIndex << "device" << descriptor.deviceIndex;
    _connectCompletion = std::move(completion);
    _connectionWatcher = std::make_unique<QFutureWatcher<HeliotisConnectionResult>>();
    auto* watcher = _connectionWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisConnectionResult>::finished, this,
        [this, watcher, descriptor]() {
            if (!_connectionWatcher || _connectionWatcher.get() != watcher) return;
            const HeliotisConnectionResult result = watcher->result();
            _connectionWatcher.reset();
            qInfo().noquote() << "[Heliotis C4] Asynchronous device connection finished:"
                              << "opened=" << result.opened
                              << "openElapsedMs=" << result.openElapsedMs;
            if (!result.opened) {
                qWarning().noquote() << "[Heliotis C4] Device connection failed:" << result.openError;
                finishCompletion(_connectCompletion, result.openError);
                return;
            }
            _connectedTarget = descriptor;
            // A new connection clears the latched reconnect-required error.
            _reconnectRequired = false;
            const QString deviceName = QString::fromStdString(_device->connectedDeviceName());
            if (!deviceName.isEmpty()) {
                _title = QStringLiteral("Heliotis C4 - %1").arg(deviceName);
                if (_titleChanged) _titleChanged(_title);
            }
            refreshFeatureTree([this](const bool featuresRefreshed) {
                qInfo().noquote() << "[Heliotis C4] Device connection completed."
                                  << "The existing device configuration was preserved;"
                                  << "Init remains the explicit default-profile and StageInit action."
                                  << "featureRefresh=" << featuresRefreshed;
                // The connection stands even when the follow-up feature read fails.
                finishCompletion(_connectCompletion, {});
            });
        });
    watcher->setFuture(QtConcurrent::run([device = _device.get(), descriptor]() {
        const auto openStarted = std::chrono::steady_clock::now();
        std::string openError;
        bool opened = false;
        try {
            opened = device && device->open(descriptor, &openError);
        } catch (const std::exception& exception) {
            openError = exception.what();
        } catch (...) {
            openError = "An unknown exception occurred while connecting to the Heliotis device.";
        }
        const auto openElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - openStarted);
        return HeliotisConnectionResult{
            opened,
            QString::fromStdString(openError.empty() && !opened
                ? std::string("Heliotis device connection failed.")
                : openError),
            static_cast<qint64>(openElapsed.count()),
        };
    }));
    return true;
}

bool HeliotisC4PluginSession::requestDisconnect(Completion completion)
{
    if (!_device || sdkOperationPending()) return false;
    _disconnectCompletion = std::move(completion);
    if (_controller && _controller->isGrabbing()) {
        _disconnectPending = true;
        _controller->requestStop();
        return true;
    }
    completeDisconnect();
    return true;
}

void HeliotisC4PluginSession::completeDisconnect()
{
    if (!_device || _disconnectionWatcher) return;
    _disconnectPending = true;
    qInfo().noquote() << "[Heliotis C4] Asynchronous device disconnect started.";

    _disconnectionWatcher = std::make_unique<QFutureWatcher<HeliotisDisconnectionResult>>();
    auto* watcher = _disconnectionWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisDisconnectionResult>::finished, this,
        [this, watcher]() {
            if (!_disconnectionWatcher || _disconnectionWatcher.get() != watcher) return;
            const HeliotisDisconnectionResult result = watcher->result();
            _disconnectionWatcher.reset();
            _disconnectPending = false;
            _connectedTarget.reset();
            _reconnectRequired = false;
            qInfo().noquote() << "[Heliotis C4] Asynchronous device disconnect completed:"
                              << "elapsedMs=" << result.elapsedMs;
            finishCompletion(_disconnectCompletion, {});
        });
    watcher->setFuture(QtConcurrent::run([
        controller = _controller.get(), device = _device.get()] {
        const auto started = std::chrono::steady_clock::now();
        if (controller) controller->stop();
        if (device) device->close();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return HeliotisDisconnectionResult{static_cast<qint64>(elapsed.count())};
    }));
}

bool HeliotisC4PluginSession::requestAcquisitionArm(const bool continuous, Completion completion)
{
    if (!_controller || !_device || !_device->isOpened()
        || _device->requiresReconnect() || _reconnectRequired || _controller->isGrabbing()
        || sdkOperationPending() || _acquisitionEndCompletion) {
        qWarning().noquote() << "[Heliotis C4] Acquisition arm ignored because the device is unavailable, active, or busy.";
        return false;
    }

    qInfo().noquote() << "[Heliotis C4] Asynchronous acquisition arm requested:"
                      << (continuous ? "Live" : "Single");
    _armCompletion = std::move(completion);
    _armWatcher = std::make_unique<QFutureWatcher<HeliotisAcquisitionArmResult>>();
    auto* watcher = _armWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisAcquisitionArmResult>::finished, this,
        [this, watcher]() {
            if (!_armWatcher || _armWatcher.get() != watcher) return;
            const HeliotisAcquisitionArmResult result = watcher->result();
            _armWatcher.reset();
            if (!result.taskError.isEmpty()) {
                qWarning().noquote() << "[Heliotis C4] Acquisition arm task raised an exception:"
                                     << result.taskError;
            }
            qInfo().noquote() << "[Heliotis C4] Asynchronous acquisition arm finished:"
                              << "mode=" << (result.continuous ? "Live" : "Single")
                              << "started=" << result.started
                              << "controllerActive=" << (_controller && _controller->isGrabbing())
                              << "deviceActive=" << (_device && _device->isAcquiring())
                              << "elapsedMs=" << result.elapsedMs;
            if (!result.started) {
                const QString error = !result.taskError.isEmpty() ? result.taskError
                    : QString::fromStdString(_device ? _device->lastAcquisitionError() : std::string{});
                finishCompletion(_armCompletion, error.isEmpty()
                    ? QStringLiteral("Heliotis acquisition could not be started.") : error);
            } else if (result.continuous || !_controller || !_controller->isGrabbing()) {
                // Live completes on arm; a Single that already stopped completes now.
                finishCompletion(_armCompletion, {});
            } else {
                // Single completes when the acquisition it armed stops.
                _acquisitionEndCompletion = std::exchange(_armCompletion, {});
            }
        });
    watcher->setFuture(QtConcurrent::run([controller = _controller.get(), continuous] {
        const auto startedAt = std::chrono::steady_clock::now();
        bool started = false;
        QString taskError;
        try {
            started = controller && (continuous
                ? controller->startLive()
                : controller->grabOne());
        } catch (const std::exception& exception) {
            taskError = QString::fromLocal8Bit(exception.what());
        } catch (...) {
            taskError = QStringLiteral("An unknown exception occurred while arming Heliotis acquisition.");
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt);
        return HeliotisAcquisitionArmResult{
            started,
            continuous,
            static_cast<qint64>(elapsed.count()),
            taskError,
        };
    }));
    return true;
}

bool HeliotisC4PluginSession::initializeH8ReferenceProfile(Completion completion, std::function<bool()> cancelled)
{
    // The declared init tool uses this admission and completion path only.
    if (!_device || !_device->isOpened()
        || (_controller && _controller->isGrabbing()) || sdkOperationPending()) {
        qWarning().noquote() << "[Heliotis C4] H8 reference-profile initialization ignored because the device is unavailable, active, or busy.";
        return false;
    }

    qInfo().noquote() << "[Heliotis C4] Explicit H8 default-profile initialization requested."
                      << "Applying all C4Utility h8SurfSimple component, chunk, trigger, encoder, motion, processing, and illumination defaults."
                      << "StageInit follows the verified encoder and motion defaults and may move the stage."
                      << "Optional geometry or illumination capability failures are warning-only.";
    _initializationWatcher = std::make_unique<QFutureWatcher<HeliotisInitializationResult>>();
    auto* watcher = _initializationWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisInitializationResult>::finished, this,
        [this, watcher, completion = std::move(completion)]() mutable {
            if (!_initializationWatcher || _initializationWatcher.get() != watcher) return;
            const HeliotisInitializationResult result = watcher->result();
            _initializationWatcher.reset();

            refreshFeatureTree([this, result, completion = std::move(completion)](const bool featuresRefreshed) mutable {
                if (!result.initialized) {
                    qWarning().noquote() << "[Heliotis C4] Explicit H8 default-profile initialization failed; connection and capture controls are preserved:"
                                         << result.error << "elapsedMs=" << result.elapsedMs;
                } else {
                    qInfo().noquote() << "[Heliotis C4] Explicit H8 default-profile initialization completed; acquisition remains available."
                                      << "featureRefresh=" << featuresRefreshed
                                      << "elapsedMs=" << result.elapsedMs;
                }
                if (completion) completion(result.initialized ? QString{} : result.error);
            });
        });
    watcher->setFuture(QtConcurrent::run([device = _device.get(), cancelled = std::move(cancelled)]() {
        const auto started = std::chrono::steady_clock::now();
        std::string error;
        bool initialized = false;
        try {
            if (cancelled && cancelled()) {
                error = "H8 reference-profile initialization was cancelled before it started.";
            } else {
                initialized = device && device->configureH8SurfaceExample(&error, cancelled);
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "An unknown exception occurred while applying the Heliotis H8 reference profile.";
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return HeliotisInitializationResult{
            initialized,
            QString::fromStdString(error.empty() && !initialized
                ? std::string("Heliotis H8 default-profile initialization failed; capture controls remain available.")
                : error),
            static_cast<qint64>(elapsed.count()),
        };
    }));
    return true;
}

bool HeliotisC4PluginSession::runFeatureOperation(
    FeatureOperation operation,
    const bool refreshFeaturesAfterSuccess,
    std::function<void(bool)> completion)
{
    if (!_device || !_device->isOpened()
        || sdkOperationPending() || _device->requiresReconnect()) {
        qWarning().noquote() << "[Heliotis C4] Feature operation ignored because the device is unavailable or busy.";
        if (completion) completion(false);
        return false;
    }

    _featureWatcher = std::make_unique<QFutureWatcher<HeliotisFeatureOperationResult>>();
    auto* watcher = _featureWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisFeatureOperationResult>::finished, this,
        [this, watcher, refreshFeaturesAfterSuccess, completion = std::move(completion)]() mutable {
            if (!_featureWatcher || _featureWatcher.get() != watcher) return;
            const HeliotisFeatureOperationResult result = watcher->result();
            _featureWatcher.reset();
            qInfo().noquote() << "[Heliotis C4] Asynchronous feature operation finished:"
                              << "succeeded=" << result.succeeded
                              << "elapsedMs=" << result.elapsedMs;
            if (!result.succeeded) {
                qWarning().noquote() << "[Heliotis C4] Feature operation failed:" << result.error;
                if (completion) completion(false);
                return;
            }
            if (refreshFeaturesAfterSuccess) {
                refreshFeatureTree([completion = std::move(completion)](const bool refreshed) mutable {
                    if (completion) completion(refreshed);
                });
                return;
            }
            if (completion) completion(true);
        });
    watcher->setFuture(QtConcurrent::run([device = _device.get(), operation = std::move(operation)]() mutable {
        const auto started = std::chrono::steady_clock::now();
        std::string error;
        bool succeeded = false;
        try {
            succeeded = device && operation(device, &error);
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "An unknown exception occurred while applying a Heliotis feature change.";
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return HeliotisFeatureOperationResult{
            succeeded,
            QString::fromStdString(error.empty() && !succeeded
                ? std::string("Heliotis feature operation failed.")
                : error),
            static_cast<qint64>(elapsed.count()),
        };
    }));
    return true;
}

void HeliotisC4PluginSession::refreshFeatureTree(std::function<void(bool)> completion)
{
    if (!_device || !_device->isOpened() || _discoveryWatcher
        || _connectionWatcher || _disconnectionWatcher || _initializationWatcher
        || _featureWatcher || _disconnectPending || _device->requiresReconnect()
        || (_controller && _controller->parameterWorkActive())) {
        qWarning().noquote() << "[Heliotis C4] Feature refresh skipped because the device is unavailable or busy.";
        if (completion) completion(false);
        return;
    }
    if (_controller && _controller->isGrabbing()) {
        qWarning().noquote() << "[Heliotis C4] Feature refresh skipped while acquisition is armed.";
        if (completion) completion(false);
        return;
    }
    if (_armWatcher) {
        qWarning().noquote() << "[Heliotis C4] Feature refresh skipped while acquisition is arming.";
        if (completion) completion(false);
        return;
    }
    if (_featureReadWatcher) {
        qWarning().noquote() << "[Heliotis C4] Feature refresh ignored because another refresh is already running.";
        if (completion) completion(false);
        return;
    }

    _featureRefreshCompletion = std::move(completion);
    _featureReadWatcher = std::make_unique<QFutureWatcher<HeliotisFeatureReadResult>>();
    auto* watcher = _featureReadWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisFeatureReadResult>::finished, this,
        [this, watcher]() {
            if (!_featureReadWatcher || _featureReadWatcher.get() != watcher) return;
            const HeliotisFeatureReadResult result = watcher->result();
            _featureReadWatcher.reset();
            auto completion = std::move(_featureRefreshCompletion);
            _featureRefreshCompletion = {};

            const bool succeeded = result.error.isEmpty();
            if (!succeeded) {
                qWarning().noquote() << "[Heliotis C4] Feature read failed after"
                                     << result.elapsedMs << "ms:" << result.error;
            } else {
                // One observation feeds the API subscriptions; consumers never
                // depend on a visible view.
                if (_controller) _controller->publishParameterSnapshot(result.features);
                qInfo().noquote() << "[Heliotis C4] Feature tree refreshed with"
                                  << result.features.size() << "feature(s) in"
                                  << result.elapsedMs << "ms.";
            }
            if (completion) completion(succeeded);
        });
    watcher->setFuture(QtConcurrent::run([device = _device.get()] {
        const auto started = std::chrono::steady_clock::now();
        std::string error;
        heliotis::HeliotisC4::FeatureList features;
        try {
            features = device->readFeatures(&error);
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "An unknown exception occurred while reading Heliotis features.";
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return HeliotisFeatureReadResult{
            std::move(features),
            QString::fromStdString(error),
            static_cast<qint64>(elapsed.count()),
        };
    }));
}
