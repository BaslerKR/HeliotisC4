#include "QHeliotisC4Widget.h"

#ifdef HELIOTISC4_HAS_QT_UI

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {

QString accessText(heliotis::FeatureAccess access)
{
    switch (access) {
    case heliotis::FeatureAccess::NotImplemented: return QStringLiteral("Not implemented");
    case heliotis::FeatureAccess::NotAvailable: return QStringLiteral("Not available");
    case heliotis::FeatureAccess::ReadOnly: return QStringLiteral("Read only");
    case heliotis::FeatureAccess::WriteOnly: return QStringLiteral("Write only");
    case heliotis::FeatureAccess::ReadWrite: return QStringLiteral("Read/write");
    case heliotis::FeatureAccess::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

QTreeWidget* createFeatureTree(QWidget* parent, const QString& objectName)
{
    auto* tree = new QTreeWidget(parent);
    tree->setObjectName(objectName);
    tree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    tree->setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Value"), QStringLiteral("Access")});
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
    _refreshButton->setText(tr("Refresh"));

    _connectButton = new QToolButton(this);
    _connectButton->setObjectName(QStringLiteral("HeliotisC4ConnectButton"));
    _connectButton->setText(tr("Connect"));
    _connectButton->setEnabled(false);

    _connectionStatus = new QLabel(this);
    _connectionStatus->setObjectName(QStringLiteral("HeliotisC4ConnectionStatus"));
    _connectionStatus->setProperty("status", QStringLiteral("disconnected"));

    topLayout->addWidget(_deviceSelector, 1);
    topLayout->addWidget(_refreshButton);
    topLayout->addWidget(_connectButton);
    topLayout->addWidget(_connectionStatus);
    rootLayout->addLayout(topLayout);

    _tabs = new QTabWidget(this);
    _tabs->setObjectName(QStringLiteral("HeliotisC4ControlTabs"));

    auto* devicePage = new QWidget(_tabs);
    auto* deviceLayout = new QVBoxLayout(devicePage);
    deviceLayout->setObjectName(QStringLiteral("DeviceTreePanelLayout"));
    _deviceFeatureTree = createFeatureTree(devicePage, QStringLiteral("HeliotisC4DeviceFeatureTree"));
    deviceLayout->addWidget(_deviceFeatureTree);
    _tabs->addTab(devicePage, tr("Device"));

    auto* motionPage = new QWidget(_tabs);
    auto* motionLayout = new QVBoxLayout(motionPage);
    motionLayout->setObjectName(QStringLiteral("DeviceTreePanelLayout"));
    _motionFeatureTree = createFeatureTree(motionPage, QStringLiteral("HeliotisC4MotionFeatureTree"));
    motionLayout->addWidget(_motionFeatureTree);
    _tabs->addTab(motionPage, tr("Motion"));

    rootLayout->addWidget(_tabs);

    connect(_refreshButton, &QToolButton::clicked, this, &QHeliotisC4Widget::refreshRequested);
    connect(_connectButton, &QToolButton::clicked, this, [this]() {
        if (_connectButton->property("connected").toBool()) {
            emit disconnectRequested();
            return;
        }
        const int index = _deviceSelector->currentData().toInt();
        if (index >= 0 && index < static_cast<int>(_devices.size())) emit connectRequested(index);
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
        const QString name = QString::fromStdString(device.deviceName);
        const QString interfaceName = QString::fromStdString(device.interfaceName);
        _deviceSelector->addItem(
            interfaceName.isEmpty() ? name : QStringLiteral("%1 — %2").arg(name, interfaceName),
            index);
    }
    _deviceSelector->setEnabled(!_devices.empty());
    _connectButton->setEnabled(!_devices.empty() && !_connectButton->property("connected").toBool());
}

void QHeliotisC4Widget::setConnectionState(const bool connected)
{
    _connectionStatus->setText(connected ? tr("Connected") : tr("Disconnected"));
    _connectionStatus->setProperty("status", connected ? QStringLiteral("connected") : QStringLiteral("disconnected"));
    _connectionStatus->style()->unpolish(_connectionStatus);
    _connectionStatus->style()->polish(_connectionStatus);

    _connectButton->setProperty("connected", connected);
    _connectButton->setText(connected ? tr("Disconnect") : tr("Connect"));
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
        item->setText(2, accessText(feature.access));
        item->setToolTip(0, QString::fromStdString(feature.description));
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
