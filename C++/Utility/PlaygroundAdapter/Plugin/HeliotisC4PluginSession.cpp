#include "HeliotisC4PluginSession.h"

#include "HeliotisC4SourceController.h"

#include "Utility/Qt/QHeliotisC4Widget.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
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

Q_LOGGING_CATEGORY(heliotisOperationDiagnosticLog, "diagnostics.HeliotisC4.Operations", QtInfoMsg)

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
    // GUI controls and device tools share this session's SDK admission. The controller signals
    // are session-owned so a headless session completes Single, Stop and Disconnect without a
    // widget; the widget only mirrors them when it exists.
    _controller->setControlBackend(this);
    connectController();
}

HeliotisC4PluginSession::~HeliotisC4PluginSession()
{
    // The control widget is owned by the consumer window and can outlive this session; drop its
    // signal handlers first so no queued widget signal reaches a destroyed session.
    if (_widget) QObject::disconnect(_widget, nullptr, _widget, nullptr);
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
    QObject::connect(_controller.get(), &HeliotisC4SourceController::controlChanged, this, [this]() {
        // API work holds the same SDK channel as GUI feature edits. Mirror only
        // its transitions so session-owned pending states remain authoritative.
        // Tool calls drive the session flows below, which present their own state.
        // While armed the only admitted call is TriggerSoftware, which is an
        // acquisition command rather than a feature edit; arming is refused
        // while parameter work runs, so no shown state can be left behind.
        const bool busy = _controller && _controller->parameterWorkActive() && !_controller->isGrabbing();
        if (busy == _externalBusyShown) return;
        _externalBusyShown = busy;
        if (_widget && !_featureWatcher && !_featureReadWatcher && !_initializationWatcher) {
            _widget->setFeatureOperationPending(busy);
        }
    });
    QObject::connect(_controller.get(), &HeliotisC4SourceController::parametersApplied, this, [this]() {
        // An API write already returns its readback; only the visible tree needs a refresh.
        if (!_widget) return;
        refreshFeatureTree();
    });
    QObject::connect(_controller.get(), &HeliotisC4SourceController::acquisitionStateChanged, this,
        [this](const bool acquiring, const bool continuous, const bool softwareTriggerAvailable) {
            if (!acquiring) finishCompletion(_acquisitionEndCompletion, {});
            if (!acquiring && _disconnectPending) {
                completeDisconnect();
                return;
            }
            if (_widget) {
                _widget->setAcquisitionState(acquiring, continuous, softwareTriggerAvailable);
                const auto value = _controller->sourceRuntimeDiagnostics(0);
                if (value) _widget->setRuntimeDiagnostics({value->grabbing, value->delivered, value->failed});
            }
            if (!acquiring && _device && _device->isOpened() && !_device->requiresReconnect()
                && _controller && _controller->parameterWatchesActive()) {
                // Descriptor writability is observed from the device, never
                // cached from the locked state held while acquisition was armed.
                refreshFeatureTree();
            } else if (!acquiring && _widget && heliotisOperationDiagnosticLog().isDebugEnabled()
                && _device && _device->isOpened() && !_device->requiresReconnect()) {
                refreshFeatureTree();
            }
        });
    QObject::connect(_controller.get(), &HeliotisC4SourceController::acquisitionFrameReceived, this,
        [this]() {
            if (_widget) _widget->setSoftwareTriggerPending(false);
        });
    QObject::connect(_controller.get(), &HeliotisC4SourceController::acquisitionError, this,
        [this](const QString& message) {
            if (!_widget) {
                qWarning().noquote() << "[Heliotis C4] Acquisition error:" << message;
                return;
            }
            if (_disconnectPending || _disconnectionWatcher) {
                qWarning().noquote() << "[Heliotis C4] Acquisition error retained in the log during disconnect:"
                                     << message;
                return;
            }
            if (!_device || !_device->isOpened()) {
                qWarning().noquote() << "[Heliotis C4] Acquisition error reported after disconnect:" << message;
                return;
            }
            if (_device && _device->requiresReconnect()) {
                _widget->setAcquisitionAvailable(false);
            }
            _widget->setAcquisitionError(message);
        });
}

QString HeliotisC4PluginSession::title() const
{
    return _title;
}

std::vector<DevicePluginDock> HeliotisC4PluginSession::createDockWidgets(QWidget* parent)
{
    if (_widget) {
        return {{QStringLiteral("device-controls"), QStringLiteral("Device Controls"),
                 Qt::LeftDockWidgetArea, _widget, true}};
    }

    _widget = new QHeliotisC4Widget(parent, _settingsFile, _settingsFile.isEmpty()
        ? QString{} : QStringLiteral("Plugins/HeliotisC4/Favorites/Features"));
    _widget->setDiscoveredDevices(_devices);
    auto* diagnosticsTimer = new QTimer(_widget);
    diagnosticsTimer->setInterval(1000);
    QObject::connect(diagnosticsTimer, &QTimer::timeout, _widget, [this] {
        if (!_controller || !_widget) return;
        const auto value = _controller->sourceRuntimeDiagnostics(0);
        if (value) _widget->setRuntimeDiagnostics({value->grabbing, value->delivered, value->failed});
    });
    diagnosticsTimer->start();
    QObject::connect(_widget, &QHeliotisC4Widget::refreshRequested, _widget, [this]() {
        static_cast<void>(refreshDevices());
    });
    QObject::connect(_widget, &QHeliotisC4Widget::connectRequested, _widget, [this](const int index) {
        if (sdkOperationPending()) {
            qWarning().noquote() << "[Heliotis C4] Connection ignored because another SDK operation is pending.";
            _widget->setConnectionError(QObject::tr("Wait for the current Heliotis operation before connecting."));
            return;
        }
        static_cast<void>(connectDevice(index));
    });
    QObject::connect(_widget, &QHeliotisC4Widget::disconnectRequested, _widget, [this]() {
        qInfo().noquote() << "[Heliotis C4] Disconnect requested.";
        if (!requestDisconnect()) {
            qWarning().noquote() << "[Heliotis C4] Disconnect ignored while another SDK operation is pending.";
            _widget->setConnectionState(_device && _device->isOpened());
        }
    });
    QObject::connect(_widget, &QHeliotisC4Widget::grabOneRequested, _widget, [this]() {
        static_cast<void>(requestAcquisitionArm(false));
    });
    QObject::connect(_widget, &QHeliotisC4Widget::initializationRequested, _widget, [this]() {
        initializeH8ReferenceProfile();
    });
    QObject::connect(_widget, &QHeliotisC4Widget::stopRequested, _widget, [this]() {
        requestAcquisitionStop();
    });
    QObject::connect(_widget, &QHeliotisC4Widget::liveGrabToggled, _widget, [this](const bool enabled) {
        if (enabled) static_cast<void>(requestAcquisitionArm(true));
        else requestAcquisitionStop();
    });
    QObject::connect(_widget, &QHeliotisC4Widget::featureWriteRequested, _widget,
        [this](const QString& name, const QString& value) {
            runFeatureOperation([name, value](heliotis::HeliotisC4Device* device, std::string* error) {
                const bool accepted = device->writeFeature(name.toStdString(), value.toStdString(), error);
                qCDebug(heliotisOperationDiagnosticLog).noquote() << "@diagnostic " + QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"event", "parameter_result"}, {"fields", QJsonObject{{"name", name}, {"requestedValue", value},
                    {"accepted", accepted}, {"error", error ? QString::fromStdString(*error) : QString{}}}}}).toJson(QJsonDocument::Compact));
                return accepted;
            });
        });
    QObject::connect(_widget, &QHeliotisC4Widget::featureCommandRequested, _widget,
        [this](const QString& name) {
            if (name == QStringLiteral("TriggerSoftware")) {
                if (_widget) _widget->setSoftwareTriggerPending(true);
                runFeatureOperation([](heliotis::HeliotisC4Device* device, std::string* error) {
                    return device->triggerSoftware(error);
                }, false, [this](const bool succeeded) {
                    if (_widget && !succeeded) _widget->setSoftwareTriggerPending(false);
                });
                return;
            }
            runFeatureOperation([name](heliotis::HeliotisC4Device* device, std::string* error) {
                return device->executeCommand(name.toStdString(), error);
            });
        });
    return {{QStringLiteral("device-controls"), QStringLiteral("Device Controls"),
             Qt::LeftDockWidgetArea, _widget, true}};
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
    if (_widget && !sdkOperationPending()) _widget->setConnectionPending(true);
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
    if (_widget) _widget->setAcquisitionStopPending();
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
    if (_widget) _widget->setDiscoveryPending(true);
    _discoveryCompletion = std::move(completion);
    _discoveryWatcher = std::make_unique<QFutureWatcher<HeliotisDiscoveryResult>>();
    auto* watcher = _discoveryWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisDiscoveryResult>::finished, this,
        [this, watcher]() {
            if (!_discoveryWatcher || _discoveryWatcher.get() != watcher) return;
            const HeliotisDiscoveryResult result = watcher->result();
            _discoveryWatcher.reset();
            if (_widget) _widget->setDiscoveryPending(false);
            if (!result.error.isEmpty()) {
                qWarning().noquote() << "[Heliotis C4] Device discovery failed after"
                                     << result.elapsedMs << "ms:" << result.error;
                if (_widget) _widget->setDiscoveryError(result.error);
                finishCompletion(_discoveryCompletion, result.error);
                return;
            }
            _devices = result.devices;
            if (_widget) _widget->setDiscoveredDevices(_devices);
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
        if (_widget) _widget->setConnectionError(QObject::tr("Wait for the current Heliotis operation before connecting."));
        return false;
    }
    if (_device && _device->isOpened()) {
        qWarning().noquote() << "[Heliotis C4] Connection ignored because the session already owns an open device.";
        if (_widget) {
            _widget->setConnectionState(true);
            _widget->setAcquisitionAvailable(!_device->requiresReconnect());
        }
        return false;
    }
    if (index < 0 || index >= static_cast<int>(_devices.size())) {
        if (_widget) _widget->setConnectionError(QObject::tr("The selected Heliotis device is no longer available."));
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
            if (_widget) _widget->setConnectionPending(false);
            qInfo().noquote() << "[Heliotis C4] Asynchronous device connection finished:"
                              << "opened=" << result.opened
                              << "openElapsedMs=" << result.openElapsedMs;
            if (!result.opened) {
                qWarning().noquote() << "[Heliotis C4] Device connection failed:" << result.openError;
                if (_widget) _widget->setConnectionError(result.openError);
                finishCompletion(_connectCompletion, result.openError);
                return;
            }
            _connectedTarget = descriptor;
            const QString deviceName = QString::fromStdString(_device->connectedDeviceName());
            if (!deviceName.isEmpty()) {
                _title = QStringLiteral("Heliotis C4 - %1").arg(deviceName);
                if (_titleChanged) _titleChanged(_title);
            }
            if (_widget) {
                _widget->setConnectedDeviceName(deviceName);
                _widget->setConnectionState(true);
                _widget->setAcquisitionAvailable(true);
            }
            refreshFeatureTree([this](const bool featuresRefreshed) {
                if (_widget) _widget->setAcquisitionAvailable(true);
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
        if (_widget) _widget->setDisconnectionPending();
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
    if (_widget) _widget->setDisconnectionPending();
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
            if (_widget) {
                _widget->setFeatures({});
                _widget->setConnectionState(false);
            }
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
        || _device->requiresReconnect() || _controller->isGrabbing()
        || sdkOperationPending() || _acquisitionEndCompletion) {
        qWarning().noquote() << "[Heliotis C4] Acquisition arm ignored because the device is unavailable, active, or busy.";
        return false;
    }

    qInfo().noquote() << "[Heliotis C4] Asynchronous acquisition arm requested:"
                      << (continuous ? "Live" : "Single");
    _armCompletion = std::move(completion);
    if (_widget) _widget->setAcquisitionArmPending(true, continuous);
    _armWatcher = std::make_unique<QFutureWatcher<HeliotisAcquisitionArmResult>>();
    auto* watcher = _armWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisAcquisitionArmResult>::finished, this,
        [this, watcher]() {
            if (!_armWatcher || _armWatcher.get() != watcher) return;
            const HeliotisAcquisitionArmResult result = watcher->result();
            _armWatcher.reset();
            if (_widget) _widget->setAcquisitionArmPending(false, result.continuous);
            if (!result.taskError.isEmpty()) {
                qWarning().noquote() << "[Heliotis C4] Acquisition arm task raised an exception:"
                                     << result.taskError;
                if (_widget) _widget->setAcquisitionError(result.taskError);
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
    // The control widget and the declared init tool share this admission and
    // completion path; neither depends on the other existing.
    if (!_device || !_device->isOpened()
        || (_controller && _controller->isGrabbing()) || sdkOperationPending()) {
        qWarning().noquote() << "[Heliotis C4] H8 reference-profile initialization ignored because the device is unavailable, active, or busy.";
        return false;
    }

    qInfo().noquote() << "[Heliotis C4] Explicit H8 default-profile initialization requested."
                      << "Applying all C4Utility h8SurfSimple component, chunk, trigger, encoder, motion, processing, and illumination defaults."
                      << "StageInit follows the verified encoder and motion defaults and may move the stage."
                      << "Optional geometry or illumination capability failures are warning-only.";
    if (_widget) _widget->setInitializationPending(true);
    _initializationWatcher = std::make_unique<QFutureWatcher<HeliotisInitializationResult>>();
    auto* watcher = _initializationWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<HeliotisInitializationResult>::finished, this,
        [this, watcher, completion = std::move(completion)]() mutable {
            if (!_initializationWatcher || _initializationWatcher.get() != watcher) return;
            const HeliotisInitializationResult result = watcher->result();
            _initializationWatcher.reset();

            refreshFeatureTree([this, result, completion = std::move(completion)](const bool featuresRefreshed) mutable {
                if (_widget) {
                    if (!result.initialized) {
                        _widget->setInitializationError(result.error);
                    } else {
                        _widget->setInitializationPending(false);
                        _widget->setAcquisitionAvailable(true);
                    }
                }
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

    if (_widget) _widget->setFeatureOperationPending(true);
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
                if (_widget) {
                    _widget->setFeatureOperationPending(false);
                    _widget->setFeatureError(result.error);
                }
                qWarning().noquote() << "[Heliotis C4] Feature operation failed:" << result.error;
                if (completion) completion(false);
                return;
            }
            if (refreshFeaturesAfterSuccess) {
                refreshFeatureTree([this, completion = std::move(completion)](const bool refreshed) mutable {
                    if (_widget) _widget->setFeatureOperationPending(false);
                    if (completion) completion(refreshed);
                });
                return;
            }
            if (_widget) _widget->setFeatureOperationPending(false);
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

    if (_widget) _widget->setFeatureRefreshPending(true);
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

            if (_widget) _widget->setFeatureRefreshPending(false);
            const bool succeeded = result.error.isEmpty();
            if (!succeeded) {
                qWarning().noquote() << "[Heliotis C4] Feature read failed after"
                                     << result.elapsedMs << "ms:" << result.error;
                if (_widget) _widget->setFeatureError(result.error);
            } else {
                if (_widget) _widget->setFeatures(result.features);
                // One observation feeds both the widget and API subscriptions;
                // consumers never depend on a visible widget.
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
