#include "QHeliotisC4Widget.h"

#ifdef HELIOTISC4_HAS_QT_UI

#include <QAbstractItemView>
#include <QComboBox>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSize>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {

constexpr int featureTreeIdRole = Qt::UserRole;
constexpr auto featureWritableProperty = "heliotisFeatureWritable";

QString categoryItemId(const QString& categoryPath)
{
    return QStringLiteral("category:") + categoryPath;
}

QString featureItemId(const heliotis::FeatureDescriptor& feature)
{
    return QStringLiteral("feature:")
        + QString::fromStdString(feature.categoryPath)
        + QStringLiteral("/")
        + QString::fromStdString(feature.displayName);
}

QString itemId(const QTreeWidgetItem* item)
{
    return item ? item->data(0, featureTreeIdRole).toString() : QString();
}

void collectTreeItems(
    QTreeWidgetItem* item,
    QHash<QString, QTreeWidgetItem*>& items)
{
    if (!item) return;

    const QString id = itemId(item);
    if (!id.isEmpty()) items.insert(id, item);
    for (int index = 0; index < item->childCount(); ++index) {
        collectTreeItems(item->child(index), items);
    }
}

QTreeWidget* createFeatureTree(QWidget* parent, const QString& objectName)
{
    auto* tree = new QTreeWidget(parent);
    tree->setObjectName(objectName);
    tree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    tree->setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Value")});
    return tree;
}

} // namespace

QHeliotisC4Widget::QHeliotisC4Widget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(300, 350);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setObjectName(QStringLiteral("DeviceRootLayout"));

    auto* topLayout = new QHBoxLayout;
    topLayout->setObjectName(QStringLiteral("DeviceTopBarLayout"));

    _deviceSelector = new QComboBox(this);
    _deviceSelector->setObjectName(QStringLiteral("HeliotisC4DeviceSelector"));
    _deviceSelector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _refreshButton = new QToolButton(this);
    _refreshButton->setObjectName(QStringLiteral("HeliotisC4RefreshButton"));
    _refreshButton->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-refresh-48.png")));
    _refreshButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _refreshButton->setIconSize(QSize(16, 16));
    _refreshButton->setToolTip(tr("Refresh Heliotis devices"));

    _connectButton = new QToolButton(this);
    _connectButton->setObjectName(QStringLiteral("HeliotisC4ConnectButton"));
    _connectButton->setCheckable(true);
    _connectButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _connectButton->setIconSize(QSize(16, 16));
    _connectButton->setToolTip(tr("Connect the selected device"));
    QIcon connectIcon;
    connectIcon.addFile(QStringLiteral(":/Resources/Icons/icons8-connect-48.png"), QSize(), QIcon::Normal, QIcon::Off);
    connectIcon.addFile(QStringLiteral(":/Resources/Icons/icons8-disconnected-48.png"), QSize(), QIcon::Normal, QIcon::On);
    _connectButton->setIcon(connectIcon);
    _connectButton->setEnabled(false);

    _initializeButton = new QToolButton(this);
    _initializeButton->setObjectName(QStringLiteral("HeliotisC4InitializeButton"));
    _initializeButton->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-setup-48.png")));
    _initializeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _initializeButton->setIconSize(QSize(16, 16));
    _initializeButton->setToolTip(tr("Reset all H8 capture settings to the vendor defaults and initialize the stage"));
    _initializeButton->setEnabled(false);

    _grabOneButton = new QToolButton(this);
    _grabOneButton->setObjectName(QStringLiteral("HeliotisC4GrabOneButton"));
    _grabOneButton->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-camera-48.png")));
    _grabOneButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _grabOneButton->setIconSize(QSize(16, 16));
    _grabOneButton->setToolTip(tr("Arm acquisition for one Heliotis frame"));
    _grabOneButton->setEnabled(false);

    _grabLiveButton = new QToolButton(this);
    _grabLiveButton->setObjectName(QStringLiteral("HeliotisC4GrabLiveButton"));
    _grabLiveButton->setCheckable(true);
    _grabLiveButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _grabLiveButton->setIconSize(QSize(16, 16));
    _grabLiveButton->setToolTip(tr("Start or stop live Heliotis acquisition"));
    QIcon liveIcon;
    liveIcon.addFile(QStringLiteral(":/Resources/Icons/icons8-cameras-48.png"), QSize(), QIcon::Normal, QIcon::Off);
    liveIcon.addFile(QStringLiteral(":/Resources/Icons/icons8-pause-48.png"), QSize(), QIcon::Normal, QIcon::On);
    _grabLiveButton->setIcon(liveIcon);
    _grabLiveButton->setEnabled(false);

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("DeviceSelectorLayout"));
    selectorLayout->addWidget(_deviceSelector);
    selectorLayout->addWidget(_refreshButton);

    auto* toolLayout = new QHBoxLayout;
    toolLayout->setObjectName(QStringLiteral("DeviceToolLayout"));
    toolLayout->addWidget(_connectButton);
    toolLayout->addWidget(_initializeButton);
    toolLayout->addWidget(_grabOneButton);
    toolLayout->addWidget(_grabLiveButton);

    topLayout->addLayout(selectorLayout);
    topLayout->addLayout(toolLayout);
    rootLayout->addLayout(topLayout);

    _tabs = new QTabWidget(this);
    _tabs->setObjectName(QStringLiteral("HeliotisC4ControlTabs"));

    auto* devicePage = new QWidget(_tabs);
    auto* deviceLayout = new QVBoxLayout(devicePage);
    deviceLayout->setObjectName(QStringLiteral("DeviceTabbedTreePanelLayout"));
    _deviceFeatureTree = createFeatureTree(devicePage, QStringLiteral("HeliotisC4DeviceFeatureTree"));
    deviceLayout->addWidget(_deviceFeatureTree);
    _tabs->addTab(devicePage, tr("Device"));

    auto* motionPage = new QWidget(_tabs);
    auto* motionLayout = new QVBoxLayout(motionPage);
    motionLayout->setObjectName(QStringLiteral("DeviceTabbedTreePanelLayout"));
    _motionFeatureTree = createFeatureTree(motionPage, QStringLiteral("HeliotisC4MotionFeatureTree"));
    motionLayout->addWidget(_motionFeatureTree);
    _tabs->addTab(motionPage, tr("Motion"));

    rootLayout->addWidget(_tabs);

    _statusBar = new QStatusBar(this);
    _statusBar->setObjectName(QStringLiteral("HeliotisC4StatusBar"));
    _statusBar->setSizeGripEnabled(false);
    _connectionStatus = new QLabel(this);
    _connectionStatus->setObjectName(QStringLiteral("HeliotisC4StatusLabel"));
    _connectionStatus->setAlignment(Qt::AlignCenter);
    _connectionStatus->setProperty("status", QStringLiteral("disconnected"));
    _statusBar->addWidget(_connectionStatus);
    _messageLabel = new QLabel(this);
    _messageLabel->setObjectName(QStringLiteral("HeliotisC4MessageLabel"));
    _messageLabel->setProperty("statusRole", QStringLiteral("message"));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _statusBar->addWidget(_messageLabel, 1);
    rootLayout->addWidget(_statusBar);

    connect(_refreshButton, &QToolButton::clicked, this, [this] {
        _refreshButton->setEnabled(false);
        _connectionStatus->setText(tr("Searching"));
        _connectionStatus->setProperty("status", QStringLiteral("idle"));
        _connectionStatus->style()->unpolish(_connectionStatus);
        _connectionStatus->style()->polish(_connectionStatus);
        _messageLabel->setText(tr("Searching for Heliotis devices..."));
        QTimer::singleShot(0, this, &QHeliotisC4Widget::refreshRequested);
    });
    connect(_connectButton, &QToolButton::toggled, this, [this](const bool connected) {
        if (!connected) {
            emit disconnectRequested();
            return;
        }
        const int index = _deviceSelector->currentData().toInt();
        if (index >= 0 && index < static_cast<int>(_devices.size())) {
            setConnectionPending(true);
            emit connectRequested(index);
            return;
        }
        setConnectionError(tr("Select a discovered Heliotis device before connecting."));
    });
    connect(_grabOneButton, &QToolButton::clicked, this, [this] {
        if (_acquisitionActive) {
            emit stopRequested();
            return;
        }
        emit grabOneRequested();
    });
    connect(_initializeButton, &QToolButton::clicked, this, [this] {
        if (_connected && !_acquisitionActive && !_initializationPending && !_featureOperationPending) {
            emit initializationRequested();
        }
    });
    connect(_grabLiveButton, &QToolButton::toggled, this, &QHeliotisC4Widget::liveGrabToggled);
    setConnectionState(false);
}

void QHeliotisC4Widget::setDiscoveredDevices(const std::vector<heliotis::DeviceDescriptor>& devices)
{
    _devices = devices;
    const QSignalBlocker blocker(_deviceSelector);
    _deviceSelector->clear();
    for (int index = 0; index < static_cast<int>(_devices.size()); ++index) {
        const auto& device = _devices[static_cast<std::size_t>(index)];
        QString name = QString::fromStdString(device.deviceName);
        QString interfaceName = QString::fromStdString(device.interfaceName);
        if (!interfaceName.isEmpty()) {
            name += QStringLiteral(" / ") + interfaceName;
            interfaceName.clear();
        }
        _deviceSelector->addItem(
            interfaceName.isEmpty() ? name : QStringLiteral("%1 — %2").arg(name, interfaceName),
            index);
    }
    _deviceSelector->setEnabled(!_connected && !_discoveryPending && !_devices.empty());
    _connectButton->setEnabled(!_connected && !_discoveryPending && !_devices.empty()
        && !_connectButton->isChecked() && !_connectionPending);
    _refreshButton->setEnabled(!_connected && !_discoveryPending);
    if (!_connectButton->isChecked()) {
        setIdleState(_devices.empty()
        ? tr("No Heliotis devices discovered.")
        : tr("%n device(s) discovered.", nullptr, static_cast<int>(_devices.size())));
    }
}

void QHeliotisC4Widget::setDiscoveryPending(const bool pending)
{
    _discoveryPending = pending;
    if (pending) {
        _deviceSelector->setEnabled(false);
        _connectButton->setEnabled(false);
        _refreshButton->setEnabled(false);
        _connectionStatus->setText(tr("Searching"));
        _connectionStatus->setProperty("status", QStringLiteral("idle"));
        _connectionStatus->style()->unpolish(_connectionStatus);
        _connectionStatus->style()->polish(_connectionStatus);
        _messageLabel->setText(tr("Searching for Heliotis devices..."));
        _messageLabel->setProperty("messageState", QStringLiteral("normal"));
        _messageLabel->style()->unpolish(_messageLabel);
        _messageLabel->style()->polish(_messageLabel);
        return;
    }

    _deviceSelector->setEnabled(!_connected && !_devices.empty());
    _connectButton->setEnabled(!_connected && !_devices.empty()
        && !_connectButton->isChecked() && !_connectionPending);
    _refreshButton->setEnabled(!_connected);
}

void QHeliotisC4Widget::setDiscoveryError(const QString& message)
{
    setDiscoveryPending(false);
    setIdleState(message);
    _messageLabel->setProperty("messageState", QStringLiteral("error"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setConnectionState(const bool connected)
{
    const bool wasConnected = _connected;
    _connected = connected;
    _connectionPending = false;
    _disconnectionPending = false;
    _acquisitionArmPending = false;
    _acquisitionStopPending = false;
    _softwareTriggerPending = false;
    if (!connected) {
        _initializationPending = false;
        _featureRefreshPending = false;
        _featureOperationPending = false;
        _featureAccessCurrent = true;
        _tabs->setEnabled(true);
    }

    const QSignalBlocker blocker(_connectButton);
    _connectButton->setChecked(connected);
    _connectButton->setToolTip(connected
        ? tr("Disconnect the selected device")
        : tr("Connect the selected device"));

    if (!connected && !wasConnected) {
        setIdleState(_devices.empty()
            ? tr("Ready to search for Heliotis devices.")
            : tr("Select a Heliotis device to connect."));
        _connectButton->setEnabled(!_devices.empty());
        _deviceSelector->setEnabled(!_devices.empty());
        _refreshButton->setEnabled(true);
        _initializeButton->setEnabled(false);
        _deviceFeatureTree->setEnabled(false);
        _motionFeatureTree->setEnabled(false);
        setAcquisitionAvailable(false);
        return;
    }
    _connectionStatus->setText(connected ? tr("Connected") : tr("Disconnected"));
    _connectionStatus->setProperty("status", connected ? QStringLiteral("connected") : QStringLiteral("disconnected"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);

    _connectButton->setEnabled(connected || !_devices.empty());
    _deviceSelector->setEnabled(!connected && !_devices.empty());
    _refreshButton->setEnabled(!connected);
    _initializeButton->setEnabled(connected && !_initializationPending
        && !_acquisitionActive && !_featureOperationPending && !_featureRefreshPending
        && !_acquisitionArmPending);
    _deviceFeatureTree->setEnabled(connected && !_featureOperationPending
        && !_featureRefreshPending && !_acquisitionArmPending);
    _motionFeatureTree->setEnabled(connected && !_acquisitionActive
        && !_featureOperationPending && !_featureRefreshPending && !_acquisitionArmPending);
    if (!connected) setAcquisitionAvailable(false);
    _messageLabel->setText(connected
        ? tr("Heliotis device connected.")
        : tr("Heliotis device disconnected."));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setDisconnectionPending()
{
    _disconnectionPending = true;
    _connectButton->setEnabled(false);
    _initializeButton->setEnabled(false);
    _grabOneButton->setEnabled(false);
    _grabLiveButton->setEnabled(false);
    _tabs->setEnabled(false);
    _connectionStatus->setText(tr("Stopping"));
    _connectionStatus->setProperty("status", QStringLiteral("idle"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);
    _messageLabel->setText(tr("Stopping acquisition before disconnecting the Heliotis device..."));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setInitializationPending(const bool pending)
{
    _initializationPending = pending;
    if (pending) {
        _tabs->setEnabled(false);
        _connectButton->setEnabled(false);
        _initializeButton->setEnabled(false);
        setAcquisitionAvailable(_acquisitionAvailable);
        _messageLabel->setText(tr("Applying all H8 capture defaults and initializing stage motion; the stage may move..."));
        _messageLabel->setProperty("messageState", QStringLiteral("normal"));
        _messageLabel->style()->unpolish(_messageLabel);
        _messageLabel->style()->polish(_messageLabel);
        return;
    }

    _tabs->setEnabled(!_acquisitionArmPending && !_featureRefreshPending
        && !_featureOperationPending);
    _connectButton->setEnabled(!_acquisitionArmPending
        && (_connectButton->isChecked() || !_devices.empty()));
    _initializeButton->setEnabled(_connected && !_acquisitionActive
        && !_featureOperationPending && !_featureRefreshPending && !_acquisitionArmPending);
    setAcquisitionAvailable(_acquisitionAvailable);
    _messageLabel->setText(tr("Heliotis H8 capture defaults and Stage Init completed."));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setInitializationError(const QString& message)
{
    _initializationPending = false;
    _tabs->setEnabled(!_acquisitionArmPending && !_featureRefreshPending
        && !_featureOperationPending);
    _connectButton->setEnabled(!_acquisitionArmPending
        && (_connectButton->isChecked() || !_devices.empty()));
    _initializeButton->setEnabled(_connected && !_acquisitionActive
        && !_featureOperationPending && !_featureRefreshPending && !_acquisitionArmPending);
    setAcquisitionAvailable(_acquisitionAvailable);
    _messageLabel->setText(message);
    _messageLabel->setProperty("messageState", QStringLiteral("error"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setConnectionPending(const bool pending)
{
    _connectionPending = pending;
    if (!pending) return;

    _connectionStatus->setText(tr("Connecting"));
    _connectionStatus->setProperty("status", QStringLiteral("idle"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);
    _connectButton->setEnabled(false);
    _initializeButton->setEnabled(false);
    _deviceSelector->setEnabled(false);
    _refreshButton->setEnabled(false);
    setAcquisitionAvailable(false);
    _messageLabel->setText(tr("Connecting to the selected Heliotis device..."));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setConnectionError(const QString& message)
{
    setConnectionState(false);
    _messageLabel->setText(message);
    _messageLabel->setProperty("messageState", QStringLiteral("error"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setAcquisitionAvailable(const bool available)
{
    _acquisitionAvailable = available;
    if (!available) {
        _acquisitionActive = false;
        _continuousAcquisition = false;
        _softwareTriggerAvailable = false;
        _softwareTriggerPending = false;
        {
            const QSignalBlocker blocker(_grabLiveButton);
            _grabLiveButton->setChecked(false);
        }
        _grabOneButton->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-camera-48.png")));
        _grabOneButton->setToolTip(tr("Arm acquisition for one Heliotis frame"));
    }
    const QString status = _connectionStatus->property("status").toString();
    const bool enabled = (status == QStringLiteral("connected") || status == QStringLiteral("grabbing"))
        && available && !_initializationPending && !_featureRefreshPending
        && !_featureOperationPending && !_acquisitionArmPending && !_acquisitionStopPending;
    _grabOneButton->setEnabled(enabled && !_grabLiveButton->isChecked());
    _grabLiveButton->setEnabled(enabled);
    if (!enabled) {
        const QSignalBlocker blocker(_grabLiveButton);
        _grabLiveButton->setChecked(false);
    }
    setSoftwareTriggerAvailable(_acquisitionActive && enabled && _softwareTriggerAvailable);
}

void QHeliotisC4Widget::setAcquisitionArmPending(const bool pending, const bool continuous)
{
    _acquisitionArmPending = pending;
    if (pending) {
        _continuousAcquisition = continuous;
        _connectButton->setEnabled(false);
        _initializeButton->setEnabled(false);
        _grabOneButton->setEnabled(false);
        _grabLiveButton->setEnabled(false);
        _tabs->setEnabled(false);
        _deviceFeatureTree->setEnabled(false);
        _motionFeatureTree->setEnabled(false);
        for (auto* editor : _deviceFeatureEditors) {
            if (editor) editor->setEnabled(false);
        }
        setSoftwareTriggerAvailable(false);
        _connectionStatus->setText(tr("Arming"));
        _connectionStatus->setProperty("status", QStringLiteral("idle"));
        _connectionStatus->style()->unpolish(_connectionStatus);
        _connectionStatus->style()->polish(_connectionStatus);
        _messageLabel->setText(continuous
            ? tr("Arming continuous Heliotis acquisition...")
            : tr("Arming one Heliotis frame..."));
        _messageLabel->setProperty("messageState", QStringLiteral("normal"));
        _messageLabel->style()->unpolish(_messageLabel);
        _messageLabel->style()->polish(_messageLabel);
        return;
    }

    if (_acquisitionActive) {
        setAcquisitionState(true, _continuousAcquisition, _softwareTriggerAvailable);
        return;
    }
    {
        const QSignalBlocker blocker(_grabLiveButton);
        _grabLiveButton->setChecked(false);
    }
    _continuousAcquisition = false;
    _tabs->setEnabled(!_initializationPending && !_featureRefreshPending
        && !_featureOperationPending);
    _connectButton->setEnabled(_connected || !_devices.empty());
    _initializeButton->setEnabled(_connected && !_initializationPending
        && !_featureRefreshPending && !_featureOperationPending);
    _deviceFeatureTree->setEnabled(_connected && !_featureRefreshPending
        && !_featureOperationPending);
    _motionFeatureTree->setEnabled(_connected && !_featureRefreshPending
        && !_featureOperationPending);
    for (auto* editor : _deviceFeatureEditors) {
        if (editor) {
            editor->setEnabled(editor->property(featureWritableProperty).toBool()
                && !_featureRefreshPending && !_featureOperationPending);
        }
    }
    if (_connected) {
        _connectionStatus->setText(tr("Connected"));
        _connectionStatus->setProperty("status", QStringLiteral("connected"));
        _connectionStatus->style()->unpolish(_connectionStatus);
        _connectionStatus->style()->polish(_connectionStatus);
    }
    setAcquisitionAvailable(_acquisitionAvailable);
}

void QHeliotisC4Widget::setAcquisitionStopPending()
{
    if (!_acquisitionActive || _disconnectionPending) return;
    _acquisitionStopPending = true;
    _softwareTriggerPending = false;
    _connectButton->setEnabled(false);
    _initializeButton->setEnabled(false);
    _grabOneButton->setEnabled(false);
    _grabLiveButton->setEnabled(false);
    _tabs->setEnabled(false);
    _deviceFeatureTree->setEnabled(false);
    _motionFeatureTree->setEnabled(false);
    setSoftwareTriggerAvailable(false);
    _connectionStatus->setText(tr("Stopping"));
    _connectionStatus->setProperty("status", QStringLiteral("idle"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);
    _messageLabel->setText(tr("Stopping Heliotis acquisition..."));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setSoftwareTriggerPending(const bool pending)
{
    const bool nextPending = pending && _acquisitionActive && _softwareTriggerAvailable;
    if (!nextPending && !_softwareTriggerPending) return;
    _softwareTriggerPending = nextPending;
    setSoftwareTriggerAvailable(_softwareTriggerAvailable);
    if (!_featureOperationPending) updateAcquisitionMessage();
}

void QHeliotisC4Widget::setAcquisitionState(
    const bool acquiring,
    const bool continuous,
    const bool softwareTriggerAvailable)
{
    // A late worker status/error must not re-enable controls while asynchronous
    // teardown owns the device handles.
    if (_disconnectionPending) return;
    if (!_connected) return;
    if (acquiring && (!_acquisitionAvailable || _initializationPending)) return;

    _acquisitionStopPending = false;
    if (!acquiring || !_acquisitionActive) _softwareTriggerPending = false;
    _acquisitionActive = acquiring;
    _continuousAcquisition = acquiring && continuous;
    _softwareTriggerAvailable = acquiring && softwareTriggerAvailable;
    // Keep the feature tree available so an armed TriggerSoftware command can
    // be delivered to the SDK. All other editors remain locked below.
    _featureAccessCurrent = !acquiring;
    _tabs->setEnabled(_connected && !_initializationPending && !_featureOperationPending
        && !_featureRefreshPending && !_acquisitionArmPending && !_acquisitionStopPending);
    _connectButton->setEnabled(!_initializationPending && !_featureOperationPending
        && !_featureRefreshPending && !_acquisitionArmPending && !_acquisitionStopPending
        && (_connected || !_devices.empty()));
    _deviceFeatureTree->setEnabled(_connected && !_featureOperationPending
        && !_featureRefreshPending && !_acquisitionArmPending);
    _motionFeatureTree->setEnabled(_connected && !acquiring
        && !_featureOperationPending && !_featureRefreshPending && !_acquisitionArmPending);
    for (auto* editor : _deviceFeatureEditors) {
        if (editor) {
            editor->setEnabled(editor->property(featureWritableProperty).toBool()
                && !acquiring && !_featureOperationPending && !_featureRefreshPending
                && !_acquisitionArmPending);
        }
    }
    _initializeButton->setEnabled(_connected && !acquiring
        && !_initializationPending && !_featureOperationPending && !_featureRefreshPending
        && !_acquisitionArmPending);

    {
        const QSignalBlocker blocker(_grabLiveButton);
        _grabLiveButton->setChecked(acquiring && continuous);
    }
    const bool acquisitionControlsEnabled = _connected && _acquisitionAvailable
        && !_initializationPending && !_featureOperationPending && !_featureRefreshPending
        && !_acquisitionArmPending && !_acquisitionStopPending;
    _grabOneButton->setEnabled(acquisitionControlsEnabled && (!acquiring || !continuous));
    _grabOneButton->setIcon(acquiring && !continuous
        ? QIcon(QStringLiteral(":/Resources/Icons/icons8-stop-48.png"))
        : QIcon(QStringLiteral(":/Resources/Icons/icons8-camera-48.png")));
    _grabOneButton->setToolTip(acquiring && !continuous
        ? tr("Stop the armed single Heliotis acquisition")
        : tr("Arm acquisition for one Heliotis frame"));
    _grabLiveButton->setEnabled(acquisitionControlsEnabled && (!acquiring || continuous));
    setSoftwareTriggerAvailable(_softwareTriggerAvailable);
    _connectionStatus->setText(acquiring ? tr("Armed") : tr("Connected"));
    _connectionStatus->setProperty("status", acquiring ? QStringLiteral("grabbing") : QStringLiteral("connected"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);
    updateAcquisitionMessage();
}

void QHeliotisC4Widget::setAcquisitionError(const QString& message)
{
    setAcquisitionState(false, false, false);
    _messageLabel->setText(message);
    _messageLabel->setProperty("messageState", QStringLiteral("error"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setSoftwareTriggerAvailable(const bool available)
{
    const bool enabled = available && _acquisitionActive && _acquisitionAvailable
        && !_featureOperationPending && !_featureRefreshPending && !_acquisitionArmPending
        && !_acquisitionStopPending && !_softwareTriggerPending;
    for (auto* button : _softwareTriggerButtons) {
        if (button) button->setEnabled(enabled);
    }
}

void QHeliotisC4Widget::setFeatureRefreshPending(const bool pending)
{
    _featureRefreshPending = pending;
    _tabs->setEnabled(!pending && !_initializationPending && !_featureOperationPending
        && !_acquisitionArmPending);
    _connectButton->setEnabled(!pending && !_acquisitionArmPending
        && (_connected || !_devices.empty()));
    _initializeButton->setEnabled(_connected && !pending && !_initializationPending
        && !_featureOperationPending && !_acquisitionActive && !_acquisitionArmPending);
    _deviceFeatureTree->setEnabled(_connected && !pending && !_featureOperationPending
        && !_acquisitionArmPending);
    _motionFeatureTree->setEnabled(_connected && !pending
        && !_featureOperationPending && !_acquisitionActive && !_acquisitionArmPending);
    for (auto* editor : _deviceFeatureEditors) {
        if (editor) {
            editor->setEnabled(editor->property(featureWritableProperty).toBool()
                && !pending && !_featureOperationPending && !_acquisitionActive
                && !_acquisitionArmPending);
        }
    }
    setAcquisitionAvailable(_acquisitionAvailable);
    setSoftwareTriggerAvailable(_softwareTriggerAvailable);
    if (pending) {
        _messageLabel->setText(tr("Refreshing Heliotis features..."));
        _messageLabel->setProperty("messageState", QStringLiteral("normal"));
        _messageLabel->style()->unpolish(_messageLabel);
        _messageLabel->style()->polish(_messageLabel);
    } else {
        _messageLabel->setText(tr("Heliotis features refreshed."));
        _messageLabel->setProperty("messageState", QStringLiteral("normal"));
        _messageLabel->style()->unpolish(_messageLabel);
        _messageLabel->style()->polish(_messageLabel);
    }
}

void QHeliotisC4Widget::setFeatureOperationPending(const bool pending)
{
    _featureOperationPending = pending;
    _connectButton->setEnabled(!pending && !_featureRefreshPending
        && !_acquisitionArmPending && !_acquisitionStopPending
        && (_connected || !_devices.empty()));
    _tabs->setEnabled(!pending && !_initializationPending && !_featureRefreshPending
        && !_acquisitionArmPending && !_acquisitionStopPending);
    _initializeButton->setEnabled(_connected && !pending && !_initializationPending
        && !_acquisitionActive && !_featureRefreshPending && !_acquisitionArmPending
        && !_acquisitionStopPending);
    _deviceFeatureTree->setEnabled(_connected && !pending && !_featureRefreshPending
        && !_acquisitionArmPending && !_acquisitionStopPending);
    _motionFeatureTree->setEnabled(_connected && !pending
        && !_acquisitionActive && !_featureRefreshPending && !_acquisitionArmPending
        && !_acquisitionStopPending);
    for (auto* editor : _deviceFeatureEditors) {
        if (editor) {
            editor->setEnabled(editor->property(featureWritableProperty).toBool()
                && !pending && !_acquisitionActive && !_featureRefreshPending
                && !_acquisitionArmPending && !_acquisitionStopPending);
        }
    }

    // A successful feature write keeps featureOperationPending set while the
    // follow-up refresh runs. The refresh therefore disables capture controls;
    // recompute them when the outer operation finally completes. Do not use
    // setAcquisitionAvailable() while armed because Single/Live stop controls
    // have mode-specific enablement owned by setAcquisitionState().
    if (!_acquisitionActive) setAcquisitionAvailable(_acquisitionAvailable);
    if (!pending) {
        setSoftwareTriggerAvailable(_acquisitionActive && _acquisitionAvailable && _softwareTriggerAvailable);
        if (_softwareTriggerPending) setSoftwareTriggerPending(true);
        else if (_acquisitionActive) updateAcquisitionMessage();
        return;
    }

    _messageLabel->setText(tr("Applying Heliotis feature change..."));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::updateAcquisitionMessage()
{
    if (!_acquisitionActive) {
        _messageLabel->setText(tr("Heliotis acquisition stopped."));
    } else if (_softwareTriggerPending) {
        _messageLabel->setText(tr("TriggerSoftware was accepted. Waiting for its Heliotis frame..."));
    } else if (_softwareTriggerAvailable) {
        _messageLabel->setText(_continuousAcquisition
            ? tr("Live is armed. Each TriggerSoftware command starts FrameStart; RecordingStart may still gate delivery.")
            : tr("Single is armed. Execute TriggerSoftware; RecordingStart may still gate its one frame."));
    } else {
        _messageLabel->setText(_continuousAcquisition
            ? tr("Live is armed for automatic or external frames.")
            : tr("Single is armed for one automatic or external frame."));
    }
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setFeatureError(const QString& message)
{
    _messageLabel->setText(message);
    _messageLabel->setProperty("messageState", QStringLiteral("error"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setIdleState(const QString& message)
{
    _connectionStatus->setText(tr("Idle"));
    _connectionStatus->setProperty("status", QStringLiteral("idle"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);
    _messageLabel->setText(message);
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setFeatures(const heliotis::HeliotisC4::FeatureList& features)
{
    _featureAccessCurrent = !_acquisitionActive;
    _deviceFeatureTree->setEnabled(_connected && !_featureOperationPending
        && !_featureRefreshPending && !_acquisitionArmPending);
    _motionFeatureTree->setEnabled(_connected && !_acquisitionActive
        && !_featureOperationPending && !_featureRefreshPending && !_acquisitionArmPending);
    heliotis::HeliotisC4::FeatureList deviceFeatures;
    heliotis::HeliotisC4::FeatureList motionFeatures;
    for (const auto& feature : features) {
        if (feature.section == heliotis::FeatureSection::Motion) {
            motionFeatures.push_back(feature);
        } else {
            deviceFeatures.push_back(feature);
        }
    }
    populateTree(_deviceFeatureTree, _deviceCategories, deviceFeatures);
    populateTree(_motionFeatureTree, _motionCategories, motionFeatures);
    setSoftwareTriggerAvailable(_acquisitionActive && _acquisitionAvailable && _softwareTriggerAvailable);
}

void QHeliotisC4Widget::populateTree(
    QTreeWidget* tree,
    QHash<QString, QTreeWidgetItem*>& categories,
    const heliotis::HeliotisC4::FeatureList& features)
{
    const TreeState state = captureTreeState(tree, categories);

    tree->clear();
    categories.clear();
    if (tree == _deviceFeatureTree) {
        _softwareTriggerButtons.clear();
        _deviceFeatureEditors.clear();
    }
    for (const auto& feature : features) {
        auto* parent = ensureCategory(tree, categories, QString::fromStdString(feature.categoryPath));
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        item->setData(0, featureTreeIdRole, featureItemId(feature));
        item->setText(0, QString::fromStdString(feature.displayName));
        item->setToolTip(0, QString::fromStdString(feature.description));
        const QString featureName = QString::fromStdString(feature.displayName);
        const bool writable = feature.access == heliotis::FeatureAccess::ReadWrite
            || feature.access == heliotis::FeatureAccess::WriteOnly;

        if (feature.type == heliotis::FeatureType::Command) {
            auto* executeButton = new QPushButton(tr("Execute"), tree);
            const bool isSoftwareTrigger = featureName == QStringLiteral("TriggerSoftware");
            executeButton->setProperty(featureWritableProperty, writable);
            executeButton->setEnabled(writable && _featureAccessCurrent && !_featureOperationPending
                && (!isSoftwareTrigger || _acquisitionActive));
            connect(executeButton, &QPushButton::clicked, this, [this, featureName] {
                emit featureCommandRequested(featureName);
            });
            tree->setItemWidget(item, 1, executeButton);
            if (isSoftwareTrigger) {
                _softwareTriggerButtons.push_back(executeButton);
            } else if (tree == _deviceFeatureTree) {
                _deviceFeatureEditors.push_back(executeButton);
            }
            continue;
        }

        if (feature.type == heliotis::FeatureType::Boolean) {
            auto* checkBox = new QCheckBox(tree);
            checkBox->setProperty(featureWritableProperty, writable);
            checkBox->setChecked(feature.valueText == "1" || feature.valueText == "true");
            checkBox->setEnabled(writable && _featureAccessCurrent && !_featureOperationPending);
            connect(checkBox, &QCheckBox::toggled, this, [this, featureName](const bool checked) {
                emit featureWriteRequested(featureName, checked ? QStringLiteral("1") : QStringLiteral("0"));
            });
            tree->setItemWidget(item, 1, checkBox);
            if (tree == _deviceFeatureTree) _deviceFeatureEditors.push_back(checkBox);
            continue;
        }

        if (feature.type == heliotis::FeatureType::Enumeration && !feature.enumEntries.empty()) {
            auto* comboBox = new QComboBox(tree);
            comboBox->setProperty(featureWritableProperty, writable);
            for (const auto& entry : feature.enumEntries) {
                comboBox->addItem(QString::fromStdString(entry));
            }
            comboBox->setCurrentText(QString::fromStdString(feature.valueText));
            comboBox->setEnabled(writable && _featureAccessCurrent && !_featureOperationPending);
            connect(comboBox, &QComboBox::textActivated, this, [this, featureName](const QString& value) {
                emit featureWriteRequested(featureName, value);
            });
            tree->setItemWidget(item, 1, comboBox);
            if (tree == _deviceFeatureTree) _deviceFeatureEditors.push_back(comboBox);
            continue;
        }

        auto* lineEdit = new QLineEdit(tree);
        lineEdit->setProperty(featureWritableProperty, writable);
        if (feature.access == heliotis::FeatureAccess::WriteOnly) {
            lineEdit->setPlaceholderText(tr("Enter value"));
        } else {
            lineEdit->setText(QString::fromStdString(feature.valueText));
        }
        lineEdit->setEnabled(writable && _featureAccessCurrent && !_featureOperationPending);
        connect(lineEdit, &QLineEdit::editingFinished, this, [this, featureName, lineEdit] {
            emit featureWriteRequested(featureName, lineEdit->text());
        });
        tree->setItemWidget(item, 1, lineEdit);
        if (tree == _deviceFeatureTree) _deviceFeatureEditors.push_back(lineEdit);
    }
    restoreTreeState(tree, categories, state);
}

QHeliotisC4Widget::TreeState QHeliotisC4Widget::captureTreeState(
    QTreeWidget* tree,
    const QHash<QString, QTreeWidgetItem*>& categories) const
{
    TreeState state;
    state.hadItems = tree->topLevelItemCount() > 0;
    for (auto iterator = categories.cbegin(); iterator != categories.cend(); ++iterator) {
        if (iterator.value() && iterator.value()->isExpanded()) {
            state.expandedCategories.insert(iterator.key());
        }
    }
    if (tree->currentItem()) {
        state.currentItem = itemId(tree->currentItem());
    }
    state.verticalScrollValue = tree->verticalScrollBar()->value();
    state.horizontalScrollValue = tree->horizontalScrollBar()->value();
    if (QTreeWidgetItem* topItem = tree->itemAt(tree->viewport()->rect().topLeft())) {
        state.topVisibleItem = itemId(topItem);
        state.topVisibleOffset = tree->visualItemRect(topItem).top();
    }
    return state;
}

void QHeliotisC4Widget::restoreTreeState(
    QTreeWidget* tree,
    const QHash<QString, QTreeWidgetItem*>& categories,
    const TreeState& state) const
{
    if (!state.hadItems) {
        // Keep the initial tree compact: show each top-level category's direct
        // children, while leaving deeper category levels collapsed.
        tree->expandToDepth(0);
    } else {
        for (auto iterator = categories.cbegin(); iterator != categories.cend(); ++iterator) {
            if (iterator.value()) {
                iterator.value()->setExpanded(state.expandedCategories.contains(iterator.key()));
            }
        }
    }

    QHash<QString, QTreeWidgetItem*> items;
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        collectTreeItems(tree->topLevelItem(index), items);
    }
    if (!state.currentItem.isEmpty()) {
        if (QTreeWidgetItem* currentItem = items.value(state.currentItem)) {
            tree->setCurrentItem(currentItem);
        }
    }

    if (QTreeWidgetItem* topVisibleItem = items.value(state.topVisibleItem)) {
        tree->scrollToItem(topVisibleItem, QAbstractItemView::PositionAtTop);
        const int offsetDelta =
            tree->visualItemRect(topVisibleItem).top() - state.topVisibleOffset;
        tree->verticalScrollBar()->setValue(
            tree->verticalScrollBar()->value() + offsetDelta);
    } else {
        tree->verticalScrollBar()->setValue(state.verticalScrollValue);
    }
    tree->horizontalScrollBar()->setValue(state.horizontalScrollValue);
}

QTreeWidgetItem* QHeliotisC4Widget::ensureCategory(
    QTreeWidget* tree,
    QHash<QString, QTreeWidgetItem*>& categories,
    const QString& categoryPath)
{
    if (categoryPath.isEmpty()) return nullptr;

    QTreeWidgetItem* parent = nullptr;
    QString currentPath;
    for (const auto& segment : categoryPath.split('/', Qt::SkipEmptyParts)) {
        currentPath += currentPath.isEmpty() ? segment : QStringLiteral("/") + segment;
        auto* category = categories.value(currentPath);
        if (!category) {
            category = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
            category->setData(0, featureTreeIdRole, categoryItemId(currentPath));
            category->setText(0, segment);
            category->setFlags(Qt::ItemIsEnabled);
            categories.insert(currentPath, category);
        }
        parent = category;
    }
    return parent;
}

#endif // HELIOTISC4_HAS_QT_UI
