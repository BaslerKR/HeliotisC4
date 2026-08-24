#pragma once

#ifdef HELIOTISC4_HAS_QT_UI

#include "HeliotisC4.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QWidget>

#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QStatusBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;

class QHeliotisC4Widget final : public QWidget {
    Q_OBJECT

public:
    explicit QHeliotisC4Widget(QWidget* parent = nullptr);

    void setDiscoveredDevices(const std::vector<heliotis::DeviceDescriptor>& devices);
    /**
     * Sets the connected device label shown as the root of both feature trees.
     *
     * @param deviceName SDK-reported name of the currently connected device.
     * @note An empty name uses the module's generic Heliotis C4 label.
     */
    void setConnectedDeviceName(const QString& deviceName);
    /** Locks discovery-dependent controls while a background device scan runs. */
    void setDiscoveryPending(bool pending);
    void setDiscoveryError(const QString& message);
    void setConnectionPending(bool pending);
    /** Shows a non-blocking stop-and-disconnect operation in progress. */
    void setDisconnectionPending();
    /**
     * Applies the authoritative device connection state.
     *
     * @param connected Whether the device session owns an open SDK device.
     * @note Disconnect clears all acquisition presentation and pending-trigger
     *       state so a later connection always starts with idle Single/Live controls.
     */
    void setConnectionState(bool connected);
    void setConnectionError(const QString& message);
    /**
     * Shows that the connected device is undergoing explicit H8 profile initialization.
     *
     * @param pending Whether the initialization worker is running.
     * @note Acquisition and feature editing are temporarily unavailable while
     *       pending; their prior availability and connection state are retained.
     */
    void setInitializationPending(bool pending);
    /**
     * Reports an H8 profile failure without changing connection or capture availability.
     *
     * @param message Failure text to show in the device message area.
     * @note Capture controls remain available for inspection or retry after failure.
     */
    void setInitializationError(const QString& message);
    /**
     * Sets capture availability independently from explicit H8 initialization.
     *
     * @param available Whether a new acquisition may be armed.
     * @note Losing availability clears active-mode presentation, including the
     *       Single stop icon, Live toggle, and software-trigger pending state.
     */
    void setAcquisitionAvailable(bool available);
    /**
     * Shows asynchronous acquisition arming without blocking the Qt event loop.
     *
     * @param pending Whether the SDK arm operation is running.
     * @param continuous Whether the requested mode is continuous.
     */
    void setAcquisitionArmPending(bool pending, bool continuous);
    /** Locks trigger and capture controls while worker-owned shutdown completes. */
    void setAcquisitionStopPending();
    /** Marks whether an accepted FrameStart software command is awaiting its frame. */
    void setSoftwareTriggerPending(bool pending);
    /**
     * Applies one authoritative acquisition state without rebuilding features.
     *
     * @param acquiring Whether the receive worker is armed.
     * @param continuous Whether the active mode is continuous.
     * @param softwareTriggerAvailable Whether FrameStart accepts software input.
     */
    void setAcquisitionState(bool acquiring, bool continuous, bool softwareTriggerAvailable);
    void setAcquisitionError(const QString& message);
    /**
     * Updates controls while the SDK feature list is read off the UI thread.
     *
     * @param pending Whether an asynchronous feature refresh is running.
     */
    void setFeatureRefreshPending(bool pending);
    /** Locks competing controls and restores capture after an asynchronous feature operation and refresh. */
    void setFeatureOperationPending(bool pending);
    void setFeatureError(const QString& message);
    /** Rebuilds both trees while preserving scroll, selection, and acquisition locks. */
    void setFeatures(const heliotis::HeliotisC4::FeatureList& features);

signals:
    void refreshRequested();
    void connectRequested(int deviceIndex);
    void disconnectRequested();
    /** Requests complete H8 capture defaults and Stage Init. */
    void initializationRequested();
    void grabOneRequested();
    void stopRequested();
    void liveGrabToggled(bool enabled);
    void featureWriteRequested(const QString& featureName, const QString& value);
    void featureCommandRequested(const QString& featureName);

private:
    struct TreeState {
        bool hadItems = false;
        bool rootExpanded = true;
        QSet<QString> expandedCategories;
        QString currentItem;
        QString topVisibleItem;
        int topVisibleOffset = 0;
        int verticalScrollValue = 0;
        int horizontalScrollValue = 0;
    };

    void populateTree(
        QTreeWidget* tree,
        QHash<QString, QTreeWidgetItem*>& categories,
        const heliotis::HeliotisC4::FeatureList& features);
    TreeState captureTreeState(
        QTreeWidget* tree,
        const QHash<QString, QTreeWidgetItem*>& categories) const;
    void restoreTreeState(
        QTreeWidget* tree,
        const QHash<QString, QTreeWidgetItem*>& categories,
        const TreeState& state) const;
    QTreeWidgetItem* ensureCategory(
        QTreeWidgetItem* rootItem,
        QHash<QString, QTreeWidgetItem*>& categories,
        const QString& categoryPath);
    void setIdleState(const QString& message);
    void setSoftwareTriggerAvailable(bool available);
    /** Refreshes the operator message from the authoritative acquisition flags. */
    void updateAcquisitionMessage();

    QTabWidget* _tabs = nullptr;
    QComboBox* _deviceSelector = nullptr;
    QToolButton* _refreshButton = nullptr;
    QToolButton* _connectButton = nullptr;
    QToolButton* _initializeButton = nullptr;
    QToolButton* _grabOneButton = nullptr;
    QToolButton* _grabLiveButton = nullptr;
    QLabel* _connectionStatus = nullptr;
    QLabel* _messageLabel = nullptr;
    QStatusBar* _statusBar = nullptr;
    QTreeWidget* _deviceFeatureTree = nullptr;
    QTreeWidget* _motionFeatureTree = nullptr;
    QHash<QString, QTreeWidgetItem*> _deviceCategories;
    QHash<QString, QTreeWidgetItem*> _motionCategories;
    std::vector<QPushButton*> _softwareTriggerButtons;
    std::vector<QWidget*> _deviceFeatureEditors;
    std::vector<heliotis::DeviceDescriptor> _devices;
    QString _connectedDeviceName;
    bool _discoveryPending = false;
    bool _connected = false;
    bool _connectionPending = false;
    bool _disconnectionPending = false;
    bool _initializationPending = false;
    bool _acquisitionAvailable = false;
    bool _acquisitionActive = false;
    bool _acquisitionArmPending = false;
    bool _acquisitionStopPending = false;
    bool _softwareTriggerPending = false;
    bool _continuousAcquisition = false;
    bool _softwareTriggerAvailable = false;
    bool _featureRefreshPending = false;
    bool _featureOperationPending = false;
    bool _featureAccessCurrent = true;
};

#endif // HELIOTISC4_HAS_QT_UI
