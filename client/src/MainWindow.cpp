#include "MainWindow.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QHostInfo>
#include <QGuiApplication>
#include <QDebug>
#include <QCloseEvent>
#include <QTimer>
#include <algorithm>

const QString MainWindow::DEFAULT_SERVER_URL = "ws://192.168.0.188:8080";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_centralWidget(nullptr)
    , m_webSocketClient(new WebSocketClient(this))
    , m_statusUpdateTimer(new QTimer(this))
    , m_trayIcon(nullptr)
    , m_screenViewWidget(nullptr)
    , m_screenCanvas(nullptr)
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

void MainWindow::showScreenView(const ClientInfo& client) {
    // Hide client list view
    m_clientListLabel->hide();
    m_clientListWidget->hide();
    m_noClientsLabel->hide();
    m_selectedClientLabel->hide();
    
    // Update client name
    m_clientNameLabel->setText(QString("%1 (%2)").arg(client.getMachineName()).arg(client.getPlatform()));
    
    // Set screens in the canvas
    m_screenCanvas->setScreens(client.getScreens());
    
    // Update volume indicator (placeholder for now)
    updateVolumeIndicator();
    
    // Show screen view
    m_screenViewWidget->show();
}

void MainWindow::showClientListView() {
    // Hide screen view
    m_screenViewWidget->hide();
    
    // Show client list view
    m_clientListLabel->show();
    if (m_availableClients.isEmpty()) {
        m_noClientsLabel->show();
        m_clientListWidget->hide();
    } else {
        m_noClientsLabel->hide();
        m_clientListWidget->show();
    }
    
    // Clear selection
    m_clientListWidget->clearSelection();
}

void MainWindow::updateVolumeIndicator() {
    // Placeholder implementation - in real app this would get actual volume
    // For now, show medium volume as demonstration
    m_volumeIndicator->setText("Volume: 🔉 Medium");
}

void MainWindow::onBackToClientListClicked() {
    showClientListView();
}

void MainWindow::onSendMediaClicked() {
    // Placeholder implementation
    QMessageBox::information(this, "Send Media", 
        QString("Sending media to %1's screens...\n\nThis feature will be implemented in the next phase.")
        .arg(m_selectedClient.getMachineName()));
}

void MainWindow::onScreenClicked(int screenId) {
    // Placeholder implementation
    QMessageBox::information(this, "Screen Selected", 
        QString("Selected screen %1 for media placement.\n\nIndividual screen editing will be implemented in the next phase.")
        .arg(screenId + 1));
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        QWidget* widget = qobject_cast<QWidget*>(obj);
        if (widget && widget->property("screenIndex").isValid()) {
            int screenIndex = widget->property("screenIndex").toInt();
            onScreenClicked(screenIndex);
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ScreenCanvas implementation
ScreenCanvas::ScreenCanvas(QWidget* parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
    , m_panning(false)
{
    setScene(m_scene);
    setDragMode(QGraphicsView::NoDrag);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setBackgroundBrush(QBrush(QColor(45, 45, 45)));
    setRenderHint(QPainter::Antialiasing);
    
    // Enable mouse tracking for panning
    setMouseTracking(true);
    
    // Enable keyboard focus for key events
    setFocusPolicy(Qt::StrongFocus);
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    clearScreens();
    createScreenItems();
    
    // Fit all screens in view
    if (!m_screens.isEmpty()) {
        // Set a very large scene rect to allow free movement
        const double LARGE_SCENE_SIZE = 100000.0;
        QRectF sceneRect(-LARGE_SCENE_SIZE/2, -LARGE_SCENE_SIZE/2, LARGE_SCENE_SIZE, LARGE_SCENE_SIZE);
        m_scene->setSceneRect(sceneRect);
        
        // Delay centering to ensure the view is properly initialized
        QTimer::singleShot(50, this, [this]() {
            centerOnScreens();
        });
    }
}

void ScreenCanvas::clearScreens() {
    // Clear existing screen items
    for (QGraphicsRectItem* item : m_screenItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_screenItems.clear();
}

void ScreenCanvas::centerOnScreens(int margin) {
    if (m_screenItems.isEmpty()) {
        return;
    }
    
    // Calculate bounding rect of all screen items
    QRectF boundingRect;
    for (QGraphicsRectItem* item : m_screenItems) {
        if (boundingRect.isNull()) {
            boundingRect = item->boundingRect().translated(item->pos());
        } else {
            boundingRect = boundingRect.united(item->boundingRect().translated(item->pos()));
        }
    }
    
    // Add margin to the bounding rect
    boundingRect = boundingRect.adjusted(-margin, -margin, margin, margin);
    
    // Fit the view to show all screens with margin
    fitInView(boundingRect, Qt::KeepAspectRatio);
}

void ScreenCanvas::createScreenItems() {
    const double SCALE_FACTOR = 0.2; // Scale down screens to 20% of their actual size
    const double SCREEN_SPACING = 5.0; // Small gap between adjacent screens
    
    // Calculate compact positioning
    QMap<int, QRectF> compactPositions = calculateCompactPositions(SCALE_FACTOR, SCREEN_SPACING);
    
    for (int i = 0; i < m_screens.size(); ++i) {
        const ScreenInfo& screen = m_screens[i];
        QGraphicsRectItem* screenItem = createScreenItem(screen, i, compactPositions[i]);
        m_screenItems.append(screenItem);
        m_scene->addItem(screenItem);
    }
}

QGraphicsRectItem* ScreenCanvas::createScreenItem(const ScreenInfo& screen, int index, const QRectF& position) {
    const double SCALE_FACTOR = 0.2;
    
    // Use the provided compact position instead of OS coordinates
    QGraphicsRectItem* item = new QGraphicsRectItem(position);
    
    // Set appearance
    if (screen.primary) {
        item->setBrush(QBrush(QColor(74, 144, 226, 180))); // Primary screen - blue
        item->setPen(QPen(QColor(74, 144, 226), 3));
    } else {
        item->setBrush(QBrush(QColor(80, 80, 80, 180))); // Secondary screen - gray
        item->setPen(QPen(QColor(160, 160, 160), 2));
    }
    
    // Store screen index for click handling
    item->setData(0, index);
    
    // Add screen label
    QGraphicsTextItem* label = new QGraphicsTextItem(QString("Screen %1\n%2×%3")
        .arg(index + 1)
        .arg(screen.width)
        .arg(screen.height));
    label->setDefaultTextColor(Qt::white);
    label->setFont(QFont("Arial", 12, QFont::Bold));
    
    // Center the label on the screen
    QRectF labelRect = label->boundingRect();
    QRectF screenRect = item->rect();
    label->setPos(screenRect.center() - labelRect.center());
    label->setParentItem(item);
    
    return item;
}

QMap<int, QRectF> ScreenCanvas::calculateCompactPositions(double scaleFactor, double spacing) const {
    QMap<int, QRectF> positions;
    
    if (m_screens.isEmpty()) {
        return positions;
    }
    
    // Group screens by their relative positioning
    // For now, implement a simple left-to-right, top-to-bottom layout
    // that maintains relative positioning but with minimal gaps
    
    // First, sort screens by their actual position (left to right, then top to bottom)
    QList<QPair<int, ScreenInfo>> screenPairs;
    for (int i = 0; i < m_screens.size(); ++i) {
        screenPairs.append(qMakePair(i, m_screens[i]));
    }
    
    // Sort by Y first (top to bottom), then by X (left to right)
    std::sort(screenPairs.begin(), screenPairs.end(), 
              [](const QPair<int, ScreenInfo>& a, const QPair<int, ScreenInfo>& b) {
                  if (qAbs(a.second.y - b.second.y) < 100) { // If roughly same height
                      return a.second.x < b.second.x; // Sort by X
                  }
                  return a.second.y < b.second.y; // Sort by Y
              });
    
    // Layout screens with compact positioning
    double currentX = 0;
    double currentY = 0;
    double rowHeight = 0;
    int lastY = INT_MIN;
    
    for (const auto& pair : screenPairs) {
        int index = pair.first;
        const ScreenInfo& screen = pair.second;
        
        double screenWidth = screen.width * scaleFactor;
        double screenHeight = screen.height * scaleFactor;
        
        // Start new row if Y position changed significantly
        if (lastY != INT_MIN && qAbs(screen.y - lastY) > 100) {
            currentX = 0;
            currentY += rowHeight + spacing;
            rowHeight = 0;
        }
        
        // Position the screen
        QRectF rect(currentX, currentY, screenWidth, screenHeight);
        positions[index] = rect;
        
        // Update for next screen
        currentX += screenWidth + spacing;
        rowHeight = qMax(rowHeight, screenHeight);
        lastY = screen.y;
    }
    
    return positions;
}

void ScreenCanvas::wheelEvent(QWheelEvent* event) {
    // Zoom in/out with mouse wheel
    const double scaleFactor = 1.15;
    if (event->angleDelta().y() > 0) {
        // Zoom in
        scale(scaleFactor, scaleFactor);
    } else {
        // Zoom out
        scale(1.0 / scaleFactor, 1.0 / scaleFactor);
    }
    event->accept();
}

void ScreenCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // Check if clicking on a screen item
        QGraphicsItem* item = itemAt(event->pos());
        if (item && item->type() == QGraphicsRectItem::Type) {
            QGraphicsRectItem* rectItem = qgraphicsitem_cast<QGraphicsRectItem*>(item);
            if (rectItem && rectItem->data(0).isValid()) {
                int screenIndex = rectItem->data(0).toInt();
                emit screenClicked(screenIndex);
                return;
            }
        }
        
        // Start panning
        m_panning = true;
        m_lastPanPoint = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
    QGraphicsView::mousePressEvent(event);
}

void ScreenCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (m_panning) {
        // Pan the view
        QPoint delta = event->pos() - m_lastPanPoint;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        m_lastPanPoint = event->pos();
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ScreenCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_panning) {
        m_panning = false;
        setCursor(Qt::ArrowCursor);
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ScreenCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space) {
        // Recenter the view on screens when space is pressed
        centerOnScreens();
        event->accept();
    } else {
        QGraphicsView::keyPressEvent(event);
    }
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
    
    // Screen view (initially hidden)
    m_screenViewWidget = new QWidget();
    m_screenViewLayout = new QVBoxLayout(m_screenViewWidget);
    
    // Client name and back button
    QHBoxLayout* headerLayout = new QHBoxLayout();
    m_backButton = new QPushButton("← Back to Client List");
    m_backButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    connect(m_backButton, &QPushButton::clicked, this, &MainWindow::onBackToClientListClicked);
    
    m_clientNameLabel = new QLabel();
    m_clientNameLabel->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    
    headerLayout->addWidget(m_backButton);
    headerLayout->addStretch();
    headerLayout->addWidget(m_clientNameLabel);
    headerLayout->addStretch();
    
    m_screenViewLayout->addLayout(headerLayout);
    
    // Volume indicator
    m_volumeIndicator = new QLabel("🔊 Volume: Medium");
    m_volumeIndicator->setStyleSheet("QLabel { font-size: 14px; color: #333; padding: 5px; }");
    m_volumeIndicator->setAlignment(Qt::AlignCenter);
    m_screenViewLayout->addWidget(m_volumeIndicator);
    
    // Screen canvas
    m_screenCanvas = new ScreenCanvas();
    m_screenCanvas->setMinimumHeight(400);
    connect(m_screenCanvas, &ScreenCanvas::screenClicked, this, &MainWindow::onScreenClicked);
    m_screenViewLayout->addWidget(m_screenCanvas);
    
    // Send button
    m_sendButton = new QPushButton("Send Media to All Screens");
    m_sendButton->setStyleSheet("QPushButton { padding: 12px 24px; font-weight: bold; background-color: #4a90e2; color: white; border-radius: 5px; }");
    m_sendButton->setEnabled(false); // Initially disabled until media is placed
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::onSendMediaClicked);
    m_screenViewLayout->addWidget(m_sendButton);
    
    m_mainLayout->addWidget(m_screenViewWidget);
    m_screenViewWidget->hide(); // Initially hidden
    
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
            m_selectedClient = client;
            
            // Show the screen view for the selected client
            showScreenView(client);
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
        
        ScreenInfo screenInfo(i, geometry.width(), geometry.height(), geometry.x(), geometry.y(), isPrimary);
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


