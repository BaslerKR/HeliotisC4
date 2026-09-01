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
    /**
     * Opens one validated C4Utility runtime and records its SDK/producer versions.
     *
     * @param sdkRoot Selected installed or staged C4Utility runtime root.
     * @throws std::runtime_error When the runtime layout, process environment,
     *         C4Hdl open, or Diaphus producer identity is invalid.
     */
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

    /** Selects host-side completion while device acquisition remains Continuous. */
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
     * Applies the complete H8 surface capture defaults, including components,
     * chunks, canonical RecordingStart/FrameStart routing, encoder, scan motion,
     * processing, and illumination. Existing AcquisitionStart routing is
     * preserved.
     *
     * @param errorMessage Optional failure detail.
     * @return True when every required group succeeds; optional capability
     *         failures are treated as compatible.
     * @note The caller must establish stage clearance before invoking this method.
     *       Encoder and motion values are reset to the C4Utility h8SurfSimple
     *       defaults before StageInit. Independent groups continue after required
     *       write failures so logs expose all reachable problems. StageInit is
     *       skipped when required encoder or motion defaults are incomplete.
     */
    [[nodiscard]] bool configureH8SurfaceExample(std::string* errorMessage = nullptr);
    /** Requests cancellation of a running H8 profile or motion initialization. */
    void requestCancelInitialization() noexcept;
    /**
     * Initializes stage motion without applying the complete reference profile.
     *
     * @param errorMessage Optional failure detail.
     * @return True when StageInit completes; false when its command or completion
     *         status fails.
     * @note This operation is serialized against profile initialization and
     *       acquisition and may move the stage. StageInit always executes so the
     *       StageInitMode owns reinitialization policy. Other trigger values and
     *       all capture, processing, illumination, and user-set values are preserved.
     */
    [[nodiscard]] bool initializeMotion(std::string* errorMessage = nullptr);
    [[nodiscard]] HeliotisC4::FeatureList readFeatures(std::string* errorMessage = nullptr) const;
    [[nodiscard]] bool writeFeature(
        const std::string& name,
        const std::string& value,
        std::string* errorMessage = nullptr);
    [[nodiscard]] bool executeCommand(const std::string& name, std::string* errorMessage = nullptr);
    /**
     * Queues one logical FrameStart software request for an armed acquisition.
     *
     * @param errorMessage Optional rejection detail.
     * @return True when the command was accepted by the worker queue.
     * @note TriggerSelector is pinned to FrameStart before SDK start. Each
     *       accepted request then issues exactly one TriggerSoftware command
     *       without another selector write before waiting for its buffer.
     */
    [[nodiscard]] bool triggerSoftware(std::string* errorMessage = nullptr);
    /**
     * Starts one worker-owned acquisition cycle.
     *
     * @param mode Requested host-side SingleFrame or Continuous behavior.
     * @param frameCallback Consumer for deep-owned frames.
     * @param errorMessage Optional arm failure detail.
     * @return True after the receive worker starts C4Utility successfully.
     * @note Both modes apply device AcquisitionMode=Continuous. SingleFrame
     *       stops after the first copied buffer; Continuous stops on request.
     *       A changed prior device mode is restored after stop. The same worker
     *       thread owns C4Dev_startAcquisition, TriggerSoftware, getBuffer, and
     *       C4Dev_stopAcquisition. For software acquisition, TriggerSelector is
     *       pinned to FrameStart before SDK start, held unchanged through every
     *       TriggerSoftware/getBuffer request, then restored after SDK stop; no
     *       trigger mode/source is changed.
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
     * Deep-copies multipart data before releasing the SDK buffer.
     *
     * @param buffer Live C4Utility buffer.
     * @param detailedDiagnostics Whether to emit sampled metadata warnings.
     * @param frame Destination deep-owned frame.
     * @param errorMessage Optional copy failure detail.
     * @return True when a valid frame with one or more copied parts was copied.
     * @note The C API part count is authoritative. ChunkPartCount is reported
     *       when it disagrees but does not block copying. ChunkPartType is
     *       classification metadata; missing or unknown types remain available
     *       as raw Unknown parts. Dimensions are best-effort; when unusable,
     *       the data API size probe supplies a one-row raw preview shape.
     */
    [[nodiscard]] bool copyFrame(
        C4_BUFFER buffer,
        bool detailedDiagnostics,
        Frame* frame,
        std::string* errorMessage) const;
    /**
     * Starts, receives, and stops one acquisition on the device worker thread.
     *
     * @param device C4Utility device handle to receive from.
     * @param mode Single-frame or continuous acquisition behavior.
     * @param softwareTriggered Whether FrameStart is controlled by software.
     * @param triggerDiagnosticSummary Trigger selector state captured before arming.
     * @param acquisitionId Monotonic identifier used to correlate one arm cycle.
     * @param originalTriggerSelector Selector cursor to restore after SDK shutdown.
     * @param restoreTriggerSelector Whether the arm operation changed that cursor.
     * @param originalAcquisitionMode Device mode to restore after SDK shutdown.
     * @param restoreAcquisitionMode Whether the arm operation changed that mode.
     * @param frameCallback Callback that receives each deep-owned frame.
     * @note One worker owns the complete C4Utility acquisition lifecycle. Calls
     *       are serialized. Each queued software request follows the vendor
     *       sample by calling TriggerSoftware with the pre-start FrameStart
     *       selector unchanged immediately before one bounded getBuffer wait.
     *       No feature access occurs after SDK start and before the command.
     *       A timeout terminates the arm so a stale logical request cannot block
     *       the next acquisition.
     */
    void acquisitionLoop(
        C4_DEVICE device,
        AcquisitionMode mode,
        bool softwareTriggered,
        std::string triggerDiagnosticSummary,
        std::uint64_t acquisitionId,
        std::string originalTriggerSelector,
        bool restoreTriggerSelector,
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
    bool _acquisitionStartCompleted = false;
    bool _acquisitionStartSucceeded = false;
    std::string _acquisitionStartError;
    bool _acquisitionWorkerReleased = false;
    std::mutex _triggerMutex;
    std::condition_variable _triggerCondition;
    std::size_t _pendingSoftwareTriggers = 0;
    bool _softwareTriggerInFlight = false;
    bool _softwareTriggeredAcquisition = false;
    bool _softwareTriggerAvailable = false;
    bool _acquiring = false;
    std::thread::id _acquisitionWorkerThreadId;
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
