#ifndef LOBBYWINDOW_H
#define LOBBYWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QListWidgetItem>

namespace Ui {
class LobbyWindow;
}

class LobbyWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit LobbyWindow(QWidget *parent = nullptr);
    ~LobbyWindow();

    void setNickname(const QString &nickname);
    void setSocket(QTcpSocket *socket);
    void detachSocket();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onCreateRoom();
    void onJoinRoom();
    void onExit();
    void onSocketDisconnected();
    void onReadyRead();

private:
    Ui::LobbyWindow *ui;
    QTcpSocket *m_socket;
    QString m_nickname;
    QByteArray m_receiveBuffer;

    void sendPacket(const QString &type, const QString &data);
    void parseMessage(const QByteArray &line);
    void updateRoomList(const QString &roomsData);
};

#endif