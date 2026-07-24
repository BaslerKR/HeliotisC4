#include "HeliotisC4System.h"
#include "Internal/C4UtilitySdkRuntime.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
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
    if (!check(C4Hdl_updateInterfaceList(_handler, &interfaceCount), errorMessage)) return devices;

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
        }
        C4If_release(interfaceHandle);
    }
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
    if (!_system) {
        if (errorMessage) *errorMessage = "C4Utility system is unavailable.";
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
    return true;
}

void HeliotisC4Device::close()
{
    C4_INTERFACE interfaceHandle = nullptr;
    C4_DEVICE deviceHandle = nullptr;
    bool wasOpen = false;
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        wasOpen = _device != nullptr;
        interfaceHandle = _interface;
        deviceHandle = _device;
        _interface = nullptr;
        _device = nullptr;
        _connectedDeviceName.clear();
    }
    if (deviceHandle) C4Dev_release(deviceHandle);
    if (interfaceHandle) C4If_release(interfaceHandle);
    if (wasOpen) dispatchConnectionStatus(false);
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

HeliotisC4::FeatureList HeliotisC4Device::readFeatures(std::string* errorMessage) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
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
        if ((access != FeatureAccess::ReadOnly && access != FeatureAccess::ReadWrite)
            && type != FeatureType::Command) {
            continue;
        }
        features.push_back({
            isMotionFeature(category, name) ? FeatureSection::Motion : FeatureSection::Device,
            category,
            name,
            type == FeatureType::Command ? std::string("Command") : readFeatureValue(feature, type),
            description,
            type,
            access,
        });
    }
    C4Ftr_release(featureList);
    return features;
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
    std::vector<StatusCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        callbacks.reserve(_statusCallbacks.size());
        for (const auto& [id, callback] : _statusCallbacks) callbacks.push_back(callback);
    }
    for (const auto& callback : callbacks) callback(Status::Connection, connected);
}

} // namespace heliotis
