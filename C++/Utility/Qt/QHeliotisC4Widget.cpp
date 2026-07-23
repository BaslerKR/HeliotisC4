#include "QHeliotisC4Widget.h"

#ifdef HELIOTISC4_HAS_QT_UI

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

} // namespace

QHeliotisC4Widget::QHeliotisC4Widget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(300, 350);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setObjectName(QStringLiteral("DeviceRootLayout"));

    _featureTree = new QTreeWidget(this);
    _featureTree->setObjectName(QStringLiteral("HeliotisC4FeatureTree"));
    _featureTree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    _featureTree->setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Value"), QStringLiteral("Access")});

    auto* treePanelLayout = new QVBoxLayout;
    treePanelLayout->setObjectName(QStringLiteral("DeviceTreePanelLayout"));
    treePanelLayout->addWidget(_featureTree);
    rootLayout->addLayout(treePanelLayout);
}

void QHeliotisC4Widget::setFeatures(const heliotis::HeliotisC4::FeatureList& features)
{
    _featureTree->clear();
    _categories.clear();
    for (const auto& feature : features) {
        auto* parent = ensureCategory(QString::fromStdString(feature.categoryPath));
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(_featureTree);
        item->setText(0, QString::fromStdString(feature.displayName));
        item->setText(1, QString::fromStdString(feature.valueText));
        item->setText(2, accessText(feature.access));
        item->setToolTip(0, QString::fromStdString(feature.description));
    }
    _featureTree->expandToDepth(0);
}

QTreeWidgetItem* QHeliotisC4Widget::ensureCategory(const QString& categoryPath)
{
    if (categoryPath.isEmpty()) return nullptr;

    QTreeWidgetItem* parent = nullptr;
    QString currentPath;
    for (const auto& segment : categoryPath.split('/', Qt::SkipEmptyParts)) {
        currentPath += currentPath.isEmpty() ? segment : QStringLiteral("/") + segment;
        auto* category = _categories.value(currentPath);
        if (!category) {
            category = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(_featureTree);
            category->setText(0, segment);
            category->setFlags(Qt::ItemIsEnabled);
            _categories.insert(currentPath, category);
        }
        parent = category;
    }
    return parent;
}

#endif // HELIOTISC4_HAS_QT_UI
