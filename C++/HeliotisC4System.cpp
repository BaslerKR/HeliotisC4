#include "HeliotisC4System.h"
#include "Internal/C4UtilitySdkRuntime.h"

#include <algorithm>
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

} // namespace

HeliotisC4System::HeliotisC4System(const std::string& sdkRoot)
{
    if (sdkRoot.empty()) throw std::runtime_error("C4Utility SDK root is empty.");
    const auto layout = internal::C4UtilitySdkRuntime::fromRoot(sdkRoot);
    if (!layout.isComplete()) throw std::runtime_error(layout.diagnostic());

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
