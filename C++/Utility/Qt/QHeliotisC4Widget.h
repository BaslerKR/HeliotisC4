#pragma once

#ifdef HELIOTISC4_HAS_QT_UI

#include "HeliotisC4.h"

#include <QHash>
#include <QString>
#include <QWidget>

#include <vector>

class QComboBox;
class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;

class QHeliotisC4Widget final : public QWidget {
    Q_OBJECT

public:
    explicit QHeliotisC4Widget(QWidget* parent = nullptr);

    void setDiscoveredDevices(const std::vector<heliotis::DeviceDescriptor>& devices);
    void setConnectionState(bool connected);
    void setFeatures(const heliotis::HeliotisC4::FeatureList& features);

signals:
    void refreshRequested();
    void connectRequested(int deviceIndex);
    void disconnectRequested();

private:
    void populateTree(
        QTreeWidget* tree,
        QHash<QString, QTreeWidgetItem*>& categories,
        const heliotis::HeliotisC4::FeatureList& features);
    QTreeWidgetItem* ensureCategory(
        QTreeWidget* tree,
        QHash<QString, QTreeWidgetItem*>& categories,
        const QString& categoryPath);

    QTabWidget* _tabs = nullptr;
    QComboBox* _deviceSelector = nullptr;
    QToolButton* _refreshButton = nullptr;
    QToolButton* _connectButton = nullptr;
    QLabel* _connectionStatus = nullptr;
    QTreeWidget* _deviceFeatureTree = nullptr;
    QTreeWidget* _motionFeatureTree = nullptr;
    QHash<QString, QTreeWidgetItem*> _deviceCategories;
    QHash<QString, QTreeWidgetItem*> _motionCategories;
    std::vector<heliotis::DeviceDescriptor> _devices;
};

#endif // HELIOTISC4_HAS_QT_UI
