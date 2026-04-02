#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>

namespace Ui {
class ChatWindow;
}

class ChatWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ChatWindow(QWidget *parent = nullptr);
    ~ChatWindow();

    void setNickname(const QString &nickname);
    void setRoomName(const QString &roomName);
    void setSocket(QTcpSocket *socket);
    void setIsOwner(bool isOwner);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void on_sendButton_clicked();
    void on_leaveButton_clicked();
    void on_kickButton_clicked();
    void on_muteButton_clicked();
    void onSocketDisconnected();
    void onReadyRead();
    void on_dismissButton_clicked();

private:
    Ui::ChatWindow *ui;
    QTcpSocket *m_socket;
    QString m_nickname;
    QString m_roomName;
    QByteArray m_receiveBuffer;
    bool m_isOwner;
    bool m_closedByServer = false;
    bool m_isDismissing = false;
    bool m_isLeaving = false;

    void addSystemMessage(const QString &msg);
    void sendPacket(const QString &type, const QString &data);
    void parseMessage(const QByteArray &line);
};

#endif