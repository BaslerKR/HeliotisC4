#pragma once

#include "HeliotisC4.h"

#include <C4HdlC.h>

#include <atomic>
#include <condition_variable>
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

/**
 * Owns one C4 device connection, explicit profile operation, and acquisition worker.
 *
 * @note The device object must outlive every registered status and frame callback.
 */
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

    /** Opens the exact refreshed discovery identity without initializing motion. */
    [[nodiscard]] bool open(const DeviceDescriptor& descriptor, std::string* errorMessage = nullptr);
    /**
     * Stops dependent work and releases the device and interface handles.
     *
     * @note A call from an acquisition/status callback or the active
     *       initialization thread requests cancellation but leaves handles
     *       open; a teardown owner must call close again afterward.
     */
    void close();
    [[nodiscard]] bool isOpened() const;
    [[nodiscard]] std::string connectedDeviceName() const;
    /**
     * Applies the motion-producing H8 reference profile without changing triggers.
     *
     * @param errorMessage Optional failure detail.
     * @return True when every required group succeeded; optional geometry and
     *         illumination groups may warn without failing the operation.
     * @note The caller must establish stage clearance before invoking this method.
     */
    [[nodiscard]] bool configureH8SurfaceExample(std::string* errorMessage = nullptr);
    /** Requests cancellation of a running H8 profile or motion initialization. */
    void requestCancelInitialization() noexcept;
    /**
     * Initializes stage motion without applying the complete reference profile.
     *
     * @param errorMessage Optional failure detail.
     * @return True when the stage was already initialized or StageInit completed;
     *         false when stage status/command support is unavailable or failed.
     * @note This operation is serialized against profile initialization and
     *       acquisition and may move the stage. It does not write capture,
     *       processing, illumination, user-set, or trigger feature values.
     */
    [[nodiscard]] bool initializeMotion(std::string* errorMessage = nullptr);
    [[nodiscard]] HeliotisC4::FeatureList readFeatures(std::string* errorMessage = nullptr) const;
    [[nodiscard]] bool writeFeature(
        const std::string& name,
        const std::string& value,
        std::string* errorMessage = nullptr);
    [[nodiscard]] bool executeCommand(const std::string& name, std::string* errorMessage = nullptr);
    /**
     * Queues one FrameStart software command for an armed acquisition.
     *
     * @param errorMessage Optional rejection detail.
     * @return True when the command was accepted by the worker queue.
     */
    [[nodiscard]] bool triggerSoftware(std::string* errorMessage = nullptr);
    /**
     * Starts one worker-owned acquisition cycle.
     *
     * @param mode Requested SingleFrame or Continuous behavior.
     * @param frameCallback Consumer for deep-owned frames.
     * @param errorMessage Optional arm failure detail.
     * @return True after C4Utility starts and the receive worker is installed.
     * @note Temporarily applies only the matching device AcquisitionMode and
     *       restores it after stop. Trigger selectors are inspected and restored
     *       to select the worker route; no trigger mode/source is changed.
     */
    [[nodiscard]] bool startAcquisition(
        AcquisitionMode mode,
        FrameCallback frameCallback,
        std::string* errorMessage = nullptr);
    /**
     * Requests worker-owned acquisition shutdown without waiting for delivery
     * callbacks or C4Utility shutdown to complete.
     *
     * @note Safe for UI event handlers. The acquisition status callback
     *       reports completion asynchronously.
     */
    void requestStopAcquisition() noexcept;
    /**
     * Stops acquisition and joins its worker for teardown callers.
     *
     * @note A call made from an acquisition callback cannot join its own
     *       worker and is converted to a non-blocking stop request.
     */
    void stopAcquisition();
    [[nodiscard]] bool isAcquiring() const;
    [[nodiscard]] bool isSoftwareTriggeredAcquisition() const;
    /**
     * Reports whether the armed acquisition currently accepts TriggerSoftware.
     *
     * @return True when FrameStart is configured as On/Software.
     * @note This reports the FrameStart configuration captured when acquisition was armed;
     *       changing trigger features during acquisition is outside the normal
     *       control path and is not reflected until the next arm.
     */
    [[nodiscard]] bool isSoftwareTriggerAvailable() const;
    /**
     * Reports whether a failed SDK stop requires a fresh device connection.
     *
     * @return True until the device is closed and opened again.
     */
    [[nodiscard]] bool requiresReconnect() const;
    [[nodiscard]] std::string lastAcquisitionError() const;

    CallbackId registerStatusCallback(StatusCallback callback);
    bool deregisterStatusCallback(CallbackId id);

private:
    /**
     * Deep-copies supported multipart data before releasing the SDK buffer.
     *
     * @param buffer Live C4Utility buffer.
     * @param detailedDiagnostics Whether to scan samples for diagnostic summaries.
     * @param frame Destination deep-owned frame.
     * @param errorMessage Optional copy failure detail.
     * @return True when a valid frame containing Range data was copied.
     * @note The C API part count is authoritative. ChunkPartCount is checked
     *       when present, while ChunkPartType remains required for semantic data.
     */
    [[nodiscard]] bool copyFrame(
        C4_BUFFER buffer,
        bool detailedDiagnostics,
        Frame* frame,
        std::string* errorMessage) const;
    /**
     * Runs the acquisition receive loop on the device worker thread.
     *
     * @param device C4Utility device handle to receive from.
     * @param mode Single-frame or continuous acquisition behavior.
     * @param softwareTriggered Whether FrameStart is controlled by software.
     * @param triggerDiagnosticSummary Trigger selector state captured before arming.
     * @param acquisitionId Monotonic identifier used to correlate one arm cycle.
     * @param originalAcquisitionMode Device mode to restore after SDK shutdown.
     * @param restoreAcquisitionMode Whether the arm operation changed that mode.
     * @param frameCallback Callback that receives each deep-owned frame.
     * @note C4Utility calls are serialized and bounded buffer polls keep stop,
     *       close, and queued software-trigger handling responsive.
     */
    void acquisitionLoop(
        C4_DEVICE device,
        AcquisitionMode mode,
        bool softwareTriggered,
        std::string triggerDiagnosticSummary,
        std::uint64_t acquisitionId,
        std::string originalAcquisitionMode,
        bool restoreAcquisitionMode,
        FrameCallback frameCallback);
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
    std::recursive_mutex _connectionLifecycleMutex;
    std::mutex _acquisitionArmMutex;
    std::mutex _acquisitionStopMutex;
    std::unordered_map<CallbackId, StatusCallback> _statusCallbacks;
    CallbackId _nextCallbackId = 1;
    std::thread _acquisitionThread;
    std::condition_variable _acquisitionWorkerCondition;
    bool _acquisitionWorkerInstalling = false;
    std::atomic<bool> _stopAcquisitionRequested{false};
    std::atomic<bool> _cancelInitializationRequested{false};
    std::mutex _acquisitionStartMutex;
    std::condition_variable _acquisitionStartCondition;
    bool _acquisitionWorkerReleased = false;
    std::mutex _triggerMutex;
    std::condition_variable _triggerCondition;
    std::size_t _pendingSoftwareTriggers = 0;
    bool _softwareTriggerInFlight = false;
    bool _softwareTriggeredAcquisition = false;
    bool _softwareTriggerAvailable = false;
    bool _acquiring = false;
    bool _activeStatusDispatching = false;
    std::thread::id _activeStatusDispatchThreadId;
    bool _initializing = false;
    std::condition_variable _initializationCondition;
    std::thread::id _initializationThreadId;
    bool _closing = false;
    bool _requiresReconnect = false;
    std::string _lastAcquisitionError;
    std::uint64_t _activeAcquisitionId = 0;
    std::uint64_t _nextAcquisitionId = 1;
    std::uint64_t _nextFrameSequence = 1;
};

} // namespace heliotis
