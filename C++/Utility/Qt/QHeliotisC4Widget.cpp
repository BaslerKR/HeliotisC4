#include "QHeliotisC4Widget.h"

#ifdef HELIOTISC4_HAS_QT_UI

#include <QComboBox>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSize>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {

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

    _grabOneButton = new QToolButton(this);
    _grabOneButton->setObjectName(QStringLiteral("HeliotisC4GrabOneButton"));
    _grabOneButton->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-camera-48.png")));
    _grabOneButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _grabOneButton->setIconSize(QSize(16, 16));
    _grabOneButton->setToolTip(tr("Acquire one Heliotis frame"));
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
    connect(_grabOneButton, &QToolButton::clicked, this, &QHeliotisC4Widget::grabOneRequested);
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
    _deviceSelector->setEnabled(!_devices.empty());
    _connectButton->setEnabled(!_devices.empty() && !_connectButton->isChecked() && !_connectionPending);
    _refreshButton->setEnabled(true);
    if (!_connectButton->isChecked()) {
        setIdleState(_devices.empty()
        ? tr("No Heliotis devices discovered.")
        : tr("%n device(s) discovered.", nullptr, static_cast<int>(_devices.size())));
    }
}

void QHeliotisC4Widget::setDiscoveryError(const QString& message)
{
    _refreshButton->setEnabled(true);
    setIdleState(message);
    _messageLabel->setProperty("messageState", QStringLiteral("error"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setConnectionState(const bool connected)
{
    _connectionPending = false;
    const QString previousStatus = _connectionStatus->property("status").toString();
    const bool wasConnected = previousStatus == QStringLiteral("connected")
        || previousStatus == QStringLiteral("grabbing");

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
    setAcquisitionAvailable(connected && _acquisitionAvailable);
    _messageLabel->setText(connected
        ? tr("Heliotis device connected.")
        : tr("Heliotis device disconnected."));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
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
    _deviceSelector->setEnabled(false);
    _refreshButton->setEnabled(false);
    setAcquisitionAvailable(false);
    _messageLabel->setText(tr("Connecting and initializing the selected Heliotis device..."));
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
    if (!available) _acquisitionActive = false;
    const QString status = _connectionStatus->property("status").toString();
    const bool enabled = (status == QStringLiteral("connected") || status == QStringLiteral("grabbing")) && available;
    _grabOneButton->setEnabled(enabled && !_grabLiveButton->isChecked());
    _grabLiveButton->setEnabled(enabled);
    if (!enabled) {
        const QSignalBlocker blocker(_grabLiveButton);
        _grabLiveButton->setChecked(false);
    }
    setSoftwareTriggerAvailable(_acquisitionActive && enabled);
}

void QHeliotisC4Widget::setAcquisitionState(const bool acquiring, const bool continuous)
{
    if (!_acquisitionAvailable) return;

    _acquisitionActive = acquiring;

    {
        const QSignalBlocker blocker(_grabLiveButton);
        _grabLiveButton->setChecked(acquiring && continuous);
    }
    _grabOneButton->setEnabled(!acquiring);
    _grabLiveButton->setEnabled(!acquiring || continuous);
    setSoftwareTriggerAvailable(acquiring);
    _connectionStatus->setText(acquiring ? tr("Armed") : tr("Connected"));
    _connectionStatus->setProperty("status", acquiring ? QStringLiteral("grabbing") : QStringLiteral("connected"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);
    _messageLabel->setText(acquiring
        ? (continuous
            ? tr("Armed for frames from the current Heliotis trigger configuration.")
            : tr("Armed for one Heliotis frame from the current trigger configuration."))
        : tr("Heliotis acquisition stopped."));
    _messageLabel->setProperty("messageState", QStringLiteral("normal"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setAcquisitionError(const QString& message)
{
    setAcquisitionState(false, false);
    _messageLabel->setText(message);
    _messageLabel->setProperty("messageState", QStringLiteral("error"));
    _messageLabel->style()->unpolish(_messageLabel);
    _messageLabel->style()->polish(_messageLabel);
}

void QHeliotisC4Widget::setSoftwareTriggerAvailable(const bool available)
{
    for (auto* button : _softwareTriggerButtons) {
        if (button) button->setEnabled(available && !_featureOperationPending);
    }
}

void QHeliotisC4Widget::setFeatureOperationPending(const bool pending)
{
    _featureOperationPending = pending;
    _tabs->setEnabled(!pending);
    if (!pending) return;

    _messageLabel->setText(tr("Applying Heliotis feature change..."));
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
}

void QHeliotisC4Widget::populateTree(
    QTreeWidget* tree,
    QHash<QString, QTreeWidgetItem*>& categories,
    const heliotis::HeliotisC4::FeatureList& features)
{
    tree->clear();
    categories.clear();
    if (tree == _deviceFeatureTree) _softwareTriggerButtons.clear();
    for (const auto& feature : features) {
        auto* parent = ensureCategory(tree, categories, QString::fromStdString(feature.categoryPath));
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        item->setText(0, QString::fromStdString(feature.displayName));
        item->setToolTip(0, QString::fromStdString(feature.description));
        const QString featureName = QString::fromStdString(feature.displayName);
        const bool writable = feature.access == heliotis::FeatureAccess::ReadWrite
            || feature.access == heliotis::FeatureAccess::WriteOnly;

        if (feature.type == heliotis::FeatureType::Command) {
            auto* executeButton = new QPushButton(tr("Execute"), tree);
            const bool isSoftwareTrigger = featureName == QStringLiteral("TriggerSoftware");
            executeButton->setEnabled(writable && !_featureOperationPending
                && (!isSoftwareTrigger || _acquisitionActive));
            connect(executeButton, &QPushButton::clicked, this, [this, featureName] {
                emit featureCommandRequested(featureName);
            });
            tree->setItemWidget(item, 1, executeButton);
            if (isSoftwareTrigger) _softwareTriggerButtons.push_back(executeButton);
            continue;
        }

        if (!writable) {
            item->setText(1, QString::fromStdString(feature.valueText));
            item->setDisabled(true);
            continue;
        }

        if (feature.type == heliotis::FeatureType::Boolean) {
            auto* checkBox = new QCheckBox(tree);
            checkBox->setChecked(feature.valueText == "1" || feature.valueText == "true");
            checkBox->setEnabled(!_featureOperationPending);
            connect(checkBox, &QCheckBox::toggled, this, [this, featureName](const bool checked) {
                emit featureWriteRequested(featureName, checked ? QStringLiteral("1") : QStringLiteral("0"));
            });
            tree->setItemWidget(item, 1, checkBox);
            continue;
        }

        if (feature.type == heliotis::FeatureType::Enumeration && !feature.enumEntries.empty()) {
            auto* comboBox = new QComboBox(tree);
            for (const auto& entry : feature.enumEntries) {
                comboBox->addItem(QString::fromStdString(entry));
            }
            comboBox->setCurrentText(QString::fromStdString(feature.valueText));
            comboBox->setEnabled(!_featureOperationPending);
            connect(comboBox, &QComboBox::textActivated, this, [this, featureName](const QString& value) {
                emit featureWriteRequested(featureName, value);
            });
            tree->setItemWidget(item, 1, comboBox);
            continue;
        }

        auto* lineEdit = new QLineEdit(tree);
        if (feature.access == heliotis::FeatureAccess::WriteOnly) {
            lineEdit->setPlaceholderText(tr("Enter value"));
        } else {
            lineEdit->setText(QString::fromStdString(feature.valueText));
        }
        lineEdit->setEnabled(!_featureOperationPending);
        connect(lineEdit, &QLineEdit::editingFinished, this, [this, featureName, lineEdit] {
            emit featureWriteRequested(featureName, lineEdit->text());
        });
        tree->setItemWidget(item, 1, lineEdit);
    }
    tree->expandToDepth(0);
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
            category->setText(0, segment);
            category->setFlags(Qt::ItemIsEnabled);
            categories.insert(currentPath, category);
        }
        parent = category;
    }
    return parent;
}

#endif // HELIOTISC4_HAS_QT_UI
