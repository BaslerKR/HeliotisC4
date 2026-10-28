#include "HeliotisC4FeatureTree.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace {

/** Encodes compound row identities without delimiter collisions. */
QString tupleKey(const QStringList& parts)
{
    return QString::fromLatin1(QJsonDocument(QJsonArray::fromStringList(parts))
        .toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding));
}

} // namespace

HeliotisC4FeatureTree::HeliotisC4FeatureTree(QWidget* parent, const QString& settingsFile,
                                             const QString& favoritesKey)
    : SearchableTreeWidget(parent,
          settingsFile.isEmpty()
              ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("HeliotisC4Features.ini"))
              : settingsFile,
          favoritesKey.isEmpty() ? QStringLiteral("Favorites/Features") : favoritesKey)
{
    setSearchPlaceholderText(tr("Search features"));
}

void HeliotisC4FeatureTree::registerFeature(QTreeWidgetItem* item, const QString& featureName)
{
    if (!item) return;
    item->setData(0, FeatureNameRole, featureName);
    registerItem(item, tupleKey({QStringLiteral("Device"), featureName}));
}

QString HeliotisC4FeatureTree::itemStateKey(const QTreeWidgetItem* item) const
{
    return tupleKey({item->data(0, ItemIdRole).toString()});
}

QString HeliotisC4FeatureTree::itemSearchText(const QTreeWidgetItem* item) const
{
    return item->text(0) + QChar(' ') + item->data(0, FeatureNameRole).toString();
}
