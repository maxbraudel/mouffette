#include "MainWindow.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QHostInfo>
#include <QGuiApplication>
#include <QDebug>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QStackedWidget>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#ifdef Q_OS_MACOS
#include <QProcess>
#endif

const QString MainWindow::DEFAULT_SERVER_URL = "ws://192.168.0.188:8080";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_centralWidget(nullptr)
    , m_webSocketClient(new WebSocketClient(this))
    , m_statusUpdateTimer(new QTimer(this))
    , m_trayIcon(nullptr)
    , m_screenViewWidget(nullptr)
    , m_screenCanvas(nullptr)
    , m_ignoreSelectionChange(false)
    , m_displaySyncTimer(new QTimer(this))
{
    // Check if system tray is available
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(this, "System Tray",
                             "System tray is not available on this system.");
    }
    
    setupUI();
    setupMenuBar();
    setupSystemTray();
    setupVolumeMonitoring();
    
    // Connect WebSocket signals
    connect(m_webSocketClient, &WebSocketClient::connected, this, &MainWindow::onConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_webSocketClient, &WebSocketClient::clientListReceived, this, &MainWindow::onClientListReceived);
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    connect(m_webSocketClient, &WebSocketClient::screensInfoReceived, this, &MainWindow::onScreensInfoReceived);
    
    // Setup status update timer
    m_statusUpdateTimer->setInterval(1000); // Update every second
    connect(m_statusUpdateTimer, &QTimer::timeout, this, &MainWindow::updateConnectionStatus);
    m_statusUpdateTimer->start();

    // Debounced display sync timer
    m_displaySyncTimer->setSingleShot(true);
    m_displaySyncTimer->setInterval(300); // debounce bursts of screen change signals
    connect(m_displaySyncTimer, &QTimer::timeout, this, [this]() {
        if (m_webSocketClient->isConnected()) syncRegistration();
    });

    // Listen to display changes to keep server-side screen info up-to-date
    auto connectScreenSignals = [this](QScreen* s) {
        connect(s, &QScreen::geometryChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::availableGeometryChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::physicalDotsPerInchChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::primaryOrientationChanged, this, [this]() { m_displaySyncTimer->start(); });
    };
    for (QScreen* s : QGuiApplication::screens()) connectScreenSignals(s);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, connectScreenSignals](QScreen* s) {
        connectScreenSignals(s);
        m_displaySyncTimer->start();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
        m_displaySyncTimer->start();
    });
    
    setWindowTitle("Mouffette - Media Sharing");
    resize(600, 500);
    
    // Set window flags to prevent showing in dock/taskbar when hidden
#ifdef Q_OS_MACOS
    setWindowFlags(windowFlags() | Qt::Tool);
    // Keep tool window visible even when app loses focus (prevents auto-hide on deactivate)
    setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
#endif
    
    // Initially disable UI until connected
    setUIEnabled(false);
    
    // Start minimized to tray and auto-connect
    hide();
    connectToServer();
}

void MainWindow::showScreenView(const ClientInfo& client) {
    qDebug() << "showScreenView called for client:" << client.getMachineName();
    
    // Update client name
    m_clientNameLabel->setText(QString("%1 (%2)").arg(client.getMachineName()).arg(client.getPlatform()));
    
    // Optimistic: show any cached screens quickly while fetching fresh data
    m_screenCanvas->setScreens(client.getScreens());
    // Request fresh screen info on demand
    if (!client.getId().isEmpty()) {
        m_webSocketClient->requestScreens(client.getId());
    }
    
    // Update volume indicator (placeholder for now)
    updateVolumeIndicator();
    
    // Switch to screen view page
    m_stackedWidget->setCurrentWidget(m_screenViewWidget);
    // Move focus to canvas and recenter with margin
    if (m_screenCanvas) {
        m_screenCanvas->setFocus(Qt::OtherFocusReason);
        m_screenCanvas->recenterWithMargin(33);
    }
    
    // Start watching this client's screens for real-time updates
    if (m_webSocketClient && m_webSocketClient->isConnected()) {
        if (!m_watchedClientId.isEmpty() && m_watchedClientId != client.getId()) {
            m_webSocketClient->unwatchScreens(m_watchedClientId);
        }
        if (!client.getId().isEmpty()) {
            m_webSocketClient->watchScreens(client.getId());
            m_watchedClientId = client.getId();
        }
    }
    qDebug() << "Screen view now showing. Current widget index:" << m_stackedWidget->currentIndex();
}

void MainWindow::showClientListView() {
    qDebug() << "showClientListView called. Current widget index before:" << m_stackedWidget->currentIndex();
    
    // Switch to client list page
    m_stackedWidget->setCurrentWidget(m_clientListPage);
    
    qDebug() << "Client list view now showing. Current widget index:" << m_stackedWidget->currentIndex();
    
    // Stop watching when leaving screen view
    if (m_webSocketClient && m_webSocketClient->isConnected() && !m_watchedClientId.isEmpty()) {
        m_webSocketClient->unwatchScreens(m_watchedClientId);
        m_watchedClientId.clear();
    }
    // Clear selection without triggering selection change event
    m_ignoreSelectionChange = true;
    m_clientListWidget->clearSelection();
    m_ignoreSelectionChange = false;
}

void MainWindow::onClientItemClicked(QListWidgetItem* item) {
    if (!item) return;
    int index = m_clientListWidget->row(item);
    if (index >= 0 && index < m_availableClients.size()) {
        const ClientInfo& client = m_availableClients[index];
        m_selectedClient = client;
        // Switch to screen view first with any cached info for immediate feedback
        showScreenView(client);
        // Then request fresh screens info on-demand
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            m_webSocketClient->requestScreens(client.getId());
        }
    }
}

void MainWindow::updateVolumeIndicator() {
    int vol = m_selectedClient.getVolumePercent();
    QString text;
    if (vol >= 0) {
        QString icon = (vol == 0) ? "🔇" : (vol < 34 ? "🔈" : (vol < 67 ? "🔉" : "🔊"));
        text = QString("%1 Volume: %2%%").arg(icon).arg(vol);
    } else {
        text = QString("🔈 Volume: --");
    }
    m_volumeIndicator->setText(text);
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
    // Swallow spacebar outside of the canvas to prevent accidental activations,
    // but let it reach the canvas for the recenter shortcut.
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space) {
            QWidget* w = qobject_cast<QWidget*>(obj);
            bool onCanvas = false;
            if (m_screenCanvas) {
                onCanvas = (w == m_screenCanvas) || (w && m_screenCanvas->isAncestorOf(w));
            }
            if (!onCanvas) {
                return true; // consume outside the canvas
            } else {
                // Recenter shortcut on canvas regardless of exact focus widget
                m_screenCanvas->recenterWithMargin(33);
                return true; // consume
            }
        }
    }
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
    
    // Enable pinch gesture
    grabGesture(Qt::PinchGesture);
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    clearScreens();
    createScreenItems();
    
    // Keep a large scene for free movement and center on screens
    const double LARGE_SCENE_SIZE = 100000.0;
    QRectF sceneRect(-LARGE_SCENE_SIZE/2, -LARGE_SCENE_SIZE/2, LARGE_SCENE_SIZE, LARGE_SCENE_SIZE);
    m_scene->setSceneRect(sceneRect);
    if (!m_screens.isEmpty()) {
        recenterWithMargin(53);
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

QRectF ScreenCanvas::screensBoundingRect() const {
    QRectF bounds;
    bool first = true;
    for (auto* item : m_screenItems) {
        if (!item) continue;
        QRectF r = item->sceneBoundingRect();
        if (first) { bounds = r; first = false; }
        else { bounds = bounds.united(r); }
    }
    return bounds;
}

void ScreenCanvas::recenterWithMargin(int marginPx) {
    QRectF bounds = screensBoundingRect();
    if (bounds.isNull() || !bounds.isValid()) return;
    // Convert pixel margin in view space to scene space using current scale
    qreal sx = transform().m11();
    qreal sy = transform().m22();
    qreal mx = sx != 0 ? marginPx / sx : marginPx;
    qreal my = sy != 0 ? marginPx / sy : marginPx;
    bounds.adjust(-mx, -my, mx, my);
    fitInView(bounds, Qt::KeepAspectRatio);
}

void ScreenCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space) {
        recenterWithMargin(53);
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

bool ScreenCanvas::event(QEvent* event) {
    if (event->type() == QEvent::Gesture) {
        return gestureEvent(static_cast<QGestureEvent*>(event));
    }
    return QGraphicsView::event(event);
}

bool ScreenCanvas::gestureEvent(QGestureEvent* event) {
    if (QGesture* pinch = event->gesture(Qt::PinchGesture)) {
        QPinchGesture* pinchGesture = static_cast<QPinchGesture*>(pinch);
        
        if (pinchGesture->changeFlags() & QPinchGesture::ScaleFactorChanged) {
            qreal scaleFactor = pinchGesture->scaleFactor();
            
            // Apply zoom based on pinch scale
            scale(scaleFactor, scaleFactor);
        }
        
        event->accept();
        return true;
    }
    return false;
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

void ScreenCanvas::wheelEvent(QWheelEvent* event) {
    // Prefer pixelDelta for trackpads (gives smooth 2D vector),
    // fallback to angleDelta for traditional mouse wheels.
    QPoint numPixels = event->pixelDelta();
    QPoint numDegrees = event->angleDelta();
    QPoint delta;
    if (!numPixels.isNull()) {
        delta = numPixels; // already in pixels, both axes
    } else if (!numDegrees.isNull()) {
        // angleDelta is in eighths of a degree; scale to pixels
        delta = numDegrees / 8; // small scaling for smoother feel
    }

    if (!delta.isNull()) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
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
    
    // Connection section (always visible)
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
    
    // Create stacked widget for page navigation
    m_stackedWidget = new QStackedWidget();
    // Block stray key events (like space) at the stack level
    m_stackedWidget->installEventFilter(this);
    m_mainLayout->addWidget(m_stackedWidget);
    
    // Create client list page
    createClientListPage();
    
    // Create screen view page  
    createScreenViewPage();
    
    // Start with client list page
    m_stackedWidget->setCurrentWidget(m_clientListPage);
}

void MainWindow::createClientListPage() {
    m_clientListPage = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_clientListPage);
    layout->setSpacing(15);
    layout->setContentsMargins(0, 0, 0, 0);
    
    // Client list section
    m_clientListLabel = new QLabel("Connected Clients:");
    m_clientListLabel->setStyleSheet("QLabel { font-size: 14px; font-weight: bold; }");
    layout->addWidget(m_clientListLabel);
    
    m_clientListWidget = new QListWidget();
    // Use palette-based colors so light/dark themes adapt automatically
    m_clientListWidget->setStyleSheet(
        "QListWidget { "
        "   border: 1px solid palette(mid); "
        "   border-radius: 5px; "
        "   padding: 5px; "
        "   background-color: palette(base); "
        "   color: palette(text); "
        "}" 
        "QListWidget::item { "
        "   padding: 10px; "
        "   border-bottom: 1px solid palette(midlight); "
        "}" 
        "QListWidget::item:selected { "
        "   background-color: palette(highlight); "
        "   color: palette(highlighted-text); "
        "}"
    );
    connect(m_clientListWidget, &QListWidget::itemClicked, this, &MainWindow::onClientItemClicked);
    // Prevent keyboard (space/enter) from triggering navigation
    m_clientListWidget->setFocusPolicy(Qt::NoFocus);
    m_clientListWidget->installEventFilter(this);
    layout->addWidget(m_clientListWidget);
    
    m_noClientsLabel = new QLabel("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
    m_noClientsLabel->setStyleSheet("QLabel { color: #666; font-style: italic; text-align: center; }");
    m_noClientsLabel->setAlignment(Qt::AlignCenter);
    m_noClientsLabel->setWordWrap(true);
    layout->addWidget(m_noClientsLabel);
    
    // Selected client info
    m_selectedClientLabel = new QLabel();
    m_selectedClientLabel->setStyleSheet("QLabel { background-color: #e8f4fd; padding: 10px; border-radius: 5px; }");
    m_selectedClientLabel->setWordWrap(true);
    m_selectedClientLabel->hide();
    layout->addWidget(m_selectedClientLabel);
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_clientListPage);
    
    // Initially show no clients message
    m_clientListWidget->hide();
    m_noClientsLabel->show();
}

void MainWindow::createScreenViewPage() {
    // Screen view page
    m_screenViewWidget = new QWidget();
    m_screenViewLayout = new QVBoxLayout(m_screenViewWidget);
    m_screenViewLayout->setSpacing(15);
    m_screenViewLayout->setContentsMargins(0, 0, 0, 0);
    
    // Client name and back button
    QHBoxLayout* headerLayout = new QHBoxLayout();
    m_backButton = new QPushButton("← Back to Client List");
    m_backButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    connect(m_backButton, &QPushButton::clicked, this, &MainWindow::onBackToClientListClicked);
    // Prevent space/enter from triggering navigation accidentally
    m_backButton->setAutoDefault(false);
    m_backButton->setDefault(false);
    m_backButton->setFocusPolicy(Qt::NoFocus);
    
    m_clientNameLabel = new QLabel();
    m_clientNameLabel->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    
    headerLayout->addWidget(m_backButton);
    headerLayout->addStretch();
    headerLayout->addWidget(m_clientNameLabel);
    headerLayout->addStretch();
    
    m_screenViewLayout->addLayout(headerLayout);
    
    // Volume indicator
    m_volumeIndicator = new QLabel("� Volume: --");
    // Use palette(window-text) so it remains readable in light/dark modes
    m_volumeIndicator->setStyleSheet("QLabel { font-size: 14px; color: palette(window-text); padding: 5px; }");
    m_volumeIndicator->setAlignment(Qt::AlignCenter);
    m_screenViewLayout->addWidget(m_volumeIndicator);
    
    // Screen canvas
    m_screenCanvas = new ScreenCanvas();
    m_screenCanvas->setMinimumHeight(400);
    m_screenCanvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(m_screenCanvas, &ScreenCanvas::screenClicked, this, &MainWindow::onScreenClicked);
    // Ensure focus is on canvas, and block stray key events
    m_screenViewWidget->installEventFilter(this);
    m_screenCanvas->setFocusPolicy(Qt::StrongFocus);
    m_screenCanvas->installEventFilter(this);
    // Give canvas stretch so it occupies remaining space
    m_screenViewLayout->addWidget(m_screenCanvas, 1);
    
    // Send button
    m_sendButton = new QPushButton("Send Media to All Screens");
    m_sendButton->setStyleSheet("QPushButton { padding: 12px 24px; font-weight: bold; background-color: #4a90e2; color: white; border-radius: 5px; }");
    m_sendButton->setEnabled(false); // Initially disabled until media is placed
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::onSendMediaClicked);
    // Keep button at bottom, centered
    m_screenViewLayout->addWidget(m_sendButton, 0, Qt::AlignHCenter);
    // Ensure header/volume have no stretch, canvas expands, button fixed
    m_screenViewLayout->setStretch(0, 0); // header
    m_screenViewLayout->setStretch(1, 0); // volume indicator
    m_screenViewLayout->setStretch(2, 1); // canvas expands
    m_screenViewLayout->setStretch(3, 0); // button fixed
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_screenViewWidget);
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
    
    // Sync this client's info with the server
    syncRegistration();
    
    statusBar()->showMessage("Connected to server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Connected", "Successfully connected to Mouffette server");
}

void MainWindow::onDisconnected() {
    setUIEnabled(false);
    m_connectButton->setText("Connect to Server");
    m_refreshButton->setEnabled(false);
    
    // Stop watching if any
    if (!m_watchedClientId.isEmpty()) {
        m_watchedClientId.clear();
    }
    
    // Clear client list
    m_availableClients.clear();
    updateClientList(m_availableClients);
    
    statusBar()->showMessage("Disconnected from server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Disconnected", "Disconnected from Mouffette server");
}

void MainWindow::startWatchingSelectedClient() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) return;
    const QString targetId = m_selectedClient.getId();
    if (targetId.isEmpty()) return;
    if (m_watchedClientId == targetId) return; // already watching
    if (!m_watchedClientId.isEmpty()) {
        m_webSocketClient->unwatchScreens(m_watchedClientId);
    }
    m_webSocketClient->watchScreens(targetId);
    m_watchedClientId = targetId;
}

void MainWindow::stopWatchingCurrentClient() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) { m_watchedClientId.clear(); return; }
    if (m_watchedClientId.isEmpty()) return;
    m_webSocketClient->unwatchScreens(m_watchedClientId);
    m_watchedClientId.clear();
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
    if (m_ignoreSelectionChange) {
        return;
    }
    
    QListWidgetItem* currentItem = m_clientListWidget->currentItem();
    
    if (currentItem) {
        int index = m_clientListWidget->row(currentItem);
        if (index >= 0 && index < m_availableClients.size()) {
            const ClientInfo& client = m_availableClients[index];
            m_selectedClient = client;
            
            // Show the screen view for the selected client
            showScreenView(client);
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                m_webSocketClient->requestScreens(client.getId());
            }
        }
    } else {
        m_selectedClientLabel->hide();
    }
}

// (Duplicate removed) onScreensInfoReceived is implemented later in the file

void MainWindow::syncRegistration() {
    QString machineName = getMachineName();
    QString platform = getPlatformName();
    QList<ScreenInfo> screens = getLocalScreenInfo();
    int volumePercent = getSystemVolumePercent();
    
    qDebug() << "Sync registration:" << machineName << "on" << platform << "with" << screens.size() << "screens";
    
    m_webSocketClient->registerClient(machineName, platform, screens, volumePercent);
}

void MainWindow::onScreensInfoReceived(const ClientInfo& clientInfo) {
    // Update the canvas only if it matches the currently selected client
    if (!clientInfo.getId().isEmpty() && clientInfo.getId() == m_selectedClient.getId()) {
        qDebug() << "Updating canvas with fresh screens for" << clientInfo.getMachineName();
        m_selectedClient = clientInfo; // keep selected client in sync
        if (m_screenCanvas) {
            m_screenCanvas->setScreens(clientInfo.getScreens());
            m_screenCanvas->recenterWithMargin(33);
        }
        // Optional: refresh UI labels if platform/machine changed
        m_clientNameLabel->setText(QString("%1 (%2)").arg(clientInfo.getMachineName()).arg(clientInfo.getPlatform()));
    // Refresh volume UI based on latest info
    updateVolumeIndicator();
    }
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

int MainWindow::getSystemVolumePercent() {
#ifdef Q_OS_MACOS
    // Query macOS system output volume (0-100) via AppleScript
    QProcess proc;
    proc.start("/usr/bin/osascript", {"-e", "output volume of (get volume settings)"});
    if (!proc.waitForFinished(1000)) return -1;
    QByteArray out = proc.readAllStandardOutput().trimmed();
    bool ok = false;
    int vol = QString::fromUtf8(out).toInt(&ok);
    if (!ok) return -1;
    vol = std::clamp(vol, 0, 100);
    return vol;
#elif defined(Q_OS_WIN)
    // Use Windows Core Audio APIs (MMDevice + IAudioEndpointVolume)
    // Headers are included only on Windows builds.
    #include <windows.h>
    #include <mmdeviceapi.h>
    #include <endpointvolume.h>
    HRESULT hr;
    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioEndpointVolume* pEndpointVol = nullptr;
    bool coInit = SUCCEEDED(CoInitialize(nullptr));
    int result = -1;
    do {
        hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnum);
        if (FAILED(hr) || !pEnum) break;
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        if (FAILED(hr) || !pDevice) break;
        hr = pDevice->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, (void**)&pEndpointVol);
        if (FAILED(hr) || !pEndpointVol) break;
        float volScalar = 0.0f;
        hr = pEndpointVol->GetMasterVolumeLevelScalar(&volScalar);
        if (FAILED(hr)) break;
        int vol = static_cast<int>(std::round(volScalar * 100.0f));
        vol = std::clamp(vol, 0, 100);
        result = vol;
    } while (false);
    if (pEndpointVol) pEndpointVol->Release();
    if (pDevice) pDevice->Release();
    if (pEnum) pEnum->Release();
    if (coInit) CoUninitialize();
    return result;
#elif defined(Q_OS_LINUX)
    return -1; // TODO: Implement via PulseAudio/PipeWire if needed
#else
    return -1;
#endif
}

void MainWindow::setupVolumeMonitoring() {
    // Poll system volume and sync to server when it changes.
    QTimer* volTimer = new QTimer(this);
    volTimer->setInterval(1200); // ~1.2s cadence
    connect(volTimer, &QTimer::timeout, this, [this]() {
        static int last = -2; // sentinel distinct from unknown -1
        int v = getSystemVolumePercent();
        if (v != last) {
            last = v;
            if (m_webSocketClient->isConnected()) {
                syncRegistration(); // includes volumePercent now
            }
        }
    });
    volTimer->start();
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


