#pragma once

#include "HeliotisC4.h"

#include <C4HdlC.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
        Connection,
        Acquisition
    };
    using StatusCallback = std::function<void(Status status, bool connected)>;
    using FrameCallback = std::function<void(Frame&& frame)>;

    enum class AcquisitionMode {
        SingleFrame,
        Continuous
    };

    explicit HeliotisC4Device(HeliotisC4System* system);
    ~HeliotisC4Device();

    HeliotisC4Device(const HeliotisC4Device&) = delete;
    HeliotisC4Device& operator=(const HeliotisC4Device&) = delete;

    [[nodiscard]] bool open(const DeviceDescriptor& descriptor, std::string* errorMessage = nullptr);
    void close();
    [[nodiscard]] bool isOpened() const;
    [[nodiscard]] std::string connectedDeviceName() const;
    // Applies the vendor h8SurfSimple setup. It writes motion, trigger,
    // processing, and illumination features, so the H8 must be known-safe.
    [[nodiscard]] bool configureH8SurfaceExample(std::string* errorMessage = nullptr);
    [[nodiscard]] bool initializeMotion(std::string* errorMessage = nullptr);
    [[nodiscard]] HeliotisC4::FeatureList readFeatures(std::string* errorMessage = nullptr) const;
    [[nodiscard]] bool writeFeature(
        const std::string& name,
        const std::string& value,
        std::string* errorMessage = nullptr);
    [[nodiscard]] bool executeCommand(const std::string& name, std::string* errorMessage = nullptr);
    [[nodiscard]] bool triggerSoftware(std::string* errorMessage = nullptr);
    [[nodiscard]] bool startAcquisition(
        AcquisitionMode mode,
        FrameCallback frameCallback,
        std::string* errorMessage = nullptr);
    void stopAcquisition();
    [[nodiscard]] bool isAcquiring() const;
    [[nodiscard]] std::string lastAcquisitionError() const;

    CallbackId registerStatusCallback(StatusCallback callback);
    bool deregisterStatusCallback(CallbackId id);

private:
    [[nodiscard]] bool copyFrame(C4_BUFFER buffer, Frame* frame, std::string* errorMessage) const;
    void acquisitionLoop(C4_DEVICE device, AcquisitionMode mode, FrameCallback frameCallback);
    void finishAcquisition();
    void setAcquisitionError(std::string message);
    void dispatchStatus(Status status, bool active);
    void dispatchConnectionStatus(bool connected);

    HeliotisC4System* _system = nullptr;
    C4_INTERFACE _interface = nullptr;
    C4_DEVICE _device = nullptr;
    std::string _connectedDeviceName;

    mutable std::mutex _stateMutex;
    mutable std::mutex _sdkMutex;
    std::unordered_map<CallbackId, StatusCallback> _statusCallbacks;
    CallbackId _nextCallbackId = 1;
    std::thread _acquisitionThread;
    std::atomic<bool> _stopAcquisitionRequested{false};
    bool _acquiring = false;
    std::string _lastAcquisitionError;
    std::uint64_t _nextFrameSequence = 1;
};

} // namespace heliotis
