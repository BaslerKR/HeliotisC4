#pragma once

#include "DeviceRuntime/Contract/DeviceRuntime.h"
#include "DevicePluginSession.h"
#include "HeliotisC4SourceController.h"
#include "HeliotisC4System.h"
#include "Utility/Qt/QHeliotisC4Widget.h"

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

/** Carries one background device-open result to the session thread. */
struct HeliotisConnectionResult {
    bool opened = false;
    QString openError;
    qint64 openElapsedMs = 0;
};

/** Carries one background discovery snapshot to the session thread. */
struct HeliotisDiscoveryResult {
    std::vector<heliotis::DeviceDescriptor> devices;
    QString error;
    qint64 elapsedMs = 0;
};

/** Carries background shutdown timing to the session thread. */
struct HeliotisDisconnectionResult {
    qint64 elapsedMs = 0;
};

/** Carries one explicit H8 reference-profile result while preserving connection state. */
struct HeliotisInitializationResult {
    bool initialized = false;
    QString error;
    qint64 elapsedMs = 0;
};

/** Carries one serialized feature mutation result to the session thread. */
struct HeliotisFeatureOperationResult {
    bool succeeded = false;
    QString error;
    qint64 elapsedMs = 0;
};

/** Carries one background feature-tree snapshot to the session thread. */
struct HeliotisFeatureReadResult {
    heliotis::HeliotisC4::FeatureList features;
    QString error;
    qint64 elapsedMs = 0;
};

/** Carries one background Single or Live arm result to the session thread. */
struct HeliotisAcquisitionArmResult {
    bool started = false;
    bool continuous = false;
    qint64 elapsedMs = 0;
    QString taskError;
};

/**
 * Owns one Heliotis device, controller, and asynchronous control-widget lifecycle.
 *
 * @note Device tools and the control widget share these flows. The widget is created only by
 *       `createDockWidgets`; a headless caller (a device worker or an API-only session) drives the
 *       same flows and completes them without one.
 */
class HeliotisC4PluginSession final : public QObject, public IDevicePluginSession,
                                     private HeliotisC4ControlBackend {
    Q_OBJECT
public:
    /**
     * Creates a session around independently owned system and device facades.
     *
     * @param system Shared discovery/runtime owner for the plugin instance.
     * @param device Device facade whose SDK lifetime is owned by this session.
     * @param devices Copied discovery snapshot exposed by the control widget.
     * @param settingsFile Host-owned INI for feature favorites; empty selects standalone storage.
     * @param parent Owning Qt object; the session is a QObject so background completions keep a
     *        connection context without a widget.
     */
    HeliotisC4PluginSession(
        std::shared_ptr<heliotis::HeliotisC4System> system,
        std::unique_ptr<heliotis::HeliotisC4Device> device,
        std::vector<heliotis::DeviceDescriptor> devices,
        QString settingsFile,
        QObject* parent = nullptr);
    ~HeliotisC4PluginSession() override;

    QString title() const override;
    std::vector<DevicePluginDock> createDockWidgets(QWidget* parent) override;
    AbstractSourceController* sourceController() const override;
    unsigned int capabilities() const noexcept override;
    void setTitleChangedCallback(std::function<void(const QString&)> callback) override;

private:
    using FeatureOperation = std::function<bool(heliotis::HeliotisC4Device*, std::string*)>;
    using Completion = HeliotisC4ControlBackend::Completion;

    /** Connects the session-owned controller signals once, before any widget exists. */
    void connectController();
    /** Calls one pending tool completion at most once. */
    static void finishCompletion(Completion& completion, const QString& error);
    /** Reports any GUI- or API-owned SDK operation that excludes another one. */
    [[nodiscard]] bool sdkOperationPending() const;

    // HeliotisC4ControlBackend: device tools reuse the same flows as the control widget.
    bool controlBusy() const override;
    std::vector<heliotis::DeviceDescriptor> targets() const override;
    std::optional<heliotis::DeviceDescriptor> connectedTarget() const override;
    bool discover(Completion completion) override;
    bool connectTarget(std::size_t index, Completion completion) override;
    bool disconnect(Completion completion) override;
    bool arm(bool continuous, Completion completion) override;
    bool stop(Completion completion) override;
    bool initialize(Completion completion) override;

    /** Requests a non-blocking stop that the acquisition state signal completes. */
    void requestAcquisitionStop();
    /**
     * Discovers interfaces and devices without blocking the session thread.
     *
     * @return False when discovery was not admitted; the completion is then dropped.
     */
    bool refreshDevices(Completion completion = {});
    /**
     * Opens one discovery snapshot entry off the session thread.
     *
     * @param index Index into the current discovery snapshot.
     * @param completion Optional tool completion called after the feature refresh.
     * @return False when the connection was not admitted.
     */
    bool connectDevice(int index, Completion completion = {});
    /**
     * Stops acquisition when needed, then closes the device asynchronously.
     *
     * @return False when another SDK operation excludes the disconnect.
     */
    bool requestDisconnect(Completion completion = {});
    /** Completes device shutdown away from the session thread. */
    void completeDisconnect();
    /**
     * Arms acquisition on a background task so SDK setup cannot block the session thread.
     *
     * @param continuous True for host-side Live; false for host-side Single.
     */
    bool requestAcquisitionArm(bool continuous, Completion completion = {});
    /**
     * Applies the explicit default H8 surface profile and StageInit.
     *
     * @param completion Optional caller completion; the widget path passes none.
     * @return False when the session did not admit the operation; the completion then runs never.
     * @note The control widget and `heliotis-c4.init` both enter here, so their
     *       admission, progress and failure behavior cannot diverge.
     */
    bool initializeH8ReferenceProfile(Completion completion = {});
    /**
     * Runs one serialized feature mutation away from the session thread.
     *
     * @param operation Device mutation to execute.
     * @param refreshFeaturesAfterSuccess Whether to reread feature metadata afterward.
     * @param completion Optional callback receiving the final operation result.
     * @return True when the asynchronous operation was scheduled.
     */
    bool runFeatureOperation(FeatureOperation operation, bool refreshFeaturesAfterSuccess = true,
        std::function<void(bool)> completion = {});
    /** Reads the SDK feature tree without blocking the session thread. */
    void refreshFeatureTree(std::function<void(bool)> completion = {});

    std::shared_ptr<heliotis::HeliotisC4System> _system;
    std::unique_ptr<heliotis::HeliotisC4Device> _device;
    std::vector<heliotis::DeviceDescriptor> _devices;
    QString _settingsFile;
    std::unique_ptr<HeliotisC4SourceController> _controller;
    QPointer<QHeliotisC4Widget> _widget;
    std::optional<heliotis::DeviceDescriptor> _connectedTarget;
    // Pending device-tool completions; each runs once on the session thread.
    Completion _discoveryCompletion;
    Completion _connectCompletion;
    Completion _disconnectCompletion;
    Completion _armCompletion;
    Completion _acquisitionEndCompletion;
    bool _externalBusyShown = false;
    std::unique_ptr<QFutureWatcher<HeliotisDiscoveryResult>> _discoveryWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisConnectionResult>> _connectionWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisDisconnectionResult>> _disconnectionWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisInitializationResult>> _initializationWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisFeatureOperationResult>> _featureWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisFeatureReadResult>> _featureReadWatcher;
    std::unique_ptr<QFutureWatcher<HeliotisAcquisitionArmResult>> _armWatcher;
    std::function<void(bool)> _featureRefreshCompletion;
    bool _disconnectPending = false;
    QString _title = QStringLiteral("Heliotis C4 Session");
    std::function<void(const QString&)> _titleChanged;
};
