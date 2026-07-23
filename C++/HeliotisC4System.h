#pragma once

#include "HeliotisC4.h"

#include <C4HdlC.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace heliotis {

class HeliotisC4Device;

class HeliotisC4System final {
public:
    explicit HeliotisC4System(const std::string& sdkRoot);
    ~HeliotisC4System();

    HeliotisC4System(const HeliotisC4System&) = delete;
    HeliotisC4System& operator=(const HeliotisC4System&) = delete;

    [[nodiscard]] std::vector<DeviceDescriptor> discoverDevices(std::string* errorMessage = nullptr);
    [[nodiscard]] std::unique_ptr<HeliotisC4Device> createDevice();

private:
    friend class HeliotisC4Device;

    C4_HANDLER _handler = nullptr;
    std::mutex _mutex;
};

class HeliotisC4Device final {
public:
    using CallbackId = std::size_t;

    enum class Status {
        Connection
    };
    using StatusCallback = std::function<void(Status status, bool connected)>;

    explicit HeliotisC4Device(HeliotisC4System* system);
    ~HeliotisC4Device();

    HeliotisC4Device(const HeliotisC4Device&) = delete;
    HeliotisC4Device& operator=(const HeliotisC4Device&) = delete;

    [[nodiscard]] bool open(const DeviceDescriptor& descriptor, std::string* errorMessage = nullptr);
    void close();
    [[nodiscard]] bool isOpened() const;
    [[nodiscard]] std::string connectedDeviceName() const;

    CallbackId registerStatusCallback(StatusCallback callback);
    bool deregisterStatusCallback(CallbackId id);

private:
    void dispatchConnectionStatus(bool connected);

    HeliotisC4System* _system = nullptr;
    C4_INTERFACE _interface = nullptr;
    C4_DEVICE _device = nullptr;
    std::string _connectedDeviceName;

    mutable std::mutex _stateMutex;
    std::unordered_map<CallbackId, StatusCallback> _statusCallbacks;
    CallbackId _nextCallbackId = 1;
};

} // namespace heliotis
