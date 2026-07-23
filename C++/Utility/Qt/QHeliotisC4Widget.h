#pragma once

#ifdef HELIOTISC4_HAS_QT_UI

#include "HeliotisC4.h"

#include <QHash>
#include <QString>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

class QHeliotisC4Widget final : public QWidget {
    Q_OBJECT

public:
    explicit QHeliotisC4Widget(QWidget* parent = nullptr);

    void setFeatures(const heliotis::HeliotisC4::FeatureList& features);

private:
    QTreeWidgetItem* ensureCategory(const QString& categoryPath);

    QTreeWidget* _featureTree = nullptr;
    QHash<QString, QTreeWidgetItem*> _categories;
};

#endif // HELIOTISC4_HAS_QT_UI
