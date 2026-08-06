#include "Utility/Qt/QHeliotisC4Widget.h"

#include <QApplication>
#include <QDebug>
#include <QScrollBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdlib>

namespace {

heliotis::FeatureDescriptor makeFeature(
    const QString& categoryPath,
    const QString& displayName,
    const QString& value)
{
    return {
        heliotis::FeatureSection::Device,
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
    if (!condition) qCritical().noquote() << message;
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

    widget.setFeatures(features);
    application.processEvents();

    auto* tree = widget.findChild<QTreeWidget*>(QStringLiteral("HeliotisC4DeviceFeatureTree"));
    if (!require(tree != nullptr, "Heliotis device feature tree was not created.")) return EXIT_FAILURE;
    auto* mainCategory = tree->topLevelItem(0);
    if (!require(mainCategory != nullptr && mainCategory->text(0) == QStringLiteral("Main"),
                 "The expected top-level Heliotis category is missing.")) {
        return EXIT_FAILURE;
    }
    auto* childCategory = mainCategory->child(0);
    if (!require(mainCategory->isExpanded(), "Top-level Heliotis categories should be expanded initially.")) {
        return EXIT_FAILURE;
    }
    if (!require(childCategory != nullptr && !childCategory->isExpanded(),
                 "Nested Heliotis categories should be collapsed initially.")) {
        return EXIT_FAILURE;
    }

    tree->setCurrentItem(tree->topLevelItem(20));
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

    return EXIT_SUCCESS;
}
