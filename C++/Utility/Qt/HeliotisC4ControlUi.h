#pragma once
#include <QWidget>
#include <QString>
class QComboBox; class QToolButton; class QTreeWidget; class QStatusBar; class QLabel;
class QLineEdit; class QTabWidget; class OneGenFeatureTree;

/** @brief Owns the device control composition without SDK or acquisition dependencies. */
struct HeliotisC4ControlUi {
    /** @brief Creates the same controls used by the runtime widget. */
    void create(QWidget* owner);
    QComboBox* _deviceSelector = nullptr;
    QToolButton* _refreshButton = nullptr;
    QToolButton* _connectButton = nullptr;
    QToolButton* _initializeButton = nullptr;
    QToolButton* _grabOneButton = nullptr;
    QToolButton* _grabLiveButton = nullptr;
    QLabel* _connectionStatus = nullptr;
    QStatusBar* _statusBar = nullptr;
    QTreeWidget* _featureTree = nullptr;
};
