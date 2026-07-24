#include "QHeliotisC4Widget.h"

#ifdef HELIOTISC4_HAS_QT_UI

#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSize>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
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

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("DeviceSelectorLayout"));
    selectorLayout->addWidget(_deviceSelector);
    selectorLayout->addWidget(_refreshButton);

    auto* toolLayout = new QHBoxLayout;
    toolLayout->setObjectName(QStringLiteral("DeviceToolLayout"));
    toolLayout->addWidget(_connectButton);

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

    connect(_refreshButton, &QToolButton::clicked, this, &QHeliotisC4Widget::refreshRequested);
    connect(_connectButton, &QToolButton::toggled, this, [this](const bool connected) {
        if (!connected) {
            emit disconnectRequested();
            return;
        }
        const int index = _deviceSelector->currentData().toInt();
        if (index >= 0 && index < static_cast<int>(_devices.size())) {
            emit connectRequested(index);
            return;
        }
        const QSignalBlocker blocker(_connectButton);
        _connectButton->setChecked(false);
    });
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
    _connectButton->setEnabled(!_devices.empty() && !_connectButton->isChecked());
    _messageLabel->setText(_devices.empty()
        ? tr("No Heliotis devices discovered.")
        : tr("%n device(s) discovered.", nullptr, static_cast<int>(_devices.size())));
}

void QHeliotisC4Widget::setConnectionState(const bool connected)
{
    _connectionStatus->setText(connected ? tr("Connected") : tr("Disconnected"));
    _connectionStatus->setProperty("status", connected ? QStringLiteral("connected") : QStringLiteral("disconnected"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);

    const QSignalBlocker blocker(_connectButton);
    _connectButton->setChecked(connected);
    _connectButton->setToolTip(connected
        ? tr("Disconnect the selected device")
        : tr("Connect the selected device"));
    _connectButton->setEnabled(connected || !_devices.empty());
    _deviceSelector->setEnabled(!connected && !_devices.empty());
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
    for (const auto& feature : features) {
        auto* parent = ensureCategory(tree, categories, QString::fromStdString(feature.categoryPath));
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        item->setText(0, QString::fromStdString(feature.displayName));
        item->setText(1, QString::fromStdString(feature.valueText));
        item->setToolTip(0, QString::fromStdString(feature.description));
        if (feature.access != heliotis::FeatureAccess::ReadWrite
            || feature.type == heliotis::FeatureType::Command) {
            item->setDisabled(true);
        }
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
