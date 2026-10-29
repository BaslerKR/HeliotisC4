#pragma once
#include <QWidget>
#include <QString>
#include "HeliotisC4FeatureTree.h"
class QComboBox; class QToolButton; class QStatusBar; class QLabel;

/** @brief Owns the device control composition without SDK or acquisition dependencies. */
struct HeliotisC4ControlUi {
    /**
     * @brief Creates the same controls used by the runtime widget.
     * @param settingsFile Consumer favorites store; empty selects standalone storage.
     * @param favoritesKey Consumer favorites key; empty selects the standalone key.
     */
    void create(QWidget* owner, const QString& settingsFile = {}, const QString& favoritesKey = {});
    /** @brief Synchronizes the next command after observed or local acquisition state changes. */
    void updateLiveAction();
    QComboBox* _deviceSelector = nullptr;
    QToolButton* _refreshButton = nullptr;
    QToolButton* _connectButton = nullptr;
    QToolButton* _initializeButton = nullptr;
    QToolButton* _grabOneButton = nullptr;
    QToolButton* _grabLiveButton = nullptr;
    QLabel* _connectionStatus = nullptr;
    QStatusBar* _statusBar = nullptr;
    HeliotisC4FeatureTree* _featureTree = nullptr;
};
