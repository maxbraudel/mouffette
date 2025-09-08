#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStatusBar>
#include <QTimer>
#include <QScreen>
#include <QApplication>
#include <QSystemTrayIcon>
#include <QCloseEvent>
#include <QScrollArea>
#include <QWidget>
#include <QEvent>
#include <QMouseEvent>
#include <QMessageBox>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QScrollBar>
#include <QStackedWidget>
#include "WebSocketClient.h"
#include "ClientInfo.h"

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
QT_END_NAMESPACE

// Custom screen canvas widget with zoom and pan capabilities
class ScreenCanvas : public QGraphicsView {
    Q_OBJECT

public:
    explicit ScreenCanvas(QWidget* parent = nullptr);
    void setScreens(const QList<ScreenInfo>& screens);
    void clearScreens();
    void recenterWithMargin(int marginPx = 33);

signals:
    void screenClicked(int screenIndex);

protected:
    bool gestureEvent(QGestureEvent* event);
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QGraphicsScene* m_scene;
    QList<QGraphicsRectItem*> m_screenItems;
    QList<ScreenInfo> m_screens;
    bool m_panning;
    QPoint m_lastPanPoint;
    
    void createScreenItems();
    QGraphicsRectItem* createScreenItem(const ScreenInfo& screen, int index, const QRectF& position);
    QMap<int, QRectF> calculateCompactPositions(double scaleFactor, double spacing) const;
    QRectF screensBoundingRect() const;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& error);
    void onClientListReceived(const QList<ClientInfo>& clients);
    void onRegistrationConfirmed(const ClientInfo& clientInfo);
    void onConnectButtonClicked();
    void onRefreshButtonClicked();
    void onClientSelectionChanged();
    void onClientItemClicked(QListWidgetItem* item);
    void updateConnectionStatus();
    
    // Screen view slots
    void onBackToClientListClicked();
    void onSendMediaClicked();
    void onScreenClicked(int screenId);
    
    // System tray slots
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void setupUI();
    void createClientListPage();
    void createScreenViewPage();
    void setupMenuBar();
    void setupSystemTray();
    void connectToServer();
    void registerThisClient();
    QList<ScreenInfo> getLocalScreenInfo();
    QString getMachineName();
    QString getPlatformName();
    void updateClientList(const QList<ClientInfo>& clients);
    void setUIEnabled(bool enabled);
    void showTrayMessage(const QString& title, const QString& message);
    
    // Screen view methods
    void showScreenView(const ClientInfo& client);
    void showClientListView();
    QWidget* createScreenWidget(const ScreenInfo& screen, int index);
    void updateVolumeIndicator();

    // UI Components
    QWidget* m_centralWidget;
    QVBoxLayout* m_mainLayout;
    QStackedWidget* m_stackedWidget;
    
    // Client list page
    QWidget* m_clientListPage;
    
    // Connection section
    QHBoxLayout* m_connectionLayout;
    QPushButton* m_connectButton;
    QPushButton* m_refreshButton;
    QLabel* m_connectionStatusLabel;
    
    // Client list section
    QLabel* m_clientListLabel;
    QListWidget* m_clientListWidget;
    QLabel* m_noClientsLabel;
    
    // Selected client info
    QLabel* m_selectedClientLabel;
    
    // Screen view section
    QWidget* m_screenViewWidget;
    QVBoxLayout* m_screenViewLayout;
    QLabel* m_clientNameLabel;
    ScreenCanvas* m_screenCanvas;
    QLabel* m_volumeIndicator;
    QPushButton* m_sendButton;
    QPushButton* m_backButton;
    
    // Menu and actions
    QMenu* m_fileMenu;
    QMenu* m_helpMenu;
    QAction* m_exitAction;
    QAction* m_aboutAction;
    
    // System tray
    QSystemTrayIcon* m_trayIcon;
    
    // Backend
    WebSocketClient* m_webSocketClient;
    QList<ClientInfo> m_availableClients;
    ClientInfo m_thisClient;
    ClientInfo m_selectedClient;
    QTimer* m_statusUpdateTimer;
    
    // Navigation state
    bool m_ignoreSelectionChange;
    
    // Constants
    static const QString DEFAULT_SERVER_URL;
};

#endif // MAINWINDOW_H
