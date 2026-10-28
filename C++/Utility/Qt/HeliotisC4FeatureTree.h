#pragma once

#include "Widgets/SearchableTreeWidget.h"

/** Heliotis feature identity and storage adapter for the shared searchable tree. */
class HeliotisC4FeatureTree final : public SearchableTreeWidget {
    Q_OBJECT
public:
    /** Producer-owned row identity; common browser roles occupy a separate range. */
    enum Role { ItemIdRole = Qt::UserRole, FeatureNameRole };
    /**
     * Creates a feature browser with consumer storage or the standalone Heliotis store.
     *
     * @param settingsFile Consumer INI file; empty selects `<AppLocalData>/HeliotisC4Features.ini`.
     * @param favoritesKey Consumer key; empty selects `Favorites/Features`.
     */
    explicit HeliotisC4FeatureTree(QWidget* parent = nullptr, const QString& settingsFile = {},
                                   const QString& favoritesKey = {});
    /**
     * Registers one feature row by its SDK feature name.
     *
     * @note The favorite key excludes the category path and device name, so a
     *       favorite follows the feature across sessions, devices and SDK
     *       category changes.
     */
    void registerFeature(QTreeWidgetItem* item, const QString& featureName);

protected:
    /** Identifies rows within the current snapshot, independently of saved favorite keys. */
    QString itemStateKey(const QTreeWidgetItem* item) const override;
    /** Includes SDK feature names when searching feature and category rows. */
    QString itemSearchText(const QTreeWidgetItem* item) const override;
};
