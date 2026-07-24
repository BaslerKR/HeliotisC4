#pragma once

#ifdef HELIOTISC4_HAS_QT_UI

#include "HeliotisC4.h"

#include <QHash>
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
    void setDiscoveryError(const QString& message);
    void setConnectionPending(bool pending);
    void setConnectionState(bool connected);
    void setConnectionError(const QString& message);
    void setAcquisitionAvailable(bool available);
    void setAcquisitionState(bool acquiring, bool continuous);
    void setAcquisitionError(const QString& message);
    void setFeatureOperationPending(bool pending);
    void setFeatureError(const QString& message);
    void setFeatures(const heliotis::HeliotisC4::FeatureList& features);

signals:
    void refreshRequested();
    void connectRequested(int deviceIndex);
    void disconnectRequested();
    void grabOneRequested();
    void liveGrabToggled(bool enabled);
    void featureWriteRequested(const QString& featureName, const QString& value);
    void featureCommandRequested(const QString& featureName);

private:
    void populateTree(
        QTreeWidget* tree,
        QHash<QString, QTreeWidgetItem*>& categories,
        const heliotis::HeliotisC4::FeatureList& features);
    QTreeWidgetItem* ensureCategory(
        QTreeWidget* tree,
        QHash<QString, QTreeWidgetItem*>& categories,
        const QString& categoryPath);
    void setIdleState(const QString& message);
    void setSoftwareTriggerAvailable(bool available);

    QTabWidget* _tabs = nullptr;
    QComboBox* _deviceSelector = nullptr;
    QToolButton* _refreshButton = nullptr;
    QToolButton* _connectButton = nullptr;
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
    std::vector<heliotis::DeviceDescriptor> _devices;
    bool _connectionPending = false;
    bool _acquisitionAvailable = false;
    bool _acquisitionActive = false;
    bool _featureOperationPending = false;
};

#endif // HELIOTISC4_HAS_QT_UI
