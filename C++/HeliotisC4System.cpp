#include "HeliotisC4System.h"
#include "Internal/C4UtilitySdkRuntime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace heliotis {

namespace {

std::string lastSdkError()
{
    const char* error = C4Hdl_getLastError();
    return error ? std::string(error) : std::string("Unknown C4Utility error.");
}

bool check(const C4HDL_ERROR result, std::string* errorMessage)
{
    if (result == C4HDL_ERR_SUCCESS) return true;
    if (errorMessage) *errorMessage = lastSdkError();
    return false;
}

std::mutex& diagnosticLogMutex()
{
    static std::mutex mutex;
    return mutex;
}

void logInfo(const std::string& message)
{
    std::lock_guard<std::mutex> lock(diagnosticLogMutex());
    std::cout << "[Heliotis C4] " << message << std::endl;
}

void logWarning(const std::string& message)
{
    std::lock_guard<std::mutex> lock(diagnosticLogMutex());
    std::cerr << "[Heliotis C4] " << message << std::endl;
}

template <typename Reader>
std::string readSdkString(Reader&& reader, std::string* errorMessage)
{
    std::size_t size = 256;
    for (int attempt = 0; attempt != 2; ++attempt) {
        std::vector<char> buffer(size, '\0');
        const C4HDL_ERROR result = reader(buffer.data(), &size);
        if (result == C4HDL_ERR_SUCCESS) return std::string(buffer.data());
        if (result != C4HDL_ERR_SMALL_BUFFER) {
            check(result, errorMessage);
            return {};
        }
        size = std::max<std::size_t>(size + 1, 512);
    }
    if (errorMessage) *errorMessage = "C4Utility returned an invalid string buffer size.";
    return {};
}

FeatureType featureType(const Type_e value)
{
    switch (value) {
    case C4FTR_TYPE_INTEGER: return FeatureType::Integer;
    case C4FTR_TYPE_FLOAT: return FeatureType::Float;
    case C4FTR_TYPE_STRING: return FeatureType::String;
    case C4FTR_TYPE_ENUMERATION: return FeatureType::Enumeration;
    case C4FTR_TYPE_COMMAND: return FeatureType::Command;
    case C4FTR_TYPE_BOOLEAN: return FeatureType::Boolean;
    default: return FeatureType::Unknown;
    }
}

FeatureAccess featureAccess(const AccessMode_e value)
{
    switch (value) {
    case C4FTR_ACCESSMODE_NI: return FeatureAccess::NotImplemented;
    case C4FTR_ACCESSMODE_NA: return FeatureAccess::NotAvailable;
    case C4FTR_ACCESSMODE_RO: return FeatureAccess::ReadOnly;
    case C4FTR_ACCESSMODE_WO: return FeatureAccess::WriteOnly;
    case C4FTR_ACCESSMODE_RW: return FeatureAccess::ReadWrite;
    default: return FeatureAccess::Unknown;
    }
}

bool isMotionFeature(const std::string& category, const std::string& name)
{
    std::string value = category + "/" + name;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value.find("stage") != std::string::npos
        || value.find("scan") != std::string::npos
        || value.find("motion") != std::string::npos
        || value.find("encoder") != std::string::npos;
}

std::string lowerCase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](const unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string();
}

std::vector<std::string> splitEnumEntries(const std::string& entries)
{
    std::vector<std::string> result;
    std::istringstream stream(entries);
    for (std::string entry; std::getline(stream, entry, ';');) {
        entry = trim(std::move(entry));
        if (!entry.empty()) result.push_back(std::move(entry));
    }
    return result;
}

bool isWritable(const FeatureAccess access)
{
    return access == FeatureAccess::ReadWrite || access == FeatureAccess::WriteOnly;
}

bool findFeatureMetadata(
    const C4_DEVICE device,
    const std::string& requestedName,
    FeatureType* type,
    FeatureAccess* access,
    std::string* errorMessage)
{
    C4_FEATUREINFO* featureList = nullptr;
    if (!check(C4Dev_getFeatureList(device, &featureList), errorMessage)) return false;
    if (!featureList) {
        if (errorMessage) *errorMessage = "C4Utility returned an empty feature list.";
        return false;
    }

    bool found = false;
    constexpr std::size_t maximumFeatureCount = 4096;
    for (std::size_t index = 0; index < maximumFeatureCount && featureList[index] != nullptr; ++index) {
        const C4_FEATUREINFO feature = featureList[index];
        std::string metadataError;
        const std::string name = readSdkString(
            [feature](char* buffer, std::size_t* size) { return C4Ftr_getName(feature, buffer, size); },
            &metadataError);
        if (!metadataError.empty() || name != requestedName) continue;

        Type_e sdkType = C4FTR_TYPE_UNKNOWN;
        AccessMode_e sdkAccess = C4FTR_ACCESSMODE_UNKNOWN;
        if (C4Ftr_getType(feature, &sdkType) != C4HDL_ERR_SUCCESS
            || C4Ftr_getAccessMode(feature, &sdkAccess) != C4HDL_ERR_SUCCESS) {
            if (errorMessage) *errorMessage = lastSdkError();
            break;
        }
        if (type) *type = featureType(sdkType);
        if (access) *access = featureAccess(sdkAccess);
        found = true;
        break;
    }
    C4Ftr_release(featureList);
    if (!found && errorMessage && errorMessage->empty()) {
        *errorMessage = "Heliotis feature is not available: " + requestedName;
    }
    return found;
}

std::string readDeviceEnumeration(const C4_DEVICE device, const char* name)
{
    std::string error;
    const std::string value = readSdkString(
        [device, name](char* buffer, std::size_t* size) {
            return C4Dev_readEnumeration(device, name, buffer, size);
        },
        &error);
    return error.empty() ? value : std::string("<unavailable: ") + error + ">";
}

std::string triggerConfigurationSummary(const C4_DEVICE device)
{
    const std::string selector = readDeviceEnumeration(device, "TriggerSelector");
    const std::string mode = readDeviceEnumeration(device, "TriggerMode");
    const std::string source = readDeviceEnumeration(device, "TriggerSource");
    return "TriggerSelector=" + selector + ", TriggerMode=" + mode + ", TriggerSource=" + source;
}

FramePartKind framePartKind(const std::string& partType)
{
    const std::string value = lowerCase(partType);
    if (value.find("surface") != std::string::npos || value.find("range") != std::string::npos) {
        return FramePartKind::Range;
    }
    if (value.find("amplitude") != std::string::npos
        || value.find("intensity") != std::string::npos
        || value.find("reflectance") != std::string::npos) {
        return FramePartKind::Intensity;
    }
    if (value.find("confidence") != std::string::npos) {
        return FramePartKind::Confidence;
    }
    return FramePartKind::Unknown;
}

bool usesFloatingPointSamples(const std::string& pixelFormat)
{
    const std::string value = lowerCase(pixelFormat);
    return value.find("float") != std::string::npos
        || value.find("32f") != std::string::npos
        || value.find("64f") != std::string::npos;
}

bool isLikelyTimeoutError(
    const std::string& message,
    const std::chrono::milliseconds elapsed,
    const std::chrono::milliseconds requestedTimeout)
{
    const std::string value = lowerCase(message);
    if (value.find("timeout") != std::string::npos
        || value.find("timed out") != std::string::npos) {
        return true;
    }

    // The C API exposes only a generic error code for getBuffer().  Treat a
    // generic error that consumed the requested wait as a timeout so an armed
    // external/stage trigger remains armed instead of ending acquisition.
    const bool genericError = value.empty()
        || value == "unknown c4utility error."
        || value == "error";
    constexpr auto tolerance = std::chrono::milliseconds(25);
    return genericError && elapsed + tolerance >= requestedTimeout;
}

bool validateChunkMetadataConfiguration(const C4_DEVICE device, std::string* errorMessage)
{
    std::int64_t chunkModeActive = 0;
    const C4HDL_ERROR result = C4Dev_readInteger(device, "ChunkModeActive", &chunkModeActive);
    logInfo("ChunkModeActive=" + std::to_string(chunkModeActive) + ".");
    if (result != C4HDL_ERR_SUCCESS || chunkModeActive != 1) {
        if (errorMessage) {
            *errorMessage = "Heliotis acquisition requires the existing device configuration to enable "
                "ChunkModeActive, ChunkPartCount, ChunkPartType, ChunkScan3dDistanceUnit, "
                "ChunkScan3dOutputMode, ChunkScan3dCoordinateScale, and ChunkScan3dCoordinateOffset. "
                "The plugin does not change device features."
                + (result == C4HDL_ERR_SUCCESS ? std::string() : " C4Utility: " + lastSdkError());
        }
        return false;
    }
    return true;
}

bool readBufferString(const C4_BUFFER buffer, const char* name, std::string* value, std::string* errorMessage)
{
    std::string readError;
    const std::string result = readSdkString(
        [buffer, name](char* text, std::size_t* size) {
            return C4Buf_readString(buffer, name, text, size);
        },
        &readError);
    if (!readError.empty() || result.empty()) {
        if (errorMessage) *errorMessage = "C4Utility buffer is missing " + std::string(name) + " metadata.";
        return false;
    }
    *value = result;
    return true;
}

bool readScan3dGeometry(const C4_BUFFER buffer, Scan3dGeometry* geometry, std::string* errorMessage)
{
    if (!buffer || !geometry) return false;

    Scan3dGeometry copied;
    if (!readBufferString(buffer, "ChunkScan3dDistanceUnit", &copied.distanceUnit, errorMessage)
        || !readBufferString(buffer, "ChunkScan3dOutputMode", &copied.outputMode, errorMessage)) {
        return false;
    }

    struct AxisValues {
        const char* selector;
        double* scale;
        double* offset;
    };
    const auto readAxis = [buffer, errorMessage](const AxisValues& axis) {
        if (C4Buf_writeString(buffer, "ChunkScan3dCoordinateSelector", axis.selector) != C4HDL_ERR_SUCCESS
            || C4Buf_readFloat(buffer, "ChunkScan3dCoordinateScale", axis.scale) != C4HDL_ERR_SUCCESS
            || C4Buf_readFloat(buffer, "ChunkScan3dCoordinateOffset", axis.offset) != C4HDL_ERR_SUCCESS) {
            if (errorMessage) {
                *errorMessage = "C4Utility buffer is missing Scan3d chunk geometry for "
                    + std::string(axis.selector) + ".";
            }
            return false;
        }
        if (!std::isfinite(*axis.scale) || !std::isfinite(*axis.offset)) {
            if (errorMessage) *errorMessage = "C4Utility returned invalid Scan3d chunk geometry for "
                + std::string(axis.selector) + ".";
            return false;
        }
        return true;
    };

    const AxisValues coordinateC{"CoordinateC", &copied.zScale, &copied.zOffset};
    if (!readAxis(coordinateC)) return false;

    const std::string outputMode = lowerCase(copied.outputMode);
    if (outputMode == "rectifiedc") {
        const AxisValues coordinateA{"CoordinateA", &copied.xScale, &copied.xOffset};
        const AxisValues coordinateB{"CoordinateB", &copied.yScale, &copied.yOffset};
        if (!readAxis(coordinateA) || !readAxis(coordinateB)) return false;
    } else if (outputMode != "calibratedc") {
        if (errorMessage) *errorMessage = "C4Utility returned an unsupported Scan3d output mode: " + copied.outputMode;
        return false;
    }

    *geometry = std::move(copied);
    return true;
}

std::string readFeatureValue(const C4_FEATUREINFO feature, const FeatureType type)
{
    if (type == FeatureType::Command || type == FeatureType::Unknown) return {};
    if (type == FeatureType::Integer || type == FeatureType::Boolean) {
        std::int64_t value = 0;
        return C4Ftr_readInteger(feature, &value) == C4HDL_ERR_SUCCESS
            ? std::to_string(value)
            : std::string("<unavailable>");
    }
    if (type == FeatureType::Float) {
        double value = 0.0;
        if (C4Ftr_readFloat(feature, &value) != C4HDL_ERR_SUCCESS) return "<unavailable>";
        std::ostringstream stream;
        stream << std::setprecision(12) << value;
        return stream.str();
    }
    std::string error;
    const std::string value = readSdkString(
        [feature, type](char* buffer, std::size_t* size) {
            return type == FeatureType::Enumeration
                ? C4Ftr_readString(feature, buffer, size)
                : C4Ftr_readString(feature, buffer, size);
        },
        &error);
    return error.empty() ? value : std::string("<unavailable>");
}

} // namespace

HeliotisC4System::HeliotisC4System(const std::string& sdkRoot)
{
    if (sdkRoot.empty()) throw std::runtime_error("C4Utility SDK root is empty.");
    const auto layout = internal::C4UtilitySdkRuntime::fromRoot(sdkRoot);
    if (!layout.isRuntimeComplete()) throw std::runtime_error(layout.runtimeDiagnostic());

    std::string error;
    if (!check(C4Hdl_open(&_handler), &error)) {
        throw std::runtime_error(error);
    }
    logInfo("C4Utility handler opened from " + sdkRoot + ".");
}

HeliotisC4System::~HeliotisC4System()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_handler) {
        C4Hdl_close(_handler);
        _handler = nullptr;
    }
}

std::vector<DeviceDescriptor> HeliotisC4System::discoverDevices(std::string* errorMessage)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<DeviceDescriptor> devices;
    if (!_handler) {
        if (errorMessage) *errorMessage = "C4Utility handler is not open.";
        return devices;
    }

    std::int64_t interfaceCount = 0;
    if (!check(C4Hdl_updateInterfaceList(_handler, &interfaceCount), errorMessage)) {
        logWarning("Interface discovery failed: " + (errorMessage ? *errorMessage : lastSdkError()));
        return devices;
    }
    logInfo("Discovery found " + std::to_string(interfaceCount) + " C4 interface(s).");

    for (std::int64_t interfaceIndex = 0; interfaceIndex < interfaceCount; ++interfaceIndex) {
        const std::string interfaceName = readSdkString(
            [this, interfaceIndex](char* buffer, std::size_t* size) {
                return C4Hdl_getInterfaceName(_handler, interfaceIndex, buffer, size);
            },
            errorMessage);
        if (interfaceName.empty() && errorMessage && !errorMessage->empty()) return {};

        C4_INTERFACE interfaceHandle = nullptr;
        if (!check(C4Hdl_openInterface(_handler, &interfaceHandle, interfaceIndex), errorMessage)) return {};

        std::int64_t deviceCount = 0;
        const bool updated = check(C4If_updateDeciveList(interfaceHandle, &deviceCount), errorMessage);
        if (!updated) {
            C4If_release(interfaceHandle);
            return {};
        }

        for (std::int64_t deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex) {
            const std::string deviceName = readSdkString(
                [interfaceHandle, deviceIndex](char* buffer, std::size_t* size) {
                    return C4If_getDeviceName(interfaceHandle, deviceIndex, buffer, size);
                },
                errorMessage);
            if (deviceName.empty() && errorMessage && !errorMessage->empty()) {
                C4If_release(interfaceHandle);
                return {};
            }
            devices.push_back({interfaceIndex, deviceIndex, interfaceName, deviceName});
            logInfo("Discovered device [interface=" + std::to_string(interfaceIndex)
                + ", device=" + std::to_string(deviceIndex) + "]: " + deviceName + ".");
        }
        C4If_release(interfaceHandle);
    }
    logInfo("Discovery completed with " + std::to_string(devices.size()) + " Heliotis device(s).");
    return devices;
}

std::unique_ptr<HeliotisC4Device> HeliotisC4System::createDevice()
{
    return std::make_unique<HeliotisC4Device>(this);
}

HeliotisC4Device::HeliotisC4Device(HeliotisC4System* system)
    : _system(system)
{
}

HeliotisC4Device::~HeliotisC4Device()
{
    close();
}

bool HeliotisC4Device::open(const DeviceDescriptor& descriptor, std::string* errorMessage)
{
    close();
    logInfo("Opening device [interface=" + std::to_string(descriptor.interfaceIndex)
        + ", device=" + std::to_string(descriptor.deviceIndex) + "]: " + descriptor.deviceName + ".");
    if (!_system) {
        if (errorMessage) *errorMessage = "C4Utility system is unavailable.";
        return false;
    }
    if (descriptor.interfaceIndex < 0 || descriptor.deviceIndex < 0) {
        if (errorMessage) *errorMessage = "The selected Heliotis device has an invalid interface or device index.";
        return false;
    }

    C4_INTERFACE interfaceHandle = nullptr;
    C4_DEVICE deviceHandle = nullptr;
    {
        std::lock_guard<std::mutex> systemLock(_system->_mutex);
        if (!_system->_handler) {
            if (errorMessage) *errorMessage = "C4Utility handler is not open.";
            return false;
        }
        if (!check(C4Hdl_openInterface(_system->_handler, &interfaceHandle, descriptor.interfaceIndex), errorMessage)) return false;
        std::int64_t deviceCount = 0;
        if (!check(C4If_updateDeciveList(interfaceHandle, &deviceCount), errorMessage)) {
            C4If_release(interfaceHandle);
            return false;
        }
        if (descriptor.deviceIndex >= deviceCount) {
            if (errorMessage) {
                *errorMessage = "The selected Heliotis device is no longer present in the refreshed interface device list.";
            }
            C4If_release(interfaceHandle);
            return false;
        }
        if (!check(C4If_openDevice(interfaceHandle, &deviceHandle, descriptor.deviceIndex), errorMessage)) {
            C4If_release(interfaceHandle);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        _interface = interfaceHandle;
        _device = deviceHandle;
        _connectedDeviceName = descriptor.deviceName;
    }
    dispatchConnectionStatus(true);
    logInfo("Device connection opened: " + descriptor.deviceName + ".");
    return true;
}

void HeliotisC4Device::close()
{
    stopAcquisition();

    C4_INTERFACE interfaceHandle = nullptr;
    C4_DEVICE deviceHandle = nullptr;
    bool wasOpen = false;
    {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        wasOpen = _device != nullptr;
        interfaceHandle = _interface;
        deviceHandle = _device;
        _interface = nullptr;
        _device = nullptr;
        _connectedDeviceName.clear();
    }
    if (deviceHandle) C4Dev_release(deviceHandle);
    if (interfaceHandle) C4If_release(interfaceHandle);
    if (wasOpen) {
        logInfo("Device connection closed.");
        dispatchConnectionStatus(false);
    }
}

bool HeliotisC4Device::isOpened() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _device != nullptr;
}

std::string HeliotisC4Device::connectedDeviceName() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _connectedDeviceName;
}

bool HeliotisC4Device::configureH8SurfaceExample(std::string* errorMessage)
{
    if (!isOpened()) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
        return false;
    }
    if (isAcquiring()) {
        if (errorMessage) *errorMessage = "The H8 reference profile cannot be applied during acquisition.";
        return false;
    }

    struct FeatureWrite final {
        const char* name;
        const char* value;
    };
    // C4Utility 1.12 h8SurfSimple, plus the Scan3d chunks required by this
    // module's deep-owned geometry contract.
    static constexpr std::array<FeatureWrite, 36> preStageInitWrites{{
        {"ComponentSelector", "Intensity"}, {"ComponentEnable", "0"},
        {"ComponentSelector", "Range"}, {"ComponentEnable", "1"},
        {"ComponentSelector", "Reflectance"}, {"ComponentEnable", "1"},
        {"ComponentSelector", "Phase"}, {"ComponentEnable", "0"},
        {"ChunkModeActive", "1"},
        {"ChunkSelector", "PartCount"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "PartType"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "Scan3dDistanceUnit"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "Scan3dOutputMode"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "Scan3dCoordinateScale"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "Scan3dCoordinateOffset"}, {"ChunkEnable", "1"},
        {"TriggerSelector", "RecordingStart"}, {"TriggerMode", "On"}, {"TriggerSource", "Stage"},
        {"TriggerSelector", "AcquisitionStart"}, {"TriggerMode", "Off"},
        {"TriggerSelector", "FrameStart"}, {"TriggerMode", "On"}, {"TriggerSource", "Software"},
        {"EncoderSelector", "Camera"}, {"EncoderInverter", "1"},
        {"ScanPosition", "-1.4"}, {"ScanRange", "0.5"}, {"ScanSpeed", "5.0"},
        {"GeneralSpeed", "10.0"}, {"ScanMode", "Down"},
    }};
    static constexpr std::array<FeatureWrite, 9> postStageInitWrites{{
        {"Scan3dExtractionMethod", "AcceleratedCenterOfMassIQCorrection"},
        {"Scan3dScalingMethod", "zTags"}, {"Scan3dDistanceUnit", "um"},
        {"TargetVerticalSpacing", "2.5"}, {"ExposureRatio", "1.0"},
        {"FPNCorrection", "AverageLastFrames"}, {"FPNCorrectionNFrames", "8"},
        {"ExtSimpMaxHWin", "7"}, {"LightControllerSelector", "LightController0"},
    }};
    static constexpr std::array<FeatureWrite, 4> illuminationWrites{{
        {"LightControllerSource", "UserOutput0"}, {"LightBrightness", "100.0"},
        {"UserOutputSelector", "UserOutput0"}, {"UserOutputValue", "1"},
    }};

    const auto applyWrites = [this, errorMessage](const auto& steps, const char* phase) {
        for (std::size_t index = 0; index < steps.size(); ++index) {
            const auto& step = steps[index];
            logInfo(std::string("H8 reference profile [") + phase + " "
                + std::to_string(index + 1) + "/" + std::to_string(steps.size()) + "]: "
                + step.name + "=" + step.value + ".");
            if (!writeFeature(step.name, step.value, errorMessage)) {
                logWarning(std::string("H8 reference profile failed at ") + step.name + "=" + step.value + ".");
                return false;
            }
        }
        return true;
    };

    logInfo("Applying C4Utility h8SurfSimple reference profile to the connected H8.");
    if (!applyWrites(preStageInitWrites, "pre-stage-init")) return false;

    logInfo("H8 reference profile: executing StageInit after motion configuration.");
    if (!executeCommand("StageInit", errorMessage)) return false;

    constexpr auto initializationTimeout = std::chrono::seconds(30);
    constexpr auto pollInterval = std::chrono::milliseconds(100);
    const auto deadline = std::chrono::steady_clock::now() + initializationTimeout;
    std::int64_t initialized = 0;
    std::size_t pollCount = 0;
    do {
        std::this_thread::sleep_for(pollInterval);
        {
            std::scoped_lock lock(_stateMutex, _sdkMutex);
            if (!_device) {
                if (errorMessage) *errorMessage = "Heliotis C4 device disconnected during reference-profile StageInit.";
                return false;
            }
            if (!check(C4Dev_readInteger(_device, "StageInitialized", &initialized), errorMessage)) return false;
        }
        ++pollCount;
        if (pollCount == 1 || initialized != 0 || pollCount % 10 == 0) {
            logInfo("H8 reference profile: StageInitialized=" + std::to_string(initialized)
                + " (poll " + std::to_string(pollCount) + ").");
        }
        if (initialized != 0) break;
    } while (std::chrono::steady_clock::now() < deadline);
    if (initialized == 0) {
        if (errorMessage) *errorMessage = "H8 reference-profile StageInit did not finish within 30 seconds.";
        logWarning("H8 reference profile: StageInit timed out after 30 seconds.");
        return false;
    }

    if (!applyWrites(postStageInitWrites, "post-stage-init")) return false;
    if (!applyWrites(illuminationWrites, "illumination")) return false;
    logInfo("H8 reference profile completed; Range/Reflectance, chunks, triggers, motion, processing, and illumination are configured.");
    return true;
}

bool HeliotisC4Device::initializeMotion(std::string* errorMessage)
{
    constexpr auto initializationTimeout = std::chrono::seconds(30);
    constexpr auto pollInterval = std::chrono::milliseconds(100);

    auto readInitialized = [this, errorMessage](std::int64_t* initialized) {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
            return false;
        }
        if (_acquiring) {
            if (errorMessage) *errorMessage = "Heliotis motion cannot be initialized during acquisition.";
            return false;
        }
        if (!check(C4Dev_readInteger(_device, "StageInitialized", initialized), errorMessage)) {
            if (errorMessage && !errorMessage->empty()) {
                *errorMessage = "Could not read H8 StageInitialized: " + *errorMessage;
            }
            return false;
        }
        return true;
    };

    std::int64_t initialized = 0;
    if (!readInitialized(&initialized)) return false;
    logInfo("Motion readiness check: StageInitialized=" + std::to_string(initialized) + ".");
    if (initialized != 0) {
        logInfo("Motion initialization is already complete.");
        return true;
    }

    {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device disconnected before motion initialization.";
            return false;
        }
        logInfo("Executing StageInit because StageInitialized is false.");
        if (!check(C4Dev_executeCommand(_device, "StageInit"), errorMessage)) {
            if (errorMessage && !errorMessage->empty()) {
                *errorMessage = "Could not start H8 StageInit: " + *errorMessage;
            }
            return false;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + initializationTimeout;
    do {
        std::this_thread::sleep_for(pollInterval);
        if (!readInitialized(&initialized)) return false;
        if (initialized != 0) {
            logInfo("StageInit completed successfully.");
            return true;
        }
    } while (std::chrono::steady_clock::now() < deadline);

    if (errorMessage) *errorMessage = "H8 StageInit did not finish within 30 seconds.";
    logWarning("StageInit timed out after 30 seconds.");
    return false;
}

HeliotisC4::FeatureList HeliotisC4Device::readFeatures(std::string* errorMessage) const
{
    std::scoped_lock lock(_stateMutex, _sdkMutex);
    HeliotisC4::FeatureList features;
    if (!_device) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
        return features;
    }

    C4_FEATUREINFO* featureList = nullptr;
    if (!check(C4Dev_getFeatureList(_device, &featureList), errorMessage)) return features;
    if (!featureList) return features;

    constexpr std::size_t maximumFeatureCount = 4096;
    for (std::size_t index = 0; index < maximumFeatureCount && featureList[index] != nullptr; ++index) {
        const C4_FEATUREINFO feature = featureList[index];
        std::string metadataError;
        const std::string name = readSdkString(
            [feature](char* buffer, std::size_t* size) { return C4Ftr_getName(feature, buffer, size); },
            &metadataError);
        if (!metadataError.empty() || name.empty()) continue;

        const std::string category = readSdkString(
            [feature](char* buffer, std::size_t* size) { return C4Ftr_getCategory(feature, buffer, size); },
            &metadataError);
        const std::string description = readSdkString(
            [feature](char* buffer, std::size_t* size) { return C4Ftr_getDescription(feature, buffer, size); },
            &metadataError);
        Type_e sdkType = C4FTR_TYPE_UNKNOWN;
        AccessMode_e sdkAccess = C4FTR_ACCESSMODE_UNKNOWN;
        C4Ftr_getType(feature, &sdkType);
        C4Ftr_getAccessMode(feature, &sdkAccess);
        const FeatureType type = featureType(sdkType);
        const FeatureAccess access = featureAccess(sdkAccess);
        if (access != FeatureAccess::ReadOnly && !isWritable(access)) {
            continue;
        }
        std::vector<std::string> enumEntries;
        if (type == FeatureType::Enumeration) {
            std::string enumError;
            enumEntries = splitEnumEntries(readSdkString(
                [feature](char* buffer, std::size_t* size) {
                    return C4Ftr_getEnumEntryList(feature, buffer, size);
                },
                &enumError));
        }
        features.push_back({
            isMotionFeature(category, name) ? FeatureSection::Motion : FeatureSection::Device,
            category,
            name,
            type == FeatureType::Command ? std::string("Execute")
                : (access == FeatureAccess::WriteOnly ? std::string("<write only>") : readFeatureValue(feature, type)),
            description,
            type,
            access,
            std::move(enumEntries),
        });
    }
    C4Ftr_release(featureList);
    logInfo("Read " + std::to_string(features.size()) + " available feature(s).");
    return features;
}

bool HeliotisC4Device::writeFeature(
    const std::string& name,
    const std::string& value,
    std::string* errorMessage)
{
    const std::string trimmedValue = trim(value);
    if (name.empty() || trimmedValue.empty()) {
        if (errorMessage) *errorMessage = "Heliotis feature name and value are required.";
        return false;
    }

    logInfo("Feature write requested: " + name + "=" + trimmedValue + ".");
    std::scoped_lock lock(_stateMutex, _sdkMutex);
    if (!_device) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
        return false;
    }
    if (_acquiring) {
        if (errorMessage) *errorMessage = "Heliotis feature writes are unavailable during acquisition.";
        return false;
    }

    FeatureType type = FeatureType::Unknown;
    FeatureAccess access = FeatureAccess::Unknown;
    if (!findFeatureMetadata(_device, name, &type, &access, errorMessage)) return false;
    if (!isWritable(access) || type == FeatureType::Command) {
        if (errorMessage) *errorMessage = "Heliotis feature is not writable: " + name;
        return false;
    }

    C4HDL_ERROR result = C4HDL_ERR_ERROR;
    switch (type) {
    case FeatureType::Integer:
    case FeatureType::Boolean: {
        std::string normalized = lowerCase(trimmedValue);
        if (type == FeatureType::Boolean) {
            if (normalized == "true") normalized = "1";
            if (normalized == "false") normalized = "0";
        }
        char* end = nullptr;
        errno = 0;
        const long long parsed = std::strtoll(normalized.c_str(), &end, 10);
        if (errno == ERANGE || end == normalized.c_str() || *end != '\0'
            || (type == FeatureType::Boolean && parsed != 0 && parsed != 1)) {
            if (errorMessage) *errorMessage = "Invalid integer value for Heliotis feature " + name + ".";
            return false;
        }
        result = C4Dev_writeInteger(_device, name.c_str(), static_cast<std::int64_t>(parsed));
        break;
    }
    case FeatureType::Float: {
        char* end = nullptr;
        errno = 0;
        const double parsed = std::strtod(trimmedValue.c_str(), &end);
        if (errno == ERANGE || end == trimmedValue.c_str() || *end != '\0' || !std::isfinite(parsed)) {
            if (errorMessage) *errorMessage = "Invalid floating-point value for Heliotis feature " + name + ".";
            return false;
        }
        result = C4Dev_writeFloat(_device, name.c_str(), parsed);
        break;
    }
    case FeatureType::String:
        result = C4Dev_writeString(_device, name.c_str(), trimmedValue.c_str());
        break;
    case FeatureType::Enumeration:
        result = C4Dev_writeEnumeration(_device, name.c_str(), trimmedValue.c_str());
        break;
    default:
        if (errorMessage) *errorMessage = "Heliotis feature type is not writable: " + name;
        return false;
    }
    const bool succeeded = check(result, errorMessage);
    if (succeeded) {
        logInfo("Feature write completed: " + name + "=" + trimmedValue + ".");
        if (name == "TriggerSelector" || name == "TriggerMode" || name == "TriggerSource") {
            logInfo("Current trigger configuration: " + triggerConfigurationSummary(_device) + ".");
        }
    } else {
        logWarning("Feature write failed for " + name + ": "
            + (errorMessage && !errorMessage->empty() ? *errorMessage : lastSdkError()));
    }
    return succeeded;
}

bool HeliotisC4Device::executeCommand(const std::string& name, std::string* errorMessage)
{
    if (name.empty()) {
        if (errorMessage) *errorMessage = "Heliotis command name is required.";
        return false;
    }

    logInfo("Command requested: " + name + ".");
    std::scoped_lock lock(_stateMutex, _sdkMutex);
    if (!_device) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
        return false;
    }
    if (_acquiring) {
        if (errorMessage) *errorMessage = "Heliotis commands are unavailable during acquisition.";
        return false;
    }

    FeatureType type = FeatureType::Unknown;
    FeatureAccess access = FeatureAccess::Unknown;
    if (!findFeatureMetadata(_device, name, &type, &access, errorMessage)) return false;
    if (type != FeatureType::Command || !isWritable(access)) {
        if (errorMessage) *errorMessage = "Heliotis command is not executable: " + name;
        return false;
    }
    const bool succeeded = check(C4Dev_executeCommand(_device, name.c_str()), errorMessage);
    if (succeeded) logInfo("Command completed: " + name + ".");
    else logWarning("Command failed for " + name + ": "
        + (errorMessage && !errorMessage->empty() ? *errorMessage : lastSdkError()));
    return succeeded;
}

bool HeliotisC4Device::triggerSoftware(std::string* errorMessage)
{
    std::scoped_lock lock(_stateMutex, _sdkMutex);
    if (!_device) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
        return false;
    }
    if (!_acquiring) {
        if (errorMessage) *errorMessage = "Software trigger requires an armed Heliotis acquisition.";
        logWarning("Software trigger rejected because acquisition is not armed.");
        return false;
    }

    logInfo("Software trigger requested while acquisition is armed. " + triggerConfigurationSummary(_device) + ".");
    FeatureType type = FeatureType::Unknown;
    FeatureAccess access = FeatureAccess::Unknown;
    if (!findFeatureMetadata(_device, "TriggerSoftware", &type, &access, errorMessage)) {
        logWarning("Software trigger metadata lookup failed: "
            + (errorMessage && !errorMessage->empty() ? *errorMessage : lastSdkError()));
        return false;
    }
    if (type != FeatureType::Command || !isWritable(access)) {
        if (errorMessage) {
            *errorMessage = "TriggerSoftware is unavailable. Select FrameStart with TriggerMode=On and TriggerSource=Software.";
        }
        logWarning("Software trigger is unavailable. " + triggerConfigurationSummary(_device) + ".");
        return false;
    }
    const bool succeeded = check(C4Dev_executeCommand(_device, "TriggerSoftware"), errorMessage);
    if (succeeded) logInfo("Software trigger command accepted by C4Utility.");
    else logWarning("Software trigger command failed: "
        + (errorMessage && !errorMessage->empty() ? *errorMessage : lastSdkError()));
    return succeeded;
}

bool HeliotisC4Device::startAcquisition(
    const AcquisitionMode mode,
    FrameCallback frameCallback,
    std::string* errorMessage)
{
    if (!frameCallback) {
        if (errorMessage) *errorMessage = "Heliotis acquisition requires a frame callback.";
        return false;
    }

    stopAcquisition();

    const char* modeName = mode == AcquisitionMode::SingleFrame ? "Single" : "Live";
    logInfo(std::string("Acquisition arm requested: mode=") + modeName + ".");

    C4_DEVICE device = nullptr;
    {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
            return false;
        }

        device = _device;
        if (!validateChunkMetadataConfiguration(device, errorMessage)) {
            logWarning(std::string("Acquisition arm blocked by chunk configuration: ")
                + (errorMessage && !errorMessage->empty() ? *errorMessage : lastSdkError()));
            return false;
        }
        const std::int64_t bufferCount = mode == AcquisitionMode::SingleFrame ? 1 : 4;
        logInfo(std::string("Calling C4Dev_startAcquisition with ") + std::to_string(bufferCount)
            + " host buffer(s). " + triggerConfigurationSummary(device) + ".");
        if (!check(C4Dev_startAcquisition(device, bufferCount), errorMessage)) {
            logWarning(std::string("C4Dev_startAcquisition failed: ")
                + (errorMessage && !errorMessage->empty() ? *errorMessage : lastSdkError()));
            return false;
        }

        _stopAcquisitionRequested.store(false);
        _acquiring = true;
        _lastAcquisitionError.clear();
    }
    dispatchStatus(Status::Acquisition, true);
    logInfo(std::string("Acquisition armed: mode=") + modeName + ".");

    try {
        std::thread worker(&HeliotisC4Device::acquisitionLoop, this, device, mode, std::move(frameCallback));
        std::lock_guard<std::mutex> lock(_stateMutex);
        _acquisitionThread = std::move(worker);
    } catch (const std::exception& exception) {
        {
            std::lock_guard<std::mutex> lock(_sdkMutex);
            C4Dev_stopAcquisition(device);
        }
        setAcquisitionError(exception.what());
        finishAcquisition();
        if (errorMessage) *errorMessage = exception.what();
        return false;
    }

    return true;
}

void HeliotisC4Device::stopAcquisition()
{
    std::thread worker;
    C4_DEVICE device = nullptr;
    bool wasAcquiring = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_acquisitionThread.joinable() && !_acquiring) return;
        _stopAcquisitionRequested.store(true);
        device = _device;
        wasAcquiring = _acquiring;
        worker = std::move(_acquisitionThread);
    }

    if (wasAcquiring && device) {
        logInfo("Stopping armed acquisition.");
        std::lock_guard<std::mutex> lock(_sdkMutex);
        C4Dev_stopAcquisition(device);
    }
    if (worker.joinable() && worker.get_id() == std::this_thread::get_id()) {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _acquisitionThread = std::move(worker);
        return;
    }
    if (worker.joinable()) worker.join();

    finishAcquisition();
}

bool HeliotisC4Device::isAcquiring() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _acquiring;
}

std::string HeliotisC4Device::lastAcquisitionError() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _lastAcquisitionError;
}

bool HeliotisC4Device::copyFrame(const C4_BUFFER buffer, Frame* frame, std::string* errorMessage) const
{
    if (!buffer || !frame) {
        if (errorMessage) *errorMessage = "C4Utility returned an invalid acquisition buffer.";
        return false;
    }

    std::int64_t partCount = 0;
    if (!check(C4Buf_getNumParts(buffer, &partCount), errorMessage) || partCount <= 0) {
        if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility buffer has no data parts.";
        return false;
    }

    std::int64_t chunkPartCount = 0;
    if (C4Buf_readInteger(buffer, "ChunkPartCount", &chunkPartCount) != C4HDL_ERR_SUCCESS
        || chunkPartCount != partCount) {
        if (errorMessage) {
            *errorMessage = "C4Utility buffer is missing complete ChunkPartCount metadata. "
                "Enable ChunkPartCount and ChunkPartType in the H8 configuration before acquisition.";
        }
        return false;
    }

    Scan3dGeometry scan3dGeometry;
    if (!readScan3dGeometry(buffer, &scan3dGeometry, errorMessage)) return false;

    Frame copied;
    std::int64_t chunkFrameId = 0;
    if (C4Buf_readInteger(buffer, "ChunkFrameID", &chunkFrameId) == C4HDL_ERR_SUCCESS && chunkFrameId >= 0) {
        copied.frameId = std::to_string(chunkFrameId);
    }
    std::int64_t chunkTimestamp = 0;
    if (C4Buf_readInteger(buffer, "ChunkTimestamp", &chunkTimestamp) == C4HDL_ERR_SUCCESS && chunkTimestamp >= 0) {
        copied.timestampNs = static_cast<std::uint64_t>(chunkTimestamp);
    }
    copied.parts.reserve(static_cast<std::size_t>(partCount));
    for (std::int64_t partIndex = 0; partIndex < partCount; ++partIndex) {
        std::array<std::int64_t, 2> dimensions{};
        std::uint32_t dimensionCount = static_cast<std::uint32_t>(dimensions.size());
        if (!check(C4Buf_getPartDimension(buffer, partIndex, dimensions.data(), &dimensionCount), errorMessage)
            || dimensionCount != dimensions.size()
            || dimensions[0] <= 0 || dimensions[1] <= 0
            || dimensions[0] > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())
            || dimensions[1] > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())) {
            if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility returned unsupported buffer dimensions.";
            return false;
        }

        const auto width = static_cast<std::uint32_t>(dimensions[0]);
        const auto height = static_cast<std::uint32_t>(dimensions[1]);
        const auto expectedSamples = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
        if (expectedSamples == 0 || expectedSamples > (std::numeric_limits<std::uint32_t>::max)()) {
            if (errorMessage) *errorMessage = "C4Utility buffer part exceeds the supported sample count.";
            return false;
        }

        std::int64_t pixelFormat = 0;
        if (!check(C4Buf_getPartPixelformat(buffer, partIndex, &pixelFormat), errorMessage)) return false;
        std::string pixelFormatError;
        const std::string pixelFormatName = readSdkString(
            [buffer, pixelFormat](char* text, std::size_t* size) {
                return C4Buf_getPixelformatName(buffer, pixelFormat, text, size);
            },
            &pixelFormatError);
        if (!pixelFormatError.empty()) {
            if (errorMessage) *errorMessage = pixelFormatError;
            return false;
        }

        if (C4Buf_writeInteger(buffer, "ChunkPartSelector", partIndex) != C4HDL_ERR_SUCCESS) {
            if (errorMessage) {
                *errorMessage = "C4Utility buffer cannot select ChunkPartSelector. "
                    "Enable ChunkPartType in the H8 configuration before acquisition.";
            }
            return false;
        }
        std::string metadataError;
        const std::string partName = readSdkString(
            [buffer](char* text, std::size_t* size) {
                return C4Buf_readString(buffer, "ChunkPartType", text, size);
            },
            &metadataError);
        if (!metadataError.empty() || partName.empty()) {
            if (errorMessage) {
                *errorMessage = "C4Utility buffer is missing ChunkPartType metadata. "
                    "Enable ChunkPartType in the H8 configuration before acquisition.";
            }
            return false;
        }

        FramePart part;
        part.kind = framePartKind(partName);
        if (part.kind == FramePartKind::Unknown) {
            if (errorMessage) *errorMessage = "C4Utility returned an unsupported H8 chunk part type: " + partName;
            return false;
        }
        part.name = partName;
        part.pixelFormat = pixelFormatName;
        part.width = width;
        part.height = height;
        double fixedPointScale = 1.0;
        const bool floatingPointSamples = usesFloatingPointSamples(pixelFormatName);
        if (!floatingPointSamples
            && C4Buf_readFloat(buffer, "ChunkPartFixpointScaling", &fixedPointScale) == C4HDL_ERR_SUCCESS) {
            if (!std::isfinite(fixedPointScale)) {
                if (errorMessage) *errorMessage = "C4Utility returned an invalid ChunkPartFixpointScaling value.";
                return false;
            }
            part.sampleScale = fixedPointScale;
        }
        std::uint32_t sampleCount = static_cast<std::uint32_t>(expectedSamples);
        if (floatingPointSamples) {
            std::vector<double> samples(sampleCount);
            if (!check(C4Buf_getDataPartFloat(buffer, partIndex, samples.data(), &sampleCount), errorMessage)
                || sampleCount != expectedSamples) {
                if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility returned an invalid floating-point part size.";
                return false;
            }
            part.bitsPerSample = 64;
            part.samples = std::move(samples);
        } else {
            std::vector<std::uint16_t> samples(sampleCount);
            if (!check(C4Buf_getDataPartUint16(buffer, partIndex, samples.data(), &sampleCount), errorMessage)
                || sampleCount != expectedSamples) {
                if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility returned an invalid uint16 part size.";
                return false;
            }
            part.bitsPerSample = 16;
            part.samples = std::move(samples);
        }
        copied.parts.push_back(std::move(part));
    }

    if (!copied.isValid()) {
        if (errorMessage) *errorMessage = "C4Utility produced an invalid frame payload.";
        return false;
    }
    copied.scan3dGeometry = std::move(scan3dGeometry);
    *frame = std::move(copied);
    return true;
}

void HeliotisC4Device::acquisitionLoop(
    const C4_DEVICE device,
    const AcquisitionMode mode,
    FrameCallback frameCallback)
{
    constexpr auto bufferTimeout = std::chrono::milliseconds(250);
    std::size_t timeoutCount = 0;
    std::size_t receivedFrameCount = 0;
    logInfo(std::string("Acquisition worker started: mode=")
        + (mode == AcquisitionMode::SingleFrame ? "Single" : "Live") + ".");
    while (!_stopAcquisitionRequested.load()) {
        C4_BUFFER buffer = nullptr;
        Frame frame;
        std::string error;
        C4HDL_ERROR result = C4HDL_ERR_ERROR;
        std::chrono::milliseconds bufferWait{};
        {
            std::lock_guard<std::mutex> lock(_sdkMutex);
            const auto waitStarted = std::chrono::steady_clock::now();
            result = C4Dev_getBuffer(device, &buffer, bufferTimeout.count());
            bufferWait = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - waitStarted);
            if (result == C4HDL_ERR_SUCCESS) {
                bool copied = false;
                try {
                    copied = copyFrame(buffer, &frame, &error);
                } catch (const std::exception& exception) {
                    error = exception.what();
                } catch (...) {
                    error = "An unknown exception occurred while copying a C4Utility buffer.";
                }
                const C4HDL_ERROR releaseResult = C4Buf_release(buffer);
                buffer = nullptr;
                if (!copied && error.empty()) error = "C4Utility could not copy an acquisition buffer.";
                if (releaseResult != C4HDL_ERR_SUCCESS && error.empty()) error = lastSdkError();
                if (!error.empty()) result = C4HDL_ERR_ERROR;
            } else {
                error = lastSdkError();
            }
        }

        if (result != C4HDL_ERR_SUCCESS) {
            if (_stopAcquisitionRequested.load()) break;
            if (isLikelyTimeoutError(error, bufferWait, bufferTimeout)) {
                ++timeoutCount;
                if (timeoutCount == 1 || timeoutCount % 20 == 0) {
                    logInfo("Acquisition is armed but waiting for a frame/trigger (timeouts="
                        + std::to_string(timeoutCount) + ").");
                }
                continue;
            }
            logWarning("Acquisition buffer wait failed: "
                + (error.empty() ? std::string("C4Utility acquisition failed.") : error));
            setAcquisitionError(error.empty() ? "C4Utility acquisition failed." : error);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            frame.sequence = _nextFrameSequence++;
        }
        ++receivedFrameCount;
        if (mode == AcquisitionMode::SingleFrame || receivedFrameCount <= 3 || receivedFrameCount % 100 == 0) {
            logInfo("Received frame sequence " + std::to_string(frame.sequence)
                + " with " + std::to_string(frame.parts.size()) + " part(s), count="
                + std::to_string(receivedFrameCount) + ".");
        }
        try {
            frameCallback(std::move(frame));
        } catch (const std::exception& exception) {
            setAcquisitionError(exception.what());
            break;
        } catch (...) {
            setAcquisitionError("An unknown exception occurred in the Heliotis frame callback.");
            break;
        }
        if (mode == AcquisitionMode::SingleFrame) break;
    }

    if (!_stopAcquisitionRequested.load()) {
        std::lock_guard<std::mutex> lock(_sdkMutex);
        C4Dev_stopAcquisition(device);
    }
    logInfo("Acquisition worker finished.");
    finishAcquisition();
}

void HeliotisC4Device::finishAcquisition()
{
    bool wasAcquiring = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        wasAcquiring = _acquiring;
        _acquiring = false;
    }
    if (wasAcquiring) dispatchStatus(Status::Acquisition, false);
    if (wasAcquiring) logInfo("Acquisition disarmed.");
}

void HeliotisC4Device::setAcquisitionError(std::string message)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _lastAcquisitionError = std::move(message);
    logWarning("Acquisition error: " + _lastAcquisitionError);
}

HeliotisC4Device::CallbackId HeliotisC4Device::registerStatusCallback(StatusCallback callback)
{
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(_stateMutex);
    const CallbackId id = _nextCallbackId++;
    _statusCallbacks.emplace(id, std::move(callback));
    return id;
}

bool HeliotisC4Device::deregisterStatusCallback(const CallbackId id)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _statusCallbacks.erase(id) != 0;
}

void HeliotisC4Device::dispatchConnectionStatus(const bool connected)
{
    dispatchStatus(Status::Connection, connected);
}

void HeliotisC4Device::dispatchStatus(const Status status, const bool active)
{
    std::vector<StatusCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        callbacks.reserve(_statusCallbacks.size());
        for (const auto& [id, callback] : _statusCallbacks) callbacks.push_back(callback);
    }
    for (const auto& callback : callbacks) callback(status, active);
}

} // namespace heliotis
