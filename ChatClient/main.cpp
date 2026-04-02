#include <QApplication>
#include <QMessageBox>
#include <QTcpSocket>
#include <QTimer>
#include "authdialog.h"
#include "lobbywindow.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    while (true) {
        AuthDialog dlg;
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }

        QString username = dlg.getUsername();
        QString password = dlg.getPassword();
        bool isRegister = !dlg.isLogin();

        QTcpSocket *socket = new QTcpSocket();
        socket->connectToHost("127.0.0.1", 8888);
        if (!socket->waitForConnected(3000)) {
            QMessageBox::critical(nullptr, "错误", "连接服务器失败");
            delete socket;
            continue;
        }

        bool authSuccess = false;
        bool needRetry = false;

        if (isRegister) {
            socket->write(QString("REG|%1|%2\n").arg(username, password).toUtf8());
            socket->flush();

            QByteArray buffer;
            while (true) {
                if (!socket->waitForReadyRead(5000)) {
                    QMessageBox::critical(nullptr, "错误", "服务器无响应");
                    needRetry = true;
                    break;
                }
                buffer.append(socket->readAll());
                int pos = buffer.indexOf('\n');
                if (pos != -1) {
                    QByteArray line = buffer.left(pos);
                    buffer.remove(0, pos + 1);
                    QString str = QString::fromUtf8(line).trimmed();
                    if (str == "REG_OK") {
                        QMessageBox::information(nullptr, "提示", "注册成功，正在自动登录...");
                        socket->write(QString("AUTH|%1|%2\n").arg(username, password).toUtf8());
                        socket->flush();
                        break;
                    } else if (str == "REG_FAIL") {
                        QMessageBox::critical(nullptr, "错误", "注册失败，用户名可能已存在");
                        needRetry = true;
                        break;
                    }
                }
            }
            if (needRetry) {
                socket->disconnectFromHost();
                socket->waitForDisconnected();
                delete socket;
                continue;
            }
    }

    if (!isRegister) {
        socket->write(QString("AUTH|%1|%2\n").arg(username, password).toUtf8());
        socket->flush();

        QByteArray buffer;
        while (true) {
            if (!socket->waitForReadyRead(5000)) {
                QMessageBox::critical(nullptr, "错误", "服务器无响应");
                needRetry = true;
                break;
            }
            buffer.append(socket->readAll());
            int pos = buffer.indexOf('\n');
            if (pos != -1) {
                QByteArray line = buffer.left(pos);
                buffer.remove(0, pos + 1);
                QString str = QString::fromUtf8(line).trimmed();
                if (str.startsWith("AUTH_OK|")) {
                    QString nickname = str.mid(8);
                    LobbyWindow *lobby = new LobbyWindow();
                    lobby->setAttribute(Qt::WA_DeleteOnClose);
                    lobby->setNickname(nickname);
                    lobby->setSocket(socket);
                    lobby->show();
                    authSuccess = true;
                    break;
                } else if (str == "AUTH_FAIL") {
                    QMessageBox::critical(nullptr, "错误", "登录失败，用户名或密码错误");
                    break;
                }
            }
        }

        if (needRetry) {
            if (socket) {
                socket->disconnectFromHost();
                socket->waitForDisconnected();
                delete socket;
            }
            continue;
        }

        if (!authSuccess) {
            if (socket) {
                socket->disconnectFromHost();
                socket->waitForDisconnected();
                delete socket;
            }
            continue;
        }
    }

    // 进入事件循环，等待大厅窗口关闭
    int result = a.exec();
    if (result == 0) {
        return 0;
    }
    // 大厅窗口关闭后，继续 while 循环，重新显示认证对话框
    // socket 会被 lobby 窗口析构时删除，无需额外处理
    }

    return 0;
}