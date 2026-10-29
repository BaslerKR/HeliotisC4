#include "HeliotisC4ControlUi.h"
#include <QtWidgets>

namespace {
HeliotisC4FeatureTree* createFeatureTree(QWidget* parent, const QString& objectName,
                                         const QString& settingsFile, const QString& favoritesKey)
{
    // The plugin owns its browser, so the host does not attach a second one.
    auto* tree = new HeliotisC4FeatureTree(parent, settingsFile, favoritesKey);
    tree->setObjectName(objectName);
    tree->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
    tree->setColumnCount(2);
    tree->setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Value")});
    tree->setUniformRowHeights(true);
    tree->setRootIsDecorated(true);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    return tree;
}
}

void HeliotisC4ControlUi::create(QWidget* owner, const QString& settingsFile, const QString& favoritesKey) {
    owner->setProperty("actionDensity", "compact");
    owner->setProperty("controlRole", "devicePanel");

    owner->setMinimumSize(300, 350);

    auto* rootLayout = new QVBoxLayout(owner);
    rootLayout->setObjectName(QStringLiteral("DeviceRootLayout"));
    rootLayout->setProperty("layoutRole", "flush");

    auto* topLayout = new QHBoxLayout;
    topLayout->setObjectName(QStringLiteral("DeviceTopBarLayout"));
    topLayout->setProperty("layoutRole", "content");

    _deviceSelector = new QComboBox(owner);
    _deviceSelector->setObjectName(QStringLiteral("HeliotisC4DeviceSelector"));
    _deviceSelector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _refreshButton = new QToolButton(owner);
    _refreshButton->setObjectName(QStringLiteral("HeliotisC4RefreshButton"));
    _refreshButton->setIcon(QIcon(QStringLiteral(":/Resources/icon/refresh-48.png")));
    _refreshButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _refreshButton->setIconSize(QSize(16, 16));
    _refreshButton->setToolTip(QCoreApplication::translate("QHeliotisC4Widget", "Refresh Heliotis devices"));

    _connectButton = new QToolButton(owner);
    _connectButton->setObjectName(QStringLiteral("HeliotisC4ConnectButton"));
    _connectButton->setCheckable(true);
    _connectButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _connectButton->setIconSize(QSize(16, 16));
    _connectButton->setToolTip(QCoreApplication::translate("QHeliotisC4Widget", "Connect the selected device"));
    QIcon connectIcon;
    // The glyph names the current state; the shared theme previews the other state on hover.
    connectIcon.addFile(QStringLiteral(":/Resources/icon/disconnected-48.png"), QSize(), QIcon::Normal, QIcon::Off);
    connectIcon.addFile(QStringLiteral(":/Resources/icon/connect-48.png"), QSize(), QIcon::Normal, QIcon::On);
    _connectButton->setIcon(connectIcon);
    _connectButton->setEnabled(false);

    _initializeButton = new QToolButton(owner);
    _initializeButton->setObjectName(QStringLiteral("HeliotisC4InitializeButton"));
    _initializeButton->setIcon(QIcon(QStringLiteral(":/Resources/icon/setting-48.png")));
    _initializeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _initializeButton->setIconSize(QSize(16, 16));
    _initializeButton->setToolTip(QCoreApplication::translate("QHeliotisC4Widget", "Reset all H8 capture settings to the vendor defaults and initialize the stage"));
    _initializeButton->setEnabled(false);

    _grabOneButton = new QToolButton(owner);
    _grabOneButton->setObjectName(QStringLiteral("HeliotisC4GrabOneButton"));
    _grabOneButton->setIcon(QIcon(QStringLiteral(":/Resources/icon/camera-48.png")));
    _grabOneButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _grabOneButton->setIconSize(QSize(16, 16));
    _grabOneButton->setToolTip(QCoreApplication::translate("QHeliotisC4Widget", "Arm acquisition for one Heliotis frame"));
    _grabOneButton->setEnabled(false);

    _grabLiveButton = new QToolButton(owner);
    _grabLiveButton->setObjectName(QStringLiteral("HeliotisC4GrabLiveButton"));
    _grabLiveButton->setCheckable(true);
    _grabLiveButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _grabLiveButton->setIconSize(QSize(16, 16));

    QIcon liveIcon;
    // Always show the next acquisition command, including during hover.
    liveIcon.addFile(QStringLiteral(":/Resources/icon/cameras-48.png"), QSize(), QIcon::Normal, QIcon::Off);
    liveIcon.addFile(QStringLiteral(":/Resources/icon/stop-48.png"), QSize(), QIcon::Normal, QIcon::On);
    _grabLiveButton->setProperty("iconStateRole", "action");
    _grabLiveButton->setIcon(liveIcon);
    QObject::connect(_grabLiveButton, &QToolButton::toggled, owner, [this] { updateLiveAction(); });
    updateLiveAction();
    _grabLiveButton->setEnabled(false);

    auto* selectorLayout = new QHBoxLayout;
    selectorLayout->setObjectName(QStringLiteral("DeviceSelectorLayout"));
    selectorLayout->setProperty("layoutRole", "row");
    selectorLayout->addWidget(_deviceSelector);
    selectorLayout->addWidget(_refreshButton);

    auto* toolLayout = new QHBoxLayout;
    toolLayout->setObjectName(QStringLiteral("DeviceToolLayout"));
    toolLayout->setProperty("layoutRole", "actions");
    toolLayout->addWidget(_connectButton);
    toolLayout->addWidget(_initializeButton);
    toolLayout->addWidget(_grabOneButton);
    toolLayout->addWidget(_grabLiveButton);

    topLayout->addLayout(selectorLayout);
    topLayout->addLayout(toolLayout);
    rootLayout->addLayout(topLayout);

    auto* featureTreeLayout = new QVBoxLayout;
    featureTreeLayout->setObjectName(QStringLiteral("DeviceTreePanelLayout"));
    featureTreeLayout->setProperty("layoutRole", "bottom-panel");
    _featureTree = createFeatureTree(owner, QStringLiteral("HeliotisC4FeatureTree"),
                                     settingsFile, favoritesKey);
    featureTreeLayout->addWidget(_featureTree);
    rootLayout->addLayout(featureTreeLayout);

    _statusBar = new QStatusBar(owner);
    _statusBar->setObjectName(QStringLiteral("HeliotisC4StatusBar"));
    _statusBar->setSizeGripEnabled(false);
    _connectionStatus = new QLabel(owner);
    _connectionStatus->setObjectName(QStringLiteral("HeliotisC4StatusLabel"));
    _connectionStatus->setAlignment(Qt::AlignCenter);
    _statusBar->addWidget(_connectionStatus);
    rootLayout->addWidget(_statusBar);


}

void HeliotisC4ControlUi::updateLiveAction()
{
    const QString command = _grabLiveButton->isChecked()
        ? QCoreApplication::translate("QHeliotisC4Widget", "Stop live acquisition")
        : QCoreApplication::translate("QHeliotisC4Widget", "Start live acquisition");
    _grabLiveButton->setToolTip(command);
    _grabLiveButton->setAccessibleName(command);
}
