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
#include "WebSocketClient.h"
#include "ClientInfo.h"

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
QT_END_NAMESPACE

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
    void updateConnectionStatus();
    
    // System tray slots
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupUI();
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

    // UI Components
    QWidget* m_centralWidget;
    QVBoxLayout* m_mainLayout;
    
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
    QTimer* m_statusUpdateTimer;
    
    // Constants
    static const QString DEFAULT_SERVER_URL;
};

#endif // MAINWINDOW_H
