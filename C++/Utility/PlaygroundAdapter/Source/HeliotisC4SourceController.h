#pragma once

#include "AbstractSourceController.h"
#include "DeviceParameterControl.h"
#include "HeliotisC4System.h"
#include "ToolProvider.h"

#include <QFuture>
#include <QJsonObject>
#include <QString>

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
};

/** Adapts worker-owned Heliotis frames, status, tools and parameters into the host contracts. */
class HeliotisC4SourceController final
    : public AbstractSourceController, public IToolProvider, public IDeviceParameterControl {
    Q_OBJECT
    Q_INTERFACES(IToolProvider IDeviceParameterControl)

public:
    explicit HeliotisC4SourceController(heliotis::HeliotisC4Device* device, QObject* parent = nullptr);
    ~HeliotisC4SourceController() override;

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

    /** Declares discovery, connection and acquisition tools without SDK work. */
    QList<ToolDescriptor> toolDescriptors() const override;
    /** Routes declared tools through the session lifecycle authority. */
    ApiResult<void> requestTool(const QString& name, int version, QJsonObject arguments,
                                IToolProvider::Completion completion) override;

    DeviceParameterResult requestParameters(InventoryCompletion completion) override;
    DeviceParameterResult requestReadParameter(DeviceParameterTarget target,
                                               IDeviceParameterControl::Completion completion) override;
    DeviceParameterResult requestWriteParameter(DeviceParameterTarget target, DeviceParameterValue value,
                                                std::optional<std::uint64_t> expectedRevision,
                                                IDeviceParameterControl::Completion completion) override;
    DeviceParameterResult requestExecuteParameter(DeviceParameterTarget target,
                                                  std::optional<std::uint64_t> expectedRevision,
                                                  IDeviceParameterControl::Completion completion) override;

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

    /** Applies one controller mode and reports synchronous arm acceptance. */
    [[nodiscard]] bool startAcquisition(heliotis::HeliotisC4Device::AcquisitionMode mode);
    /** Forwards one module-owned GraphicsFrame to the session consumer. */
    void handleFrame(GraphicsFrame&& frame, unsigned int sourceIndex);
    /** Copies the connection and acquisition state on the controller thread. */
    [[nodiscard]] QJsonObject controlState() const;
    /** Completes the accepted tool call on the controller thread. */
    void finishTool(const std::shared_ptr<ToolCall>& call, const QString& name, const QString& error,
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

    heliotis::HeliotisC4Device* _device = nullptr;
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};
    std::atomic<bool> _isContinuous{false};
    std::atomic<bool> _softwareTriggerActive{false};
    std::atomic<std::uint64_t> _statusGeneration{0};
    std::atomic<std::uint64_t> _framesInCurrentAcquisition{0};
    std::atomic<std::uint64_t> _deliveredFrames{0};
    heliotis::HeliotisC4Device::CallbackId _statusCallbackId = 0;
    std::unique_ptr<heliotis::HeliotisGraphicsFrameStream> _graphicsStream;

    HeliotisC4ControlBackend* _backend = nullptr;
    std::shared_ptr<ToolCall> _toolCall;
    std::atomic<bool> _parameterBusy{false};
    QFuture<void> _parameterWork;
    mutable std::mutex _stateMutex;
    QString _connectionId;
    std::map<std::string, DeviceParameterDescriptor> _observations;
    std::uint64_t _parameterRevision = 0;
};
