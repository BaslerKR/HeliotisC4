#include "DevicePluginTemplate.h"

#include "HeliotisC4SourceController.h"
#include "HeliotisC4System.h"
#include "Utility/Qt/QHeliotisC4Widget.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QStringList>
#include <QtConcurrentRun>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

/** Carries one background device-open result to the UI thread. */
struct HeliotisConnectionResult {
    bool opened = false;
    QString openError;
    qint64 openElapsedMs = 0;
};

/** Carries one background discovery snapshot to the UI thread. */
struct HeliotisDiscoveryResult {
    std::vector<heliotis::DeviceDescriptor> devices;
    QString error;
    qint64 elapsedMs = 0;
};

/** Carries background shutdown timing to the UI thread. */
struct HeliotisDisconnectionResult {
    qint64 elapsedMs = 0;
};

/** Carries one explicit H8 reference-profile result while preserving connection state. */
struct HeliotisInitializationResult {
    bool initialized = false;
    QString error;
    qint64 elapsedMs = 0;
};

/** Carries one serialized feature mutation result to the UI thread. */
struct HeliotisFeatureOperationResult {
    bool succeeded = false;
    QString error;
    qint64 elapsedMs = 0;
};

/** Carries one background feature-tree snapshot to the UI thread. */
struct HeliotisFeatureReadResult {
    heliotis::HeliotisC4::FeatureList features;
    QString error;
    qint64 elapsedMs = 0;
};

/** Carries one background Single or Live arm result to the UI thread. */
struct HeliotisAcquisitionArmResult {
    bool started = false;
    bool continuous = false;
    qint64 elapsedMs = 0;
    QString taskError;
};

/** Describes the one C4Utility root selected before C4Hdl is opened. */
struct HeliotisRuntimeSelection {
    QString root;
    QString c4UtilityEnvironment;
    QString diaphusProducer;
    QString source;
};

/**
 * Checks the runtime files required by the module without opening C4Hdl.
 *
 * @param root Candidate C4Utility root.
 * @return True when the required C4Hdl, GenApi, and Diaphus files exist.
 */
bool isCompleteHeliotisRuntime(const QString& root)
{
    if (root.trimmed().isEmpty()) return false;

    const QDir runtimeRoot(QDir::cleanPath(root));
    const QStringList requiredFiles {
        QStringLiteral("c4hdl/win64-x64/c/bin/C4HdlC.dll"),
        QStringLiteral("c4hdl/win64-x64/genicam/bin/GenApi_MD_VC141_v3_2.dll"),
        QStringLiteral("c4hdl/win64-x64/genicam/bin/GCBase_MD_VC141_v3_2.dll"),
        QStringLiteral("c4hdl/win64-x64/genicam/bin/MathParser_MD_VC141_v3_2.dll"),
        QStringLiteral("c4hdl/win64-x64/genicam/bin/XmlParser_MD_VC141_v3_2.dll"),
        QStringLiteral("c4hdl/win64-x64/genicam/bin/Log_MD_VC141_v3_2.dll"),
        QStringLiteral("c4hdl/win64-x64/genicam/bin/log4cpp_MD_VC141_v3_2.dll"),
        QStringLiteral("c4hdl/win64-x64/genicam/bin/NodeMapData_MD_VC141_v3_2.dll"),
        QStringLiteral("diaphus/win64-x64/diaphus.cti"),
    };
    for (const QString& relativePath : requiredFiles) {
        if (!QFileInfo(runtimeRoot.filePath(relativePath)).isFile()) return false;
    }
    return true;
}

/**
 * Creates normalized process values for one validated C4Utility runtime.
 *
 * @param root Complete C4Utility runtime root.
 * @param source Diagnostic name for the selection source.
 * @return Normalized runtime root, vendor environment values, and source.
 */
HeliotisRuntimeSelection makeHeliotisRuntimeSelection(const QString& root, const QString& source)
{
    const QString normalizedRoot = QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(root).absoluteFilePath()));
    QString c4UtilityEnvironment = normalizedRoot;
    if (!c4UtilityEnvironment.endsWith(QDir::separator())) {
        c4UtilityEnvironment.append(QDir::separator());
    }
    const QString diaphusProducer = QDir::toNativeSeparators(
        QDir(normalizedRoot).filePath(QStringLiteral("diaphus/win64-x64/diaphus.cti")));
    return {normalizedRoot, c4UtilityEnvironment, diaphusProducer, source};
}

/**
 * Normalizes an existing runtime path for deterministic identity comparison.
 *
 * @param path Runtime root or file path to normalize.
 * @return Canonical path when available, otherwise an absolute clean path.
 */
QString comparableHeliotisRuntimePath(const QString& path)
{
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return QDir::toNativeSeparators(QDir::cleanPath(
        canonical.isEmpty() ? file.absoluteFilePath() : canonical));
}

/**
 * Selects the vendor runtime used for the complete Heliotis plugin lifetime.
 *
 * @return An explicit override or installed SDK when complete; otherwise the
 *         package-local runtime used on machines without C4Utility installed.
 * @throws std::runtime_error When no complete runtime candidate is available.
 * @note `HELIOTISC4_C4UTILITY_ROOT` is the explicit integration override. The
 *       ordinary vendor `C4UTILITY_ROOT`, standard Program Files roots, and
 *       the configured SDK precede the package fallback in that order.
 */
HeliotisRuntimeSelection selectHeliotisRuntime()
{
    const QString explicitRoot = qEnvironmentVariable("HELIOTISC4_C4UTILITY_ROOT").trimmed();
    if (!explicitRoot.isEmpty()) {
        if (!isCompleteHeliotisRuntime(explicitRoot)) {
            throw std::runtime_error(
                QStringLiteral("HELIOTISC4_C4UTILITY_ROOT does not contain a complete C4Utility runtime: %1")
                    .arg(explicitRoot).toStdString());
        }
        return makeHeliotisRuntimeSelection(explicitRoot, QStringLiteral("explicit-override"));
    }

    const QString packagedRuntimeRoot =
        qEnvironmentVariable("PLAYGROUND_HELIOTISC4_RUNTIME_ROOT").trimmed();
    const QString vendorEnvironmentRoot = qEnvironmentVariable("C4UTILITY_ROOT").trimmed();
    const bool inheritedPackageRoot = !vendorEnvironmentRoot.isEmpty()
        && !packagedRuntimeRoot.isEmpty()
        && comparableHeliotisRuntimePath(vendorEnvironmentRoot).compare(
            comparableHeliotisRuntimePath(packagedRuntimeRoot), Qt::CaseInsensitive) == 0;
    if (isCompleteHeliotisRuntime(vendorEnvironmentRoot) && !inheritedPackageRoot) {
        return makeHeliotisRuntimeSelection(vendorEnvironmentRoot, QStringLiteral("vendor-environment"));
    }
    if (inheritedPackageRoot) {
        qWarning().noquote() << "[Heliotis C4] Ignoring an inherited package C4UTILITY_ROOT;"
                             << "an installed or configured SDK remains preferred:"
                             << vendorEnvironmentRoot;
    } else if (!vendorEnvironmentRoot.isEmpty()) {
        qWarning().noquote() << "[Heliotis C4] Installed C4UTILITY_ROOT is incomplete;"
                             << "trying standard/configured installations before package fallback:"
                             << vendorEnvironmentRoot;
    }

    for (const char* environmentName : {"ProgramW6432", "ProgramFiles"}) {
        const QString programFilesRoot = qEnvironmentVariable(environmentName).trimmed();
        if (programFilesRoot.isEmpty()) continue;
        const QString installedRoot = QDir(programFilesRoot).filePath(QStringLiteral("C4Utility"));
        const bool isPackageRoot = !packagedRuntimeRoot.isEmpty()
            && comparableHeliotisRuntimePath(installedRoot).compare(
                comparableHeliotisRuntimePath(packagedRuntimeRoot), Qt::CaseInsensitive) == 0;
        if (isCompleteHeliotisRuntime(installedRoot) && !isPackageRoot) {
            return makeHeliotisRuntimeSelection(
                installedRoot, QStringLiteral("standard-installation"));
        }
    }

    const QString configuredSdkRoot = QString::fromUtf8(HELIOTISC4_SDK_ROOT);
    if (isCompleteHeliotisRuntime(configuredSdkRoot)) {
        return makeHeliotisRuntimeSelection(configuredSdkRoot, QStringLiteral("configured-sdk"));
    }

    if (isCompleteHeliotisRuntime(packagedRuntimeRoot)) {
        qWarning().noquote() << "[Heliotis C4] No complete installed C4Utility runtime is available;"
                             << "using the package fallback:" << packagedRuntimeRoot;
        return makeHeliotisRuntimeSelection(packagedRuntimeRoot, QStringLiteral("package-fallback"));
    }

    throw std::runtime_error(
        QStringLiteral("No complete C4Utility runtime is available. "
                       "HELIOTISC4_C4UTILITY_ROOT='%1', C4UTILITY_ROOT='%2', "
                       "configured SDK='%3', package fallback='%4'.")
            .arg(explicitRoot, vendorEnvironmentRoot, configuredSdkRoot, packagedRuntimeRoot)
            .toStdString());
}

/**
 * Resolves an already loaded Windows module for runtime provenance logging.
 *
 * @param moduleName Windows module basename, including its extension.
 * @return Absolute loaded path or a diagnostic placeholder.
 */
QString loadedHeliotisModulePath(const wchar_t* moduleName)
{
#if defined(Q_OS_WIN)
    const HMODULE module = GetModuleHandleW(moduleName);
    if (!module) return QStringLiteral("<not-loaded>");

    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) return QStringLiteral("<unavailable>");
    if (length >= path.size()) return QStringLiteral("<path-truncated>");
    path.resize(length);
    return QDir::toNativeSeparators(QString::fromStdWString(path));
#else
    Q_UNUSED(moduleName)
    return QStringLiteral("<unsupported-platform>");
#endif
}

#if defined(Q_OS_WIN)
/** Owns the selected C4Hdl module and its process DLL-search registrations. */
struct HeliotisRuntimeLibrary {
    /** Releases the explicit module reference and registered DLL directories. */
    ~HeliotisRuntimeLibrary()
    {
        if (module) FreeLibrary(module);
        for (auto it = directoryCookies.crbegin(); it != directoryCookies.crend(); ++it) {
            RemoveDllDirectory(*it);
        }
    }

    HMODULE module = nullptr;
    std::vector<DLL_DIRECTORY_COOKIE> directoryCookies;
};

/**
 * Loads C4Hdl from the already selected installed or fallback runtime root.
 *
 * @param runtime Validated runtime selection whose C4Hdl and GenApi folders
 *        must remain registered for the plugin lifetime.
 * @return Plugin-lifetime owner for the exact selected C4Hdl module.
 * @throws std::runtime_error When directory registration or exact loading fails.
 * @note The plugin target delay-loads C4HdlC so this function runs before the
 *       first imported SDK function can select a package DLL implicitly.
 */
std::unique_ptr<HeliotisRuntimeLibrary> loadSelectedHeliotisRuntimeLibrary(
    const HeliotisRuntimeSelection& runtime)
{
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "Could not configure the C4Utility DLL search policy");
    }

    auto library = std::make_unique<HeliotisRuntimeLibrary>();
    const QDir runtimeRoot(runtime.root);
    for (const QString& relativeDirectory : {
             QStringLiteral("c4hdl/win64-x64/c/bin"),
             QStringLiteral("c4hdl/win64-x64/genicam/bin")}) {
        const QString directory = QDir::toNativeSeparators(
            runtimeRoot.absoluteFilePath(relativeDirectory));
        const std::wstring nativeDirectory = directory.toStdWString();
        const DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(nativeDirectory.c_str());
        if (!cookie) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "Could not register a selected C4Utility DLL directory");
        }
        library->directoryCookies.push_back(cookie);
    }

    const QString expectedPath = QDir::toNativeSeparators(runtimeRoot.absoluteFilePath(
        QStringLiteral("c4hdl/win64-x64/c/bin/C4HdlC.dll")));
    const std::wstring nativePath = expectedPath.toStdWString();
    library->module = LoadLibraryExW(
        nativePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
            | LOAD_LIBRARY_SEARCH_USER_DIRS
            | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library->module) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "Could not load C4HdlC from the selected C4Utility runtime");
    }

    const QString loadedPath = loadedHeliotisModulePath(L"C4HdlC.dll");
    if (comparableHeliotisRuntimePath(loadedPath).compare(
            comparableHeliotisRuntimePath(expectedPath), Qt::CaseInsensitive) != 0) {
        throw std::runtime_error(
            QStringLiteral("C4HdlC was already loaded from a different runtime. Expected: %1, actual: %2")
                .arg(expectedPath, loadedPath).toStdString());
    }
    return library;
}
#endif

QVariantList toDiscoveryData(const std::vector<heliotis::DeviceDescriptor>& devices)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(devices.size()));
    for (const auto& device : devices) {
        result.append(QVariantMap{
            {QStringLiteral("interfaceIndex"), device.interfaceIndex},
            {QStringLiteral("deviceIndex"), device.deviceIndex},
            {QStringLiteral("interfaceName"), QString::fromStdString(device.interfaceName)},
            {QStringLiteral("deviceName"), QString::fromStdString(device.deviceName)},
        });
    }
    return result;
}

std::vector<heliotis::DeviceDescriptor> fromDiscoveryData(const QVariantList& values)
{
    std::vector<heliotis::DeviceDescriptor> devices;
    devices.reserve(static_cast<std::size_t>(values.size()));
    for (const QVariant& value : values) {
        const QVariantMap item = value.toMap();
        devices.push_back({
            item.value(QStringLiteral("interfaceIndex")).toLongLong(),
            item.value(QStringLiteral("deviceIndex")).toLongLong(),
            item.value(QStringLiteral("interfaceName")).toString().toStdString(),
            item.value(QStringLiteral("deviceName")).toString().toStdString(),
        });
    }
    return devices;
}

} // namespace

/** Owns one Heliotis device, controller, and asynchronous control-widget lifecycle. */
class HeliotisC4PluginSession final : public IDevicePluginSession {
public:
    /**
     * Creates a session around independently owned system and device facades.
     *
     * @param system Shared discovery/runtime owner for the plugin instance.
     * @param device Device facade whose SDK lifetime is owned by this session.
     * @param devices Copied discovery snapshot exposed by the control widget.
     */
    HeliotisC4PluginSession(
        std::shared_ptr<heliotis::HeliotisC4System> system,
        std::unique_ptr<heliotis::HeliotisC4Device> device,
        std::vector<heliotis::DeviceDescriptor> devices)
        : _system(std::move(system)),
          _device(std::move(device)),
          _devices(std::move(devices)),
          _controller(std::make_unique<HeliotisC4SourceController>(_device.get()))
    {}

    ~HeliotisC4PluginSession() override
    {
        if (_widget) {
            QObject::disconnect(_widget, nullptr, _widget, nullptr);
            if (_discoveryWatcher) QObject::disconnect(_discoveryWatcher.get(), nullptr, _widget, nullptr);
            if (_connectionWatcher) QObject::disconnect(_connectionWatcher.get(), nullptr, _widget, nullptr);
            if (_disconnectionWatcher) QObject::disconnect(_disconnectionWatcher.get(), nullptr, _widget, nullptr);
            if (_initializationWatcher) QObject::disconnect(_initializationWatcher.get(), nullptr, _widget, nullptr);
            if (_featureWatcher) QObject::disconnect(_featureWatcher.get(), nullptr, _widget, nullptr);
            if (_featureReadWatcher) QObject::disconnect(_featureReadWatcher.get(), nullptr, _widget, nullptr);
            if (_armWatcher) QObject::disconnect(_armWatcher.get(), nullptr, _widget, nullptr);
        }
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

    QString title() const override { return _title; }

    std::vector<DevicePluginDock> createDockWidgets(QWidget* parent) override
    {
        if (_widget) {
            return {{QStringLiteral("device-controls"), QStringLiteral("Device Controls"),
                     Qt::LeftDockWidgetArea, _widget, true}};
        }

        _widget = new QHeliotisC4Widget(parent);
        _widget->setDiscoveredDevices(_devices);
        QObject::connect(_widget, &QHeliotisC4Widget::refreshRequested, _widget, [this]() {
            refreshDevices();
        });
        QObject::connect(_widget, &QHeliotisC4Widget::connectRequested, _widget, [this](const int index) {
            if (_discoveryWatcher || _connectionWatcher || _disconnectionWatcher || _initializationWatcher
                || _featureWatcher || _featureReadWatcher || _armWatcher || _disconnectPending) {
                qWarning().noquote() << "[Heliotis C4] Connection ignored because another SDK operation is pending.";
                _widget->setConnectionError(QObject::tr("Wait for the current Heliotis operation before connecting."));
                return;
            }
            if (_device && _device->isOpened()) {
                qWarning().noquote() << "[Heliotis C4] Connection ignored because the session already owns an open device.";
                _widget->setConnectionState(true);
                _widget->setAcquisitionAvailable(!_device->requiresReconnect());
                return;
            }
            if (index < 0 || index >= static_cast<int>(_devices.size())) {
                _widget->setConnectionError(QObject::tr("The selected Heliotis device is no longer available."));
                return;
            }

            const heliotis::DeviceDescriptor descriptor = _devices[static_cast<std::size_t>(index)];
            qInfo().noquote() << "[Heliotis C4] Connecting to" << QString::fromStdString(descriptor.deviceName)
                              << "interface" << descriptor.interfaceIndex << "device" << descriptor.deviceIndex;
            _connectionWatcher = std::make_unique<QFutureWatcher<HeliotisConnectionResult>>();
            auto* watcher = _connectionWatcher.get();
            QObject::connect(watcher, &QFutureWatcher<HeliotisConnectionResult>::finished, _widget, [this, watcher]() {
                if (!_connectionWatcher || _connectionWatcher.get() != watcher) return;
                const HeliotisConnectionResult result = watcher->result();
                _connectionWatcher.reset();
                if (!_widget) return;
                _widget->setConnectionPending(false);
                qInfo().noquote() << "[Heliotis C4] Asynchronous device connection finished:"
                                  << "opened=" << result.opened
                                  << "openElapsedMs=" << result.openElapsedMs;
                if (!result.opened) {
                    qWarning().noquote() << "[Heliotis C4] Device connection failed:" << result.openError;
                    _widget->setConnectionError(result.openError);
                    return;
                }
                const QString deviceName = QString::fromStdString(_device->connectedDeviceName());
                if (!deviceName.isEmpty()) {
                    _title = QStringLiteral("Heliotis C4 - %1").arg(deviceName);
                    if (_titleChanged) _titleChanged(_title);
                }
                _widget->setConnectedDeviceName(deviceName);
                _widget->setConnectionState(true);
                _widget->setAcquisitionAvailable(true);
                refreshFeatureTree([this](const bool featuresRefreshed) {
                    if (!_widget) return;
                    _widget->setAcquisitionAvailable(true);
                    qInfo().noquote() << "[Heliotis C4] Device connection completed."
                                      << "The existing device configuration was preserved;"
                                      << "Init remains the explicit default-profile and StageInit action."
                                      << "featureRefresh=" << featuresRefreshed;
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
        });
        QObject::connect(_widget, &QHeliotisC4Widget::disconnectRequested, _widget, [this]() {
            qInfo().noquote() << "[Heliotis C4] Disconnect requested.";
            if (_disconnectPending || _discoveryWatcher || _connectionWatcher || _disconnectionWatcher
                || _initializationWatcher
                || _featureWatcher || _featureReadWatcher || _armWatcher) {
                qWarning().noquote() << "[Heliotis C4] Disconnect ignored while another SDK operation is pending.";
                _widget->setConnectionState(_device && _device->isOpened());
                return;
            }
            if (_controller && _controller->isGrabbing()) {
                _disconnectPending = true;
                _widget->setDisconnectionPending();
                _controller->requestStop();
                return;
            }
            completeDisconnect();
        });
        QObject::connect(_widget, &QHeliotisC4Widget::grabOneRequested, _widget, [this]() {
            requestAcquisitionArm(false);
        });
        QObject::connect(_widget, &QHeliotisC4Widget::initializationRequested, _widget, [this]() {
            initializeH8ReferenceProfile();
        });
        QObject::connect(_widget, &QHeliotisC4Widget::stopRequested, _widget, [this]() {
            if (_widget) _widget->setAcquisitionStopPending();
            _controller->requestStop();
        });
        QObject::connect(_widget, &QHeliotisC4Widget::liveGrabToggled, _widget, [this](const bool enabled) {
            if (enabled) requestAcquisitionArm(true);
            else {
                if (_widget) _widget->setAcquisitionStopPending();
                _controller->requestStop();
            }
        });
        QObject::connect(_widget, &QHeliotisC4Widget::featureWriteRequested, _widget,
            [this](const QString& name, const QString& value) {
                runFeatureOperation([name, value](heliotis::HeliotisC4Device* device, std::string* error) {
                    return device->writeFeature(name.toStdString(), value.toStdString(), error);
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
        QObject::connect(_controller.get(), &HeliotisC4SourceController::acquisitionStateChanged, _widget,
            [this](const bool acquiring, const bool continuous, const bool softwareTriggerAvailable) {
                if (!acquiring && _disconnectPending) {
                    completeDisconnect();
                    return;
                }
                if (_widget) _widget->setAcquisitionState(acquiring, continuous, softwareTriggerAvailable);
            });
        QObject::connect(_controller.get(), &HeliotisC4SourceController::acquisitionFrameReceived, _widget,
            [this]() {
                if (_widget) _widget->setSoftwareTriggerPending(false);
            });
        QObject::connect(_controller.get(), &HeliotisC4SourceController::acquisitionError, _widget,
            [this](const QString& message) {
                if (!_widget) return;
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
        return {{QStringLiteral("device-controls"), QStringLiteral("Device Controls"),
                 Qt::LeftDockWidgetArea, _widget, true}};
    }

    AbstractSourceController* sourceController() const override { return _controller.get(); }
    unsigned int capabilities() const noexcept override
    {
        return DevicePluginSessionCapability::GraphicsEngine
            | DevicePluginSessionCapability::ScriptEditor;
    }
    void setTitleChangedCallback(std::function<void(const QString&)> callback) override { _titleChanged = std::move(callback); }

private:
    using FeatureOperation = std::function<bool(heliotis::HeliotisC4Device*, std::string*)>;

    /** Discovers interfaces and devices without blocking the Qt event thread. */
    void refreshDevices()
    {
        if (!_widget || !_system || _discoveryWatcher || _connectionWatcher || _disconnectionWatcher
            || _initializationWatcher || _featureWatcher || _featureReadWatcher || _armWatcher
            || _disconnectPending || (_device && _device->isOpened())
            || (_controller && _controller->isGrabbing())) {
            qWarning().noquote() << "[Heliotis C4] Device discovery ignored because another SDK operation is pending.";
            return;
        }

        qInfo().noquote() << "[Heliotis C4] Asynchronous device discovery requested.";
        _widget->setDiscoveryPending(true);
        _discoveryWatcher = std::make_unique<QFutureWatcher<HeliotisDiscoveryResult>>();
        auto* watcher = _discoveryWatcher.get();
        QObject::connect(watcher, &QFutureWatcher<HeliotisDiscoveryResult>::finished, _widget,
            [this, watcher]() {
            if (!_discoveryWatcher || _discoveryWatcher.get() != watcher) return;
            const HeliotisDiscoveryResult result = watcher->result();
            _discoveryWatcher.reset();
            if (!_widget) return;
            _widget->setDiscoveryPending(false);
            if (!result.error.isEmpty()) {
                qWarning().noquote() << "[Heliotis C4] Device discovery failed after"
                                     << result.elapsedMs << "ms:" << result.error;
                _widget->setDiscoveryError(result.error);
                return;
            }
            _devices = result.devices;
            _widget->setDiscoveredDevices(_devices);
            qInfo().noquote() << "[Heliotis C4] Device discovery completed with"
                              << _devices.size() << "device(s) in" << result.elapsedMs << "ms.";
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
    }

    /**
     * Completes device shutdown away from the Qt event thread.
     *
     * @note Interactive callers first use the non-blocking stop path; this
     *       method schedules final joined teardown and handle release.
     */
    void completeDisconnect()
    {
        if (!_widget || !_device || _disconnectionWatcher) return;
        _disconnectPending = true;
        _widget->setDisconnectionPending();
        qInfo().noquote() << "[Heliotis C4] Asynchronous device disconnect started.";

        _disconnectionWatcher = std::make_unique<QFutureWatcher<HeliotisDisconnectionResult>>();
        auto* watcher = _disconnectionWatcher.get();
        QObject::connect(watcher, &QFutureWatcher<HeliotisDisconnectionResult>::finished, _widget,
            [this, watcher]() {
            if (!_disconnectionWatcher || _disconnectionWatcher.get() != watcher) return;
            const HeliotisDisconnectionResult result = watcher->result();
            _disconnectionWatcher.reset();
            _disconnectPending = false;
            if (!_widget) return;
            _widget->setFeatures({});
            _widget->setConnectionState(false);
            qInfo().noquote() << "[Heliotis C4] Asynchronous device disconnect completed:"
                              << "elapsedMs=" << result.elapsedMs;
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

    /**
     * Arms acquisition on a background task so SDK setup cannot block Qt.
     *
     * @param continuous True for host-side Live; false for host-side Single.
     */
    void requestAcquisitionArm(const bool continuous)
    {
        if (!_widget || !_controller || !_device || !_device->isOpened()
            || _device->requiresReconnect() || _controller->isGrabbing()
            || _armWatcher || _discoveryWatcher
            || _connectionWatcher || _disconnectionWatcher || _initializationWatcher
            || _featureWatcher || _featureReadWatcher || _disconnectPending) {
            qWarning().noquote() << "[Heliotis C4] Acquisition arm ignored because the device is unavailable, active, or busy.";
            return;
        }

        qInfo().noquote() << "[Heliotis C4] Asynchronous acquisition arm requested:"
                          << (continuous ? "Live" : "Single");
        _widget->setAcquisitionArmPending(true, continuous);
        _armWatcher = std::make_unique<QFutureWatcher<HeliotisAcquisitionArmResult>>();
        auto* watcher = _armWatcher.get();
        QObject::connect(watcher, &QFutureWatcher<HeliotisAcquisitionArmResult>::finished, _widget,
            [this, watcher]() {
            if (!_armWatcher || _armWatcher.get() != watcher) return;
            const HeliotisAcquisitionArmResult result = watcher->result();
            _armWatcher.reset();
            if (!_widget) return;
            _widget->setAcquisitionArmPending(false, result.continuous);
            if (!result.taskError.isEmpty()) {
                qWarning().noquote() << "[Heliotis C4] Acquisition arm task raised an exception:"
                                     << result.taskError;
                _widget->setAcquisitionError(result.taskError);
            }
            qInfo().noquote() << "[Heliotis C4] Asynchronous acquisition arm finished:"
                              << "mode=" << (result.continuous ? "Live" : "Single")
                              << "started=" << result.started
                              << "controllerActive=" << (_controller && _controller->isGrabbing())
                              << "deviceActive=" << (_device && _device->isAcquiring())
                              << "elapsedMs=" << result.elapsedMs;
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
    }

    /**
     * Applies the explicit default H8 surface profile requested by Init.
     *
     * @note The profile resets capture, trigger, encoder, motion, processing,
     *       and illumination controls to the C4Utility h8SurfSimple defaults.
     *       StageInit may move the stage. Failure leaves the connection and
     *       capture controls available for inspection or retry.
     */
    void initializeH8ReferenceProfile()
    {
        if (!_widget || !_device || !_device->isOpened()
            || (_controller && _controller->isGrabbing()) || _disconnectPending
            || _discoveryWatcher || _connectionWatcher || _disconnectionWatcher
            || _initializationWatcher
            || _featureWatcher || _featureReadWatcher || _armWatcher) {
            qWarning().noquote() << "[Heliotis C4] H8 reference-profile initialization ignored because the device is unavailable, active, or busy.";
            return;
        }

        qInfo().noquote() << "[Heliotis C4] Explicit H8 default-profile initialization requested."
                          << "Applying all C4Utility h8SurfSimple component, chunk, trigger, encoder, motion, processing, and illumination defaults."
                          << "StageInit follows the verified encoder and motion defaults and may move the stage."
                          << "Optional geometry or illumination capability failures are warning-only.";
        _widget->setInitializationPending(true);
        _initializationWatcher = std::make_unique<QFutureWatcher<HeliotisInitializationResult>>();
        auto* watcher = _initializationWatcher.get();
        QObject::connect(watcher, &QFutureWatcher<HeliotisInitializationResult>::finished, _widget,
            [this, watcher]() {
            if (!_initializationWatcher || _initializationWatcher.get() != watcher) return;
            const HeliotisInitializationResult result = watcher->result();
            _initializationWatcher.reset();
            if (!_widget) return;

            refreshFeatureTree([this, result](const bool featuresRefreshed) {
                if (!_widget) return;
                if (!result.initialized) {
                    qWarning().noquote() << "[Heliotis C4] Explicit H8 default-profile initialization failed; connection and capture controls are preserved:"
                                         << result.error << "elapsedMs=" << result.elapsedMs;
                    _widget->setInitializationError(result.error);
                    return;
                }

                _widget->setInitializationPending(false);
                _widget->setAcquisitionAvailable(true);
                qInfo().noquote() << "[Heliotis C4] Explicit H8 default-profile initialization completed; acquisition remains available."
                                  << "featureRefresh=" << featuresRefreshed
                                  << "elapsedMs=" << result.elapsedMs;
            });
        });
        watcher->setFuture(QtConcurrent::run([device = _device.get()]() {
            const auto started = std::chrono::steady_clock::now();
            std::string error;
            bool initialized = false;
            try {
                initialized = device && device->configureH8SurfaceExample(&error);
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
    }

    /**
     * Runs one serialized feature mutation away from the Qt event thread.
     *
     * @param operation Device mutation to execute.
     * @param refreshFeaturesAfterSuccess Whether to reread feature metadata afterward.
     * @param completion Optional callback receiving the final operation result.
     * @return True when the asynchronous operation was scheduled.
     */
    bool runFeatureOperation(
        FeatureOperation operation,
        const bool refreshFeaturesAfterSuccess = true,
        std::function<void(bool)> completion = {})
    {
        if (!_widget || !_device || !_device->isOpened()
            || _featureWatcher || _discoveryWatcher || _initializationWatcher
            || _connectionWatcher || _disconnectionWatcher || _featureReadWatcher || _armWatcher
            || _disconnectPending || _device->requiresReconnect()) {
            qWarning().noquote() << "[Heliotis C4] Feature operation ignored because the device is unavailable or busy.";
            if (completion) completion(false);
            return false;
        }

        _widget->setFeatureOperationPending(true);
        _featureWatcher = std::make_unique<QFutureWatcher<HeliotisFeatureOperationResult>>();
        auto* watcher = _featureWatcher.get();
        QObject::connect(watcher, &QFutureWatcher<HeliotisFeatureOperationResult>::finished, _widget,
            [this, watcher, refreshFeaturesAfterSuccess, completion = std::move(completion)]() mutable {
            if (!_featureWatcher || _featureWatcher.get() != watcher) return;
            const HeliotisFeatureOperationResult result = watcher->result();
            _featureWatcher.reset();
            if (!_widget) return;
            qInfo().noquote() << "[Heliotis C4] Asynchronous feature operation finished:"
                              << "succeeded=" << result.succeeded
                              << "elapsedMs=" << result.elapsedMs;
            if (!result.succeeded) {
                _widget->setFeatureOperationPending(false);
                qWarning().noquote() << "[Heliotis C4] Feature operation failed:" << result.error;
                if (completion) completion(false);
                _widget->setFeatureError(result.error);
                return;
            }
            if (refreshFeaturesAfterSuccess) {
                refreshFeatureTree([this, completion = std::move(completion)](const bool refreshed) mutable {
                    if (_widget) _widget->setFeatureOperationPending(false);
                    if (completion) completion(refreshed);
                });
                return;
            }
            _widget->setFeatureOperationPending(false);
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

    /**
     * Reads the SDK feature tree without blocking the Qt event thread.
     *
     * @param completion Optional callback receiving whether the refresh succeeded.
     * @note Refresh is rejected while acquisition is active because C4 feature
     *       reads share the serialized SDK channel with buffer polling.
     */
    void refreshFeatureTree(std::function<void(bool)> completion = {})
    {
        if (!_widget || !_device || !_device->isOpened() || _discoveryWatcher
            || _connectionWatcher || _disconnectionWatcher || _initializationWatcher
            || _featureWatcher || _disconnectPending || _device->requiresReconnect()) {
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

        _widget->setFeatureRefreshPending(true);
        _featureRefreshCompletion = std::move(completion);
        _featureReadWatcher = std::make_unique<QFutureWatcher<HeliotisFeatureReadResult>>();
        auto* watcher = _featureReadWatcher.get();
        QObject::connect(watcher, &QFutureWatcher<HeliotisFeatureReadResult>::finished, _widget,
            [this, watcher]() {
            if (!_featureReadWatcher || _featureReadWatcher.get() != watcher) return;
            const HeliotisFeatureReadResult result = watcher->result();
            _featureReadWatcher.reset();
            auto completion = std::move(_featureRefreshCompletion);
            _featureRefreshCompletion = {};
            if (!_widget) return;

            _widget->setFeatureRefreshPending(false);
            const bool succeeded = result.error.isEmpty();
            if (!succeeded) {
                qWarning().noquote() << "[Heliotis C4] Feature read failed after"
                                     << result.elapsedMs << "ms:" << result.error;
                _widget->setFeatureError(result.error);
            } else {
                _widget->setFeatures(result.features);
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

    std::shared_ptr<heliotis::HeliotisC4System> _system;
    std::unique_ptr<heliotis::HeliotisC4Device> _device;
    std::vector<heliotis::DeviceDescriptor> _devices;
    std::unique_ptr<HeliotisC4SourceController> _controller;
    QPointer<QHeliotisC4Widget> _widget;
    std::unique_ptr<QFutureWatcher<HeliotisDiscoveryResult>> _discoveryWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisConnectionResult>> _connectionWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisDisconnectionResult>> _disconnectionWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisInitializationResult>> _initializationWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisFeatureOperationResult>> _featureWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisFeatureReadResult>> _featureReadWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisAcquisitionArmResult>> _armWatcher;
    std::function<void(bool)> _featureRefreshCompletion;
    bool _disconnectPending = false;
    QString _title = QStringLiteral("Heliotis C4 Session");
    std::function<void(const QString&)> _titleChanged;
};

/** Owns Heliotis discovery, sessions, and one coherently selected SDK runtime. */
class HeliotisC4Plugin final : public DevicePluginTemplate {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PlaygroundDevicePlugin_iid)
    Q_INTERFACES(IDevicePlugin)

public:
    DevicePluginDescriptor descriptor() const override
    {
        return {QStringLiteral("heliotis-c4"), QStringLiteral("Heliotis C4"), playgroundDevicePluginVersion(), true};
    }

    /** Releases C4 handles before the selected runtime module and search paths. */
    void shutdown() override
    {
        std::lock_guard<std::mutex> lock(_systemMutex);
        _system.reset();
#if defined(Q_OS_WIN)
        _runtimeLibrary.reset();
#endif
    }

    bool discoverDevices(QVariantMap* discoveryData, QString* errorMessage) override
    {
        try {
            const auto system = c4System();
            std::string discoveryError;
            const auto devices = system->discoverDevices(&discoveryError);
            if (!discoveryError.empty()) {
                if (errorMessage) *errorMessage = QString::fromStdString(discoveryError);
                return false;
            }
            if (discoveryData) discoveryData->insert(QStringLiteral("devices"), toDiscoveryData(devices));
            return true;
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return false;
        }
    }

    std::unique_ptr<IDevicePluginSession> createSession(
        const QVariantMap& discoveryData, QString* errorMessage) override
    {
        try {
            const auto system = c4System();
            auto device = system->createDevice();
            return std::make_unique<HeliotisC4PluginSession>(
                system, std::move(device),
                fromDiscoveryData(discoveryData.value(QStringLiteral("devices")).toList()));
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return {};
        }
    }

private:
    /**
     * Returns the shared C4Utility system after selecting and pinning one runtime.
     *
     * @return Shared plugin-lifetime C4Utility system.
     * @throws std::runtime_error When runtime selection, environment setup, or
     *         C4Hdl initialization fails.
     * @note The mutex serializes discovery and session creation around the one
     *       process-wide vendor runtime selection.
     */
    std::shared_ptr<heliotis::HeliotisC4System> c4System()
    {
        std::lock_guard<std::mutex> lock(_systemMutex);
        if (!_system) {
            const HeliotisRuntimeSelection runtime = selectHeliotisRuntime();
            if (!qputenv("C4UTILITY_ROOT", runtime.c4UtilityEnvironment.toUtf8())
                || !qputenv("DIAPHUS_GENTL64_FILE", runtime.diaphusProducer.toUtf8())) {
                throw std::runtime_error("Could not configure the selected C4Utility process environment.");
            }

#if defined(Q_OS_WIN)
            auto runtimeLibrary = loadSelectedHeliotisRuntimeLibrary(runtime);
#endif
            qInfo().noquote() << "[Heliotis C4] C4Utility runtime selected before C4Hdl_open:"
                              << "source=" << runtime.source
                              << "root=" << runtime.root
                              << "packageFallback="
                              << (runtime.source == QStringLiteral("package-fallback"))
                              << "C4UTILITY_ROOT=" << qgetenv("C4UTILITY_ROOT")
                              << "DIAPHUS_GENTL64_FILE=" << qgetenv("DIAPHUS_GENTL64_FILE")
                              << "loadedC4HdlC="
                              << loadedHeliotisModulePath(L"C4HdlC.dll");

            const QByteArray runtimeRoot = runtime.root.toUtf8();
            auto system = std::make_shared<heliotis::HeliotisC4System>(runtimeRoot.constData());
#if defined(Q_OS_WIN)
            _runtimeLibrary = std::move(runtimeLibrary);
#endif
            _system = std::move(system);
        }
        return _system;
    }

    std::mutex _systemMutex;
#if defined(Q_OS_WIN)
    std::unique_ptr<HeliotisRuntimeLibrary> _runtimeLibrary;
#endif
    std::shared_ptr<heliotis::HeliotisC4System> _system;
};

#include "HeliotisC4Plugin.moc"
