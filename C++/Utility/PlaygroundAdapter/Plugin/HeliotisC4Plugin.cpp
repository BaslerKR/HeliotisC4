#include "HeliotisC4PluginSession.h"
#include "HeliotisC4System.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
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

} // namespace

/** Owns Heliotis discovery, sessions, and one coherently selected SDK runtime. */
class HeliotisC4Plugin final : public QObject, public IDeviceRuntime, public IDevicePluginSettings {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PlaygroundDeviceRuntime_iid)
    Q_INTERFACES(IDeviceRuntime IDevicePluginSettings)

public:
    /** Selects the host-owned INI used for feature favorites; other keys are preserved. */
    void setSettingsFile(const QString& fileName) override { _settingsFile = fileName; }

    DevicePluginDescriptor descriptor() const override
    {
        return {QStringLiteral("heliotis-c4"), playgroundDevicePluginDisplayName(), playgroundDevicePluginVersion(), true};
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

    bool discoverDevices(QString* errorMessage) override
    {
        try {
            const auto system = c4System();
            std::string discoveryError;
            const auto devices = system->discoverDevices(&discoveryError);
            if (!discoveryError.empty()) {
                if (errorMessage) *errorMessage = QString::fromStdString(discoveryError);
                return false;
            }
            // The package retains its discovery result; the Host receives only the
            // typed target inventory through the session.
            {
                std::lock_guard<std::mutex> lock(_discoveryMutex);
                _discoveredDevices = devices;
            }
            return true;
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return false;
        }
    }

    std::unique_ptr<IDevicePluginSession> createSession(
        const QVariantMap& sessionOptions, QString* errorMessage) override
    {
        Q_UNUSED(sessionOptions)
        try {
            const auto system = c4System();
            auto device = system->createDevice();
            std::vector<heliotis::DeviceDescriptor> devices;
            {
                std::lock_guard<std::mutex> lock(_discoveryMutex);
                devices = _discoveredDevices;
            }
            return std::make_unique<HeliotisC4PluginSession>(
                system, std::move(device), std::move(devices), _settingsFile);
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
    // Discovery runs on a host worker thread; session creation runs on the GUI
    // thread, so the retained snapshot needs its own guard.
    std::mutex _discoveryMutex;
    std::vector<heliotis::DeviceDescriptor> _discoveredDevices;
#if defined(Q_OS_WIN)
    std::unique_ptr<HeliotisRuntimeLibrary> _runtimeLibrary;
#endif
    std::shared_ptr<heliotis::HeliotisC4System> _system;
    QString _settingsFile;
};

#include "HeliotisC4Plugin.moc"
