#include "MainWindow.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QHostInfo>
#include <QGuiApplication>
#include <QDebug>
#include <QCloseEvent>

const QString MainWindow::DEFAULT_SERVER_URL = "ws://localhost:8080";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_centralWidget(nullptr)
    , m_webSocketClient(new WebSocketClient(this))
    , m_statusUpdateTimer(new QTimer(this))
    , m_trayIcon(nullptr)
{
    // Check if system tray is available
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(this, "System Tray",
                             "System tray is not available on this system.");
    }
    
    setupUI();
    setupMenuBar();
    setupSystemTray();
    
    // Connect WebSocket signals
    connect(m_webSocketClient, &WebSocketClient::connected, this, &MainWindow::onConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_webSocketClient, &WebSocketClient::connectionError, this, &MainWindow::onConnectionError);
    connect(m_webSocketClient, &WebSocketClient::clientListReceived, this, &MainWindow::onClientListReceived);
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    
    // Setup status update timer
    m_statusUpdateTimer->setInterval(1000); // Update every second
    connect(m_statusUpdateTimer, &QTimer::timeout, this, &MainWindow::updateConnectionStatus);
    m_statusUpdateTimer->start();
    
    setWindowTitle("Mouffette - Media Sharing");
    resize(600, 500);
    
    // Set window flags to prevent showing in dock/taskbar when hidden
#ifdef Q_OS_MACOS
    setWindowFlags(windowFlags() | Qt::Tool);
#endif
    
    // Initially disable UI until connected
    setUIEnabled(false);
    
    // Start minimized to tray and auto-connect
    hide();
    connectToServer();
}

MainWindow::~MainWindow() {
    if (m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
    }
}

void MainWindow::setupUI() {
    m_centralWidget = new QWidget(this);
    setCentralWidget(m_centralWidget);
    
    m_mainLayout = new QVBoxLayout(m_centralWidget);
    m_mainLayout->setSpacing(20);
    m_mainLayout->setContentsMargins(20, 20, 20, 20);
    
    // Connection section
    m_connectionLayout = new QHBoxLayout();
    
    m_connectButton = new QPushButton("Connect to Server");
    m_connectButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectButtonClicked);
    
    m_refreshButton = new QPushButton("Refresh");
    m_refreshButton->setEnabled(false);
    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::onRefreshButtonClicked);
    
    m_connectionStatusLabel = new QLabel("Disconnected");
    m_connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
    
    m_connectionLayout->addWidget(m_connectButton);
    m_connectionLayout->addWidget(m_refreshButton);
    m_connectionLayout->addStretch();
    m_connectionLayout->addWidget(new QLabel("Status:"));
    m_connectionLayout->addWidget(m_connectionStatusLabel);
    
    m_mainLayout->addLayout(m_connectionLayout);
    
    // Client list section
    m_clientListLabel = new QLabel("Connected Clients:");
    m_clientListLabel->setStyleSheet("QLabel { font-size: 14px; font-weight: bold; }");
    m_mainLayout->addWidget(m_clientListLabel);
    
    m_clientListWidget = new QListWidget();
    m_clientListWidget->setStyleSheet(
        "QListWidget { "
        "   border: 2px solid #ccc; "
        "   border-radius: 5px; "
        "   padding: 5px; "
        "   background-color: #f9f9f9; "
        "} "
        "QListWidget::item { "
        "   padding: 10px; "
        "   border-bottom: 1px solid #eee; "
        "} "
        "QListWidget::item:selected { "
        "   background-color: #4a90e2; "
        "   color: white; "
        "}"
    );
    connect(m_clientListWidget, &QListWidget::itemSelectionChanged, this, &MainWindow::onClientSelectionChanged);
    m_mainLayout->addWidget(m_clientListWidget);
    
    m_noClientsLabel = new QLabel("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
    m_noClientsLabel->setStyleSheet("QLabel { color: #666; font-style: italic; text-align: center; }");
    m_noClientsLabel->setAlignment(Qt::AlignCenter);
    m_noClientsLabel->setWordWrap(true);
    m_mainLayout->addWidget(m_noClientsLabel);
    
    // Selected client info
    m_selectedClientLabel = new QLabel();
    m_selectedClientLabel->setStyleSheet("QLabel { background-color: #e8f4fd; padding: 10px; border-radius: 5px; }");
    m_selectedClientLabel->setWordWrap(true);
    m_selectedClientLabel->hide();
    m_mainLayout->addWidget(m_selectedClientLabel);
    
    // Initially show no clients message
    m_clientListWidget->hide();
    m_noClientsLabel->show();
}

void MainWindow::setupMenuBar() {
    // File menu
    m_fileMenu = menuBar()->addMenu("File");
    
    m_exitAction = new QAction("Quit Mouffette", this);
    m_exitAction->setShortcut(QKeySequence::Quit);
    connect(m_exitAction, &QAction::triggered, this, [this]() {
        if (m_webSocketClient->isConnected()) {
            m_webSocketClient->disconnect();
        }
        QApplication::quit();
    });
    m_fileMenu->addAction(m_exitAction);
    
    // Help menu
    m_helpMenu = menuBar()->addMenu("Help");
    
    m_aboutAction = new QAction("About", this);
    connect(m_aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "About Mouffette", 
            "Mouffette v1.0.0\n\n"
            "A cross-platform media sharing application that allows users to "
            "share and display media on other connected users' screens.\n\n"
            "Built with Qt and WebSocket technology.");
    });
    m_helpMenu->addAction(m_aboutAction);
}

void MainWindow::setupSystemTray() {
    // Create tray icon (no context menu, just click handling)
    m_trayIcon = new QSystemTrayIcon(this);
    
    // Set icon - try to load from resources, fallback to simple icon
    QIcon trayIconIcon(":/icons/mouffette.png");
    if (trayIconIcon.isNull()) {
        // Fallback to simple colored icon
        QPixmap pixmap(16, 16);
        pixmap.fill(Qt::blue);
        trayIconIcon = QIcon(pixmap);
    }
    m_trayIcon->setIcon(trayIconIcon);
    
    // Set tooltip
    m_trayIcon->setToolTip("Mouffette - Media Sharing");
    
    // Connect tray icon activation for non-context menu clicks
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::onTrayIconActivated);
    
    // Show the tray icon
    m_trayIcon->show();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_trayIcon && m_trayIcon->isVisible()) {
        // Hide to tray instead of closing
        hide();
        event->ignore();
        
        // Show message first time
        static bool firstHide = true;
        if (firstHide) {
            showTrayMessage("Mouffette", "Application is now running in the background. Click the tray icon to show the window again.");
            firstHide = false;
        }
    } else {
        event->accept();
    }
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    // Show/hide window on any click (left, right, or double-click)
    switch (reason) {
    case QSystemTrayIcon::Trigger:        // Single left-click
    case QSystemTrayIcon::DoubleClick:    // Double left-click  
    case QSystemTrayIcon::Context:        // Right-click
        if (isVisible()) {
            hide();
        } else {
            // Show the window
            show();
            raise();
            activateWindow();
        }
        break;
    default:
        break;
    }
}

void MainWindow::showTrayMessage(const QString& title, const QString& message) {
    if (m_trayIcon) {
        m_trayIcon->showMessage(title, message, QSystemTrayIcon::Information, 3000);
    }
}

void MainWindow::onConnectButtonClicked() {
    if (m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
        m_connectButton->setText("Connect to Server");
    } else {
        connectToServer();
        m_connectButton->setText("Disconnect");
    }
}

void MainWindow::onRefreshButtonClicked() {
    if (m_webSocketClient->isConnected()) {
        m_webSocketClient->requestClientList();
    }
}

void MainWindow::connectToServer() {
    m_webSocketClient->connectToServer(DEFAULT_SERVER_URL);
}

void MainWindow::onConnected() {
    setUIEnabled(true);
    m_connectButton->setText("Disconnect");
    m_refreshButton->setEnabled(true);
    
    // Register this client
    registerThisClient();
    
    statusBar()->showMessage("Connected to server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Connected", "Successfully connected to Mouffette server");
}

void MainWindow::onDisconnected() {
    setUIEnabled(false);
    m_connectButton->setText("Connect to Server");
    m_refreshButton->setEnabled(false);
    
    // Clear client list
    m_availableClients.clear();
    updateClientList(m_availableClients);
    
    statusBar()->showMessage("Disconnected from server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Disconnected", "Disconnected from Mouffette server");
}

void MainWindow::onConnectionError(const QString& error) {
    QMessageBox::warning(this, "Connection Error", 
        QString("Failed to connect to server:\n%1").arg(error));
    
    setUIEnabled(false);
    m_connectButton->setText("Connect to Server");
    m_refreshButton->setEnabled(false);
}

void MainWindow::onClientListReceived(const QList<ClientInfo>& clients) {
    qDebug() << "Received client list with" << clients.size() << "clients";
    
    // Check for new clients
    int previousCount = m_availableClients.size();
    m_availableClients = clients;
    updateClientList(clients);
    
    // Show notification if new clients appeared
    if (clients.size() > previousCount && previousCount >= 0) {
        int newClients = clients.size() - previousCount;
        if (newClients > 0) {
            QString message = QString("%1 new client%2 available for sharing")
                .arg(newClients)
                .arg(newClients == 1 ? "" : "s");
            showTrayMessage("New Clients Available", message);
        }
    }
}

void MainWindow::onRegistrationConfirmed(const ClientInfo& clientInfo) {
    m_thisClient = clientInfo;
    qDebug() << "Registration confirmed for:" << clientInfo.getMachineName();
    
    // Request initial client list
    m_webSocketClient->requestClientList();
}

void MainWindow::onClientSelectionChanged() {
    QListWidgetItem* currentItem = m_clientListWidget->currentItem();
    
    if (currentItem) {
        int index = m_clientListWidget->row(currentItem);
        if (index >= 0 && index < m_availableClients.size()) {
            const ClientInfo& client = m_availableClients[index];
            
            QString infoText = QString(
                "<b>Selected Client:</b> %1<br/>"
                "<b>Platform:</b> %2<br/>"
                "<b>Screens:</b> %3<br/>"
                "<b>Status:</b> %4"
            ).arg(client.getMachineName())
             .arg(client.getPlatform())
             .arg(client.getScreenCount())
             .arg(client.getStatus());
            
            // Add screen details
            if (!client.getScreens().isEmpty()) {
                infoText += "<br/><br/><b>Screen Details:</b><br/>";
                for (int i = 0; i < client.getScreens().size(); ++i) {
                    const ScreenInfo& screen = client.getScreens()[i];
                    infoText += QString("Screen %1: %2x%3%4<br/>")
                        .arg(i + 1)
                        .arg(screen.width)
                        .arg(screen.height)
                        .arg(screen.primary ? " (Primary)" : "");
                }
            }
            
            m_selectedClientLabel->setText(infoText);
            m_selectedClientLabel->show();
        }
    } else {
        m_selectedClientLabel->hide();
    }
}

void MainWindow::registerThisClient() {
    QString machineName = getMachineName();
    QString platform = getPlatformName();
    QList<ScreenInfo> screens = getLocalScreenInfo();
    
    qDebug() << "Registering client:" << machineName << "on" << platform << "with" << screens.size() << "screens";
    
    m_webSocketClient->registerClient(machineName, platform, screens);
}

QList<ScreenInfo> MainWindow::getLocalScreenInfo() {
    QList<ScreenInfo> screens;
    QList<QScreen*> screenList = QGuiApplication::screens();
    
    for (int i = 0; i < screenList.size(); ++i) {
        QScreen* screen = screenList[i];
        QRect geometry = screen->geometry();
        bool isPrimary = (screen == QGuiApplication::primaryScreen());
        
        ScreenInfo screenInfo(i, geometry.width(), geometry.height(), isPrimary);
        screens.append(screenInfo);
    }
    
    return screens;
}

QString MainWindow::getMachineName() {
    QString hostName = QHostInfo::localHostName();
    if (hostName.isEmpty()) {
        hostName = "Unknown Machine";
    }
    return hostName;
}

QString MainWindow::getPlatformName() {
#ifdef Q_OS_MACOS
    return "macOS";
#elif defined(Q_OS_WIN)
    return "Windows";
#elif defined(Q_OS_LINUX)
    return "Linux";
#else
    return "Unknown";
#endif
}

void MainWindow::updateClientList(const QList<ClientInfo>& clients) {
    m_clientListWidget->clear();
    
    if (clients.isEmpty()) {
        m_clientListWidget->hide();
        m_noClientsLabel->show();
    } else {
        m_noClientsLabel->hide();
        m_clientListWidget->show();
        
        for (const ClientInfo& client : clients) {
            QString displayText = client.getDisplayText();
            QListWidgetItem* item = new QListWidgetItem(displayText);
            item->setToolTip(QString("ID: %1\nStatus: %2").arg(client.getId()).arg(client.getStatus()));
            m_clientListWidget->addItem(item);
        }
    }
    
    // Hide selected client info when list changes
    m_selectedClientLabel->hide();
}

void MainWindow::setUIEnabled(bool enabled) {
    m_refreshButton->setEnabled(enabled);
    m_clientListWidget->setEnabled(enabled);
}

void MainWindow::updateConnectionStatus() {
    QString status = m_webSocketClient->getConnectionStatus();
    m_connectionStatusLabel->setText(status);
    
    if (status == "Connected") {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: green; font-weight: bold; }");
    } else if (status.startsWith("Connecting") || status.startsWith("Reconnecting")) {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: orange; font-weight: bold; }");
    } else {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
    }
}


