#pragma once

#include "DeviceRuntime/Contract/AbstractSourceController.h"
#include "DeviceRuntime/Contract/DeviceConnectionControl.h"
#include "DeviceRuntime/Contract/DeviceImageDiagnostics.h"
#include "DeviceRuntime/Contract/DeviceParameterControl.h"
#include "DeviceRuntime/Contract/FrameContextClient.h"
#include "HeliotisC4System.h"
#include "Tools/ToolProvider.h"

#include <QFuture>
#include <QString>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace heliotis {
class HeliotisGraphicsFrameStream;
struct HeliotisRangeRejection;
struct HeliotisImageRejection;
}

/**
 * Session-owned lifecycle authority that device tools reuse.
 *
 * @note Every member runs on the controller thread. An operation returns
 *       false when it is not admitted; then it never calls its completion.
 *       An admitted operation calls its completion exactly once on the
 *       controller thread, with an empty message on success.
 */
class HeliotisC4ControlBackend {
public:
    using Completion = std::function<void(const QString& error)>;
    virtual ~HeliotisC4ControlBackend() = default;

    /** Reports GUI-owned SDK work that excludes another lifecycle or feature operation. */
    [[nodiscard]] virtual bool controlBusy() const = 0;
    /** Copies the current discovery snapshot. */
    [[nodiscard]] virtual std::vector<heliotis::DeviceDescriptor> targets() const = 0;
    /** Returns the discovery identity of the open device. */
    [[nodiscard]] virtual std::optional<heliotis::DeviceDescriptor> connectedTarget() const = 0;
    /** Refreshes the discovery snapshot while disconnected. */
    virtual bool discover(Completion completion) = 0;
    /** Opens one exact snapshot entry and refreshes features. */
    virtual bool connectTarget(std::size_t index, Completion completion) = 0;
    /** Stops acquisition when needed and closes the device. */
    virtual bool disconnect(Completion completion) = 0;
    /**
     * Arms acquisition through the same path as the Single and Live controls.
     *
     * @note Live completes when arming finishes. Single completes when the
     *       acquisition that it armed has stopped.
     */
    virtual bool arm(bool continuous, Completion completion) = 0;
    /** Requests a non-blocking stop and completes when acquisition has stopped. */
    virtual bool stop(Completion completion) = 0;
    /**
     * Applies the complete H8 reference profile and runs StageInit.
     *
     * @param completion Receives the terminal result or cancellation error.
     * @param cancelled Optional predicate for a cancellation requested before
     *        the worker enters the device profile; it must remain valid until
     *        completion.
     * @note This is the one path used by the declared `heliotis-c4.init` tool.
     *       The caller confirms physical stage clearance before invoking it.
     */
    virtual bool initialize(Completion completion, std::function<bool()> cancelled = {}) = 0;
};

/** Adapts worker-owned Heliotis frames, status, tools and parameters into the host contracts. */
class HeliotisC4SourceController final
    : public AbstractSourceController, public IDeviceConnectionControl, public IDeviceParameterControl,
      public IFrameContextClient, public IToolProvider, public IToolExecution {
    Q_OBJECT
    Q_INTERFACES(IDeviceConnectionControl IDeviceParameterControl IFrameContextClient IToolProvider IToolExecution)

public:
    void setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source) override;
    using TargetsCompletion = IDeviceConnectionControl::TargetsCompletion;
    /**
     * @brief Creates the controller with the runtime owner's optional rejection sinks.
     * @note The sinks are borrowed for the controller lifetime and must outlive it;
     *       the worker clears them before runtime shutdown.
     */
    explicit HeliotisC4SourceController(heliotis::HeliotisC4Device* device, QObject* parent = nullptr,
        DeviceRangeRejectionSink rangeRejectionSink = nullptr,
        DeviceImageRejectionSink imageRejectionSink = nullptr);
    ~HeliotisC4SourceController() override;

    /** @brief Copies per-run counters shared with the control panel. */
    std::optional<SourceRuntimeDiagnostics> sourceRuntimeDiagnostics(unsigned sourceIndex) const override;
    /** Arms continuous acquisition through the common controller contract. */
    void start() override;
    /** Arms continuous acquisition and reports whether the SDK accepted it. */
    [[nodiscard]] bool startLive();
    /** Requests non-blocking worker shutdown for interactive UI controls. */
    void requestStop();
    /** Stops and joins acquisition for session teardown. */
    void stop() override;
    [[nodiscard]] bool isGrabbing() const override;
    [[nodiscard]] bool grabOne();
    void setFrameConsumer(FrameConsumer consumer) override;
    [[nodiscard]] bool supports3D() const override;

    /**
     * Installs the session lifecycle authority used by device tools.
     *
     * @note The session clears it before teardown; pending tool calls then
     *       complete with a failure.
     */
    void setControlBackend(HeliotisC4ControlBackend* backend);
    /** Reports accepted API work that excludes GUI lifecycle and feature operations (controller thread). */
    [[nodiscard]] bool externalBusy() const noexcept;
    /** Reports an accepted API parameter call that holds the SDK feature channel. */
    [[nodiscard]] bool parameterWorkActive() const noexcept { return _parameterBusy.load(); }
    /** Returns the current connection generation, empty while disconnected. */
    [[nodiscard]] QString connectionId() const;

    /** Declares every lifecycle operation while the session lifecycle authority is bound. */
    DeviceConnectionCapabilities connectionCapabilities() const override;
    /** Copies the exact C4Utility interface/device discovery snapshot. */
    DeviceConnectionResult requestConnectionTargets(TargetsCompletion completion) override;
    /** Observes the module-owned connection and delivered-frame count. */
    DeviceConnectionResult requestConnectionState(IDeviceConnectionControl::Completion completion) override;
    /** Refreshes C4Utility discovery while disconnected and idle. */
    DeviceConnectionResult requestDiscover(TargetsCompletion completion) override;
    /** Opens an exact discovered target; the device configuration is preserved and Init stays explicit. */
    DeviceConnectionResult requestConnect(std::string targetId, IDeviceConnectionControl::Completion completion) override;
    /** Stops acquisition when needed and closes the exact connection. */
    DeviceConnectionResult requestDisconnect(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
    /** Arms Single acquisition and completes after it stops with a delivered frame. */
    DeviceConnectionResult requestSingle(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
    /** Arms Live acquisition; completion is arm acknowledgement. */
    DeviceConnectionResult requestLiveStart(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
    /** Stops acquisition and completes after it has stopped. */
    DeviceConnectionResult requestLiveStop(std::string connectionId, IDeviceConnectionControl::Completion completion) override;

    DeviceParameterResult requestParameters(InventoryCompletion completion) override;
    DeviceParameterResult requestReadParameter(DeviceParameterTarget target,
                                               IDeviceParameterControl::Completion completion) override;
    DeviceParameterResult requestWriteParameter(DeviceParameterTarget target, DeviceParameterValue value,
                                                std::optional<std::uint64_t> expectedRevision,
                                                IDeviceParameterControl::Completion completion) override;
    DeviceParameterResult requestExecuteParameter(DeviceParameterTarget target,
                                                  std::optional<std::uint64_t> expectedRevision,
                                                  IDeviceParameterControl::Completion completion) override;
    /** Subscribes to revision-stamped descriptor snapshots and deltas for one connection. */
    DeviceParameterResult requestParameterChanges(std::string connectionId,
        std::string subscriptionId, ChangeListener delivery) override;
    /** Stops one subscription and completes after its queued delivery is discarded. */
    DeviceParameterResult releaseParameterChanges(std::string connectionId,
        std::string subscriptionId, IDeviceParameterControl::Completion completion) override;

    /** Declares the module-init action alongside the host lifecycle tools. */
    QList<ToolDescriptor> toolDescriptors() const override;
    ApiResult<void> requestTool(const QString& name, int version, QJsonObject arguments,
                                IToolProvider::Completion completion) override;
    ApiResult<void> requestToolWithProgress(const QString& callId, const QString& name, int version,
                                            QJsonObject arguments, IToolExecution::Progress progress,
                                            IToolProvider::Completion completion) override;
    ApiResult<void> cancelTool(const QString& callId) override;

    /**
     * Publishes the observed feature tree after a connect, write, stop or refresh.
     *
     * @param features Complete SDK feature snapshot already read off the GUI thread.
     * @note Active subscriptions receive one bounded delta change; a delta that
     *       cannot fit or that overflows the bounded queue becomes an explicit
     *       resync snapshot instead of a silent gap.
     */
    void publishParameterSnapshot(const heliotis::HeliotisC4::FeatureList& features);
    /** Reports whether any parameter change subscription currently exists. */
    [[nodiscard]] bool parameterWatchesActive() const;

signals:
    /** Reports that one continuous SDK frame was received and the next software trigger may be queued. */
    void acquisitionFrameReceived();
    /** Reports worker arm/disarm state and whether FrameStart software control is available. */
    void acquisitionStateChanged(bool acquiring, bool continuous, bool softwareTriggerAvailable);
    /** Reports an acquisition failure that is safe to present to the operator. */
    void acquisitionError(const QString& message);
    /** Reports connection or API admission changes; consumer views refresh on their owning thread. */
    void controlChanged();
    /** Reports an applied API parameter change whose feature view is now stale. */
    void parametersApplied();

private:
    struct ToolCall;
    struct InitToolCall;
    struct ParameterWatch;

    /** Applies one controller mode and reports synchronous arm acceptance. */
    [[nodiscard]] bool startAcquisition(heliotis::HeliotisC4Device::AcquisitionMode mode);
    /** Forwards one module-owned GraphicsFrame to the session consumer. */
    void handleFrame(GraphicsFrame&& frame, unsigned int sourceIndex);
    /** Publishes one observed range rejection with the fixed host enum and numeric fields. */
    void reportRangeRejection(const heliotis::HeliotisRangeRejection& rejection) noexcept;
    /** Publishes one observed integer image rejection with the fixed host enum and numeric fields. */
    void reportImageRejection(const heliotis::HeliotisImageRejection& rejection) noexcept;
    /** Clears per-acquisition counts and arms unthrottled Single reporting. */
    void resetRejectionCounters(bool single) noexcept;
    enum class Lifecycle { Targets, Connection, Discover, Connect, Disconnect, Single, LiveStart, LiveStop };
    /** Copies the connection and acquisition state on the controller thread. */
    [[nodiscard]] DeviceConnectionState connectionState() const;
    /** Applies the shared GUI/API admission rules and starts one lifecycle operation. */
    DeviceConnectionResult admitLifecycle(Lifecycle operation, const std::string& argument, std::shared_ptr<ToolCall> call);
    /** Completes the accepted lifecycle call on the controller thread. */
    void finishTool(const std::shared_ptr<ToolCall>& call, Lifecycle operation, const QString& error,
                    std::uint64_t framesBefore);
    /** Applies the shared GUI/API admission rules for parameter calls. */
    [[nodiscard]] DeviceParameterResult parameterAdmission(bool softwareTrigger);
    /** Runs one exact read, write or command on a module worker. */
    DeviceParameterResult requestParameter(DeviceParameterTarget target,
                                           std::optional<DeviceParameterValue> value,
                                           std::optional<std::uint64_t> expectedRevision, bool mutate,
                                           IDeviceParameterControl::Completion completion);
    /** Converts one feature snapshot entry and assigns its observation revision. */
    [[nodiscard]] std::optional<DeviceParameterDescriptor> observeParameter(
        const heliotis::FeatureDescriptor& feature, const std::string& connection);
    /** Returns the admission code and reason when the module init action is unavailable. */
    [[nodiscard]] std::optional<std::pair<ApiErrorCode, QString>> initUnavailable() const;
    /** Builds the declared init descriptor with its current availability. */
    [[nodiscard]] ToolDescriptor initToolDescriptor() const;
    /** Completes one accepted init call exactly once from the session result. */
    void finishInitTool(const std::shared_ptr<InitToolCall>& call, const QString& error);
    /** Queues one change for a watch and schedules the bounded drain. */
    void enqueueParameterChange(const std::shared_ptr<ParameterWatch>& watch,
                                DeviceParameterChange change, std::string overflowReason);
    /** Schedules one coalesced delivery pass on the controller thread. */
    void scheduleParameterDrain();
    /** Delivers queued changes; the listener may run on this thread only. */
    void drainParameterChanges();
    /** Ends every subscription with one terminal error and drops its queue. */
    void terminateParameterWatches(const QString& message);

    heliotis::HeliotisC4Device* _device = nullptr;
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};
    std::atomic<std::uint64_t> _diagnosticDelivered{0};
    std::atomic<bool> _isContinuous{false};
    std::atomic<bool> _softwareTriggerActive{false};
    std::atomic<std::uint64_t> _statusGeneration{0};
    std::atomic<std::uint64_t> _framesInCurrentAcquisition{0};
    std::atomic<std::uint64_t> _deliveredFrames{0};
    heliotis::HeliotisC4Device::CallbackId _statusCallbackId = 0;
    std::unique_ptr<heliotis::HeliotisGraphicsFrameStream> _graphicsStream;
    /** Borrowed runtime-owner sinks; null when the Host does not expose the capability. */
    const DeviceRangeRejectionSink _rangeRejectionSink;
    const DeviceImageRejectionSink _imageRejectionSink;
    std::array<std::atomic<std::uint64_t>,
        static_cast<std::size_t>(DeviceRangeRejection::StorageRefused) + 1> _rangeRejections{};
    std::array<std::atomic<std::uint64_t>,
        static_cast<std::size_t>(DeviceImageRejection::NoImage) + 1> _imageRejections{};
    /** Single reports every failure; Live reports the first and every 100th per reason. */
    std::atomic<bool> _singleInFlight{false};

    HeliotisC4ControlBackend* _backend = nullptr;
    std::shared_ptr<ToolCall> _toolCall;
    std::shared_ptr<InitToolCall> _initToolCall;
    std::atomic<bool> _parameterBusy{false};
    QFuture<void> _parameterWork;
    mutable std::mutex _stateMutex;
    QString _connectionId;
    std::map<std::string, DeviceParameterDescriptor> _observations;
    std::uint64_t _parameterRevision = 0;
    mutable std::mutex _watchMutex;
    std::map<std::string, std::shared_ptr<ParameterWatch>> _parameterWatches;
    bool _parameterDrainScheduled = false;
};
