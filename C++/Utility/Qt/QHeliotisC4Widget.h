#pragma once

#ifdef HELIOTISC4_HAS_QT_UI

#include "HeliotisC4.h"

#include <QHash>
#include <QString>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;

class QHeliotisC4Widget final : public QWidget {
    Q_OBJECT

public:
    explicit QHeliotisC4Widget(QWidget* parent = nullptr);

    void setFeatures(const heliotis::HeliotisC4::FeatureList& features);

private:
    void populateTree(
        QTreeWidget* tree,
        QHash<QString, QTreeWidgetItem*>& categories,
        const heliotis::HeliotisC4::FeatureList& features);
    QTreeWidgetItem* ensureCategory(
        QTreeWidget* tree,
        QHash<QString, QTreeWidgetItem*>& categories,
        const QString& categoryPath);

    QTabWidget* _tabs = nullptr;
    QTreeWidget* _deviceFeatureTree = nullptr;
    QTreeWidget* _motionFeatureTree = nullptr;
    QHash<QString, QTreeWidgetItem*> _deviceCategories;
    QHash<QString, QTreeWidgetItem*> _motionCategories;
};

#endif // HELIOTISC4_HAS_QT_UI
