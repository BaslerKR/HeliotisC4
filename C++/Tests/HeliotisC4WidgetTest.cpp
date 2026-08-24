#include "Utility/Qt/QHeliotisC4Widget.h"

#include <QApplication>
#include <QDebug>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdlib>
#include <iostream>

namespace {

heliotis::FeatureDescriptor makeFeature(
    const QString& categoryPath,
    const QString& displayName,
    const QString& value)
{
    return {
        categoryPath.toStdString(),
        displayName.toStdString(),
        value.toStdString(),
        {},
        heliotis::FeatureType::String,
        heliotis::FeatureAccess::ReadWrite,
        {},
    };
}

bool require(const bool condition, const char* message)
{
    if (!condition) {
        qCritical().noquote() << message;
        std::cerr << message << std::endl;
    }
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);

    QHeliotisC4Widget widget;
    widget.resize(460, 320);
    widget.show();

    heliotis::HeliotisC4::FeatureList features;
    features.push_back(makeFeature(QStringLiteral("Main/Child"), QStringLiteral("NestedValue"), QStringLiteral("0")));
    for (int index = 0; index < 40; ++index) {
        features.push_back(makeFeature(
            QStringLiteral("Category%1").arg(index),
            QStringLiteral("Value"),
            QString::number(index)));
    }
    features.push_back({
        "Acquisition Control",
        "TriggerSoftware",
        {},
        {},
        heliotis::FeatureType::Command,
        heliotis::FeatureAccess::WriteOnly,
        {},
    });
    features.push_back({
        "Motion Control",
        "ScanSpeed",
        "5",
        {},
        heliotis::FeatureType::Float,
        heliotis::FeatureAccess::ReadWrite,
        {},
    });

    widget.setConnectedDeviceName(QStringLiteral("Test H8"));
    widget.setFeatures(features);
    application.processEvents();

    auto* tree = widget.findChild<QTreeWidget*>(QStringLiteral("HeliotisC4FeatureTree"));
    if (!require(tree != nullptr, "Heliotis feature tree was not created.")) return EXIT_FAILURE;
    if (!require(widget.findChild<QTreeWidget*>(QStringLiteral("HeliotisC4MotionFeatureTree")) == nullptr,
                 "Heliotis must expose one feature tree instead of a separate Motion tree.")) {
        return EXIT_FAILURE;
    }
    auto* deviceRoot = tree->topLevelItem(0);
    if (!require(deviceRoot != nullptr && deviceRoot->text(0) == QStringLiteral("Test H8"),
                 "The connected Heliotis device root is missing.")) {
        return EXIT_FAILURE;
    }
    auto* mainCategory = deviceRoot->child(0);
    if (!require(deviceRoot->isExpanded(), "The connected Heliotis device root should be expanded initially.")) {
        return EXIT_FAILURE;
    }
    if (!require(mainCategory != nullptr && mainCategory->text(0) == QStringLiteral("Main")
                     && !mainCategory->isExpanded(),
                 "First-level Heliotis categories should be collapsed initially.")) {
        return EXIT_FAILURE;
    }
    auto* secondCategory = deviceRoot->child(1);
    if (!require(secondCategory != nullptr && !secondCategory->isExpanded(),
                 "All Heliotis category parents below the device root should be collapsed initially.")) {
        return EXIT_FAILURE;
    }
    const auto motionItems = tree->findItems(
        QStringLiteral("Motion Control"), Qt::MatchExactly | Qt::MatchRecursive, 0);
    if (!require(!motionItems.isEmpty(),
                 "Motion features must be present in the unified Heliotis feature tree.")) {
        return EXIT_FAILURE;
    }

    tree->setCurrentItem(deviceRoot->child(20));
    tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum() / 2);
    application.processEvents();
    auto* topBefore = tree->itemAt(tree->viewport()->rect().topLeft());
    if (!require(topBefore != nullptr, "A top-visible Heliotis item was not found.")) return EXIT_FAILURE;
    const QString topBeforeId = topBefore->data(0, Qt::UserRole).toString();
    const int topBeforeOffset = tree->visualItemRect(topBefore).top();
    const QString currentBeforeId = tree->currentItem()
        ? tree->currentItem()->data(0, Qt::UserRole).toString()
        : QString();

    auto updatedFeatures = features;
    for (auto& feature : updatedFeatures) {
        feature.valueText = "updated";
    }
    for (int index = 0; index < 12; ++index) {
        updatedFeatures.insert(
            updatedFeatures.begin() + 2 + index,
            makeFeature(
                QStringLiteral("Category0"),
                QStringLiteral("Extra%1").arg(index),
                QStringLiteral("updated")));
    }

    widget.setFeatures(updatedFeatures);
    application.processEvents();
    auto* topAfter = tree->itemAt(tree->viewport()->rect().topLeft());
    if (!require(topAfter != nullptr, "A top-visible Heliotis item was lost after refresh.")) return EXIT_FAILURE;
    if (!require(topAfter->data(0, Qt::UserRole).toString() == topBeforeId,
                 "The top-visible Heliotis item changed after a feature refresh.")) {
        return EXIT_FAILURE;
    }
    if (!require(tree->visualItemRect(topAfter).top() == topBeforeOffset,
                 "The top-visible Heliotis item offset changed after a feature refresh.")) {
        return EXIT_FAILURE;
    }
    if (!require(tree->currentItem() != nullptr
                     && tree->currentItem()->data(0, Qt::UserRole).toString() == currentBeforeId,
                 "The current Heliotis tree item changed after a feature refresh.")) {
        return EXIT_FAILURE;
    }

    auto* status = widget.findChild<QLabel*>(QStringLiteral("HeliotisC4StatusLabel"));
    auto* refreshButton = widget.findChild<QToolButton*>(QStringLiteral("HeliotisC4RefreshButton"));
    auto* connectButton = widget.findChild<QToolButton*>(QStringLiteral("HeliotisC4ConnectButton"));
    auto* grabOneButton = widget.findChild<QToolButton*>(QStringLiteral("HeliotisC4GrabOneButton"));
    auto* liveButton = widget.findChild<QToolButton*>(QStringLiteral("HeliotisC4GrabLiveButton"));
    auto* initializeButton = widget.findChild<QToolButton*>(QStringLiteral("HeliotisC4InitializeButton"));
    auto* messageLabel = widget.findChild<QLabel*>(QStringLiteral("HeliotisC4MessageLabel"));
    if (!require(status != nullptr && refreshButton != nullptr && connectButton != nullptr
                     && grabOneButton != nullptr && liveButton != nullptr && initializeButton != nullptr
                     && messageLabel != nullptr,
                 "The Heliotis connection and acquisition controls are missing.")) {
        return EXIT_FAILURE;
    }
    widget.setDiscoveredDevices({{0, 0, "TestInterface", "TestDevice"}});
    widget.setDiscoveryPending(true);
    application.processEvents();
    if (!require(!refreshButton->isEnabled() && !connectButton->isEnabled(),
                 "Background discovery must lock refresh and stale-descriptor connection controls.")) {
        return EXIT_FAILURE;
    }
    widget.setDiscoveryPending(false);
    application.processEvents();
    if (!require(refreshButton->isEnabled() && connectButton->isEnabled(),
                 "Discovery-dependent controls must recover after background discovery.")) {
        return EXIT_FAILURE;
    }
    widget.setConnectionState(true);
    widget.setAcquisitionAvailable(true);
    if (!require(initializeButton->isEnabled() && grabOneButton->isEnabled() && liveButton->isEnabled(),
                 "Connection must expose capture and explicit H8 profile initialization independently.")) {
        return EXIT_FAILURE;
    }
    if (!require(initializeButton->toolTip().contains(QStringLiteral("vendor defaults")),
                 "The Init action must identify that it resets the complete capture defaults.")) {
        return EXIT_FAILURE;
    }
    widget.setInitializationPending(true);
    application.processEvents();
    if (!require(status->property("status").toString() == QStringLiteral("connected"),
                 "H8 initialization must not replace the connected status.")) {
        return EXIT_FAILURE;
    }
    if (!require(!liveButton->isEnabled(),
                 "Acquisition must be temporarily disabled while H8 initialization is pending.")) {
        return EXIT_FAILURE;
    }
    if (!require(!initializeButton->isEnabled(),
                 "A second H8 initialization must be disabled while initialization is pending.")) {
        return EXIT_FAILURE;
    }

    widget.setInitializationError(QStringLiteral("H8 reference profile failed"));
    application.processEvents();
    if (!require(status->property("status").toString() == QStringLiteral("connected"),
                 "An H8 initialization error must not disconnect the device.")) {
        return EXIT_FAILURE;
    }
    if (!require(grabOneButton->isEnabled() && liveButton->isEnabled(),
                 "An H8 initialization failure must restore capture controls for inspection or retry.")) {
        return EXIT_FAILURE;
    }
    if (!require(initializeButton->isEnabled(),
                 "H8 initialization must remain retryable after failure.")) {
        return EXIT_FAILURE;
    }

    widget.setInitializationPending(true);
    widget.setInitializationPending(false);
    application.processEvents();
    if (!require(grabOneButton->isEnabled() && liveButton->isEnabled(),
                 "Completed H8 initialization must preserve connection-owned capture availability.")) {
        return EXIT_FAILURE;
    }

    widget.setAcquisitionState(true, false, true);
    widget.setFeatures(updatedFeatures);
    application.processEvents();
    const auto normalItems = tree->findItems(QStringLiteral("NestedValue"), Qt::MatchExactly | Qt::MatchRecursive, 0);
    const auto triggerItems = tree->findItems(QStringLiteral("TriggerSoftware"), Qt::MatchExactly | Qt::MatchRecursive, 0);
    auto* normalEditor = normalItems.isEmpty()
        ? nullptr
        : qobject_cast<QLineEdit*>(tree->itemWidget(normalItems.front(), 1));
    auto* triggerButton = triggerItems.isEmpty()
        ? nullptr
        : qobject_cast<QPushButton*>(tree->itemWidget(triggerItems.front(), 1));
    if (!require(normalEditor != nullptr && triggerButton != nullptr,
                 "Expected Heliotis feature editors were not created.")) {
        return EXIT_FAILURE;
    }
    if (!require(tree->isEnabled(),
                 "Armed acquisition must retain the unified feature tree for TriggerSoftware.")) {
        return EXIT_FAILURE;
    }
    if (!require(!normalEditor->isEnabled() && triggerButton->isEnabled(),
                 "A refresh while armed must not unlock ordinary editors or disable TriggerSoftware.")) {
        return EXIT_FAILURE;
    }

    widget.setAcquisitionState(false, false, false);
    application.processEvents();
    if (!require(normalEditor->isEnabled() && tree->isEnabled() && !triggerButton->isEnabled(),
                 "Stopping acquisition must restore writable editors and disable TriggerSoftware.")) {
        return EXIT_FAILURE;
    }
    widget.setFeatureRefreshPending(true);
    application.processEvents();
    if (!require(!tree->isEnabled()
                     && !initializeButton->isEnabled() && !liveButton->isEnabled(),
                 "An asynchronous feature refresh must lock SDK-dependent controls.")) {
        return EXIT_FAILURE;
    }
    widget.setFeatureRefreshPending(false);
    application.processEvents();
    if (!require(tree->isEnabled()
                     && initializeButton->isEnabled() && liveButton->isEnabled(),
                 "SDK-dependent controls must recover after an asynchronous feature refresh.")) {
        return EXIT_FAILURE;
    }

    // A successful feature write owns featureOperationPending across its
    // follow-up refresh. Capture must recover only after that outer operation
    // is released.
    widget.setFeatureOperationPending(true);
    application.processEvents();
    if (!require(!grabOneButton->isEnabled() && !liveButton->isEnabled(),
                 "A pending feature write must lock capture controls.")) {
        return EXIT_FAILURE;
    }
    widget.setFeatureRefreshPending(true);
    widget.setFeatureRefreshPending(false);
    widget.setFeatureOperationPending(false);
    application.processEvents();
    if (!require(grabOneButton->isEnabled() && liveButton->isEnabled(),
                 "Capture controls must recover after a feature write and its nested refresh.")) {
        return EXIT_FAILURE;
    }

    widget.setAcquisitionArmPending(true, false);
    application.processEvents();
    if (!require(!tree->isEnabled() && !initializeButton->isEnabled() && !liveButton->isEnabled(),
                 "Asynchronous acquisition arming must lock competing SDK controls.")) {
        return EXIT_FAILURE;
    }
    widget.setAcquisitionArmPending(false, false);
    application.processEvents();
    if (!require(tree->isEnabled() && initializeButton->isEnabled() && liveButton->isEnabled(),
                 "SDK controls must recover after asynchronous acquisition arming.")) {
        return EXIT_FAILURE;
    }

    // Model a SingleFrame worker that arms and finishes before the asynchronous
    // arm watcher itself is delivered to the GUI thread.
    widget.setAcquisitionArmPending(true, false);
    widget.setAcquisitionState(true, false, true);
    widget.setAcquisitionState(false, false, false);
    widget.setAcquisitionArmPending(false, false);
    application.processEvents();
    if (!require(status->property("status").toString() == QStringLiteral("connected")
                     && tree->isEnabled()
                     && !triggerButton->isEnabled() && liveButton->isEnabled(),
                  "A rapidly completed SingleFrame arm must settle in the stopped UI state.")) {
        return EXIT_FAILURE;
    }

    liveButton->setChecked(true);
    widget.setAcquisitionArmPending(true, true);
    widget.setAcquisitionArmPending(false, true);
    application.processEvents();
    if (!require(!liveButton->isChecked(),
                 "A failed Live arm must not leave the Live toggle checked.")) {
        return EXIT_FAILURE;
    }

    widget.setAcquisitionState(true, true, false);
    application.processEvents();
    if (!require(!triggerButton->isEnabled()
                     && messageLabel->text().contains(QStringLiteral("automatic or external")),
                 "Free-run or external Live must keep TriggerSoftware disabled and describe its wait path.")) {
        return EXIT_FAILURE;
    }

    // Only Continuous acquisition enables a subsequent software command after
    // the current frame; SingleFrame proceeds directly to disarm.
    widget.setAcquisitionState(true, true, true);
    widget.setSoftwareTriggerPending(true);
    widget.setFeatureOperationPending(true);
    widget.setFeatureOperationPending(false);
    application.processEvents();
    if (!require(!triggerButton->isEnabled()
                     && messageLabel->text().contains(QStringLiteral("Waiting for its Heliotis frame")),
                 "An accepted software trigger must stay disabled until its frame arrives.")) {
        return EXIT_FAILURE;
    }
    widget.setSoftwareTriggerPending(false);
    application.processEvents();
    if (!require(triggerButton->isEnabled(),
                 "A received software-triggered frame must enable the next command in an armed acquisition.")) {
        return EXIT_FAILURE;
    }

    widget.setSoftwareTriggerPending(true);
    widget.setFeatureOperationPending(true);
    widget.setSoftwareTriggerPending(false);
    widget.setFeatureOperationPending(false);
    application.processEvents();
    if (!require(triggerButton->isEnabled()
                     && messageLabel->text().contains(QStringLiteral("Live is armed"))
                     && !messageLabel->text().contains(QStringLiteral("Waiting for its Heliotis frame")),
                 "A frame that beats feature-operation completion must not leave TriggerSoftware stuck pending.")) {
        return EXIT_FAILURE;
    }

    widget.setFeatureOperationPending(true);
    widget.setAcquisitionStopPending();
    widget.setFeatureOperationPending(false);
    application.processEvents();
    if (!require(status->text() == QStringLiteral("Stopping")
                     && !grabOneButton->isEnabled() && !liveButton->isEnabled()
                     && !triggerButton->isEnabled() && !tree->isEnabled(),
                 "A non-blocking stop request must lock acquisition and feature controls until worker completion.")) {
        return EXIT_FAILURE;
    }
    widget.setAcquisitionState(false, false, false);
    application.processEvents();
    if (!require(status->property("status").toString() == QStringLiteral("connected")
                     && grabOneButton->isEnabled() && liveButton->isEnabled()
                     && tree->isEnabled(),
                 "Worker completion must recover controls from the stop-pending state.")) {
        return EXIT_FAILURE;
    }

    widget.setAcquisitionState(true, true, false);
    widget.setDisconnectionPending();
    widget.setAcquisitionError(QStringLiteral("late worker error"));
    application.processEvents();
    if (!require(!connectButton->isEnabled() && !liveButton->isEnabled()
                     && !tree->isEnabled(),
                 "A late acquisition error must not unlock controls during asynchronous disconnect.")) {
        return EXIT_FAILURE;
    }

    // Disconnect is the acquisition-presentation session boundary. A worker
    // that was still armed must not leave either capture action in Stop state.
    widget.setConnectionState(false);
    application.processEvents();
    if (!require(grabOneButton->toolTip().contains(QStringLiteral("Arm acquisition"))
                     && !liveButton->isChecked() && !grabOneButton->isEnabled()
                     && !liveButton->isEnabled() && !triggerButton->isEnabled(),
                 "Disconnect must restore idle Single/Live presentation and clear trigger state.")) {
        return EXIT_FAILURE;
    }

    // A queued inactive callback from the old worker must not relabel the
    // disconnected session as connected before the next open completes.
    const QString disconnectedStatus = status->property("status").toString();
    widget.setAcquisitionState(false, false, false);
    application.processEvents();
    if (!require(status->property("status").toString() == disconnectedStatus,
                 "A late acquisition state must not overwrite disconnected presentation.")) {
        return EXIT_FAILURE;
    }

    widget.setConnectionState(true);
    widget.setAcquisitionAvailable(true);
    application.processEvents();
    if (!require(grabOneButton->isEnabled() && liveButton->isEnabled()
                     && grabOneButton->toolTip().contains(QStringLiteral("Arm acquisition"))
                     && !liveButton->isChecked(),
                 "Reconnect must expose fresh idle Single and Live controls.")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
