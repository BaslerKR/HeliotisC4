#include "QHeliotisC4Widget.h"

#ifdef HELIOTISC4_HAS_QT_UI

#include <QTabWidget>
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
