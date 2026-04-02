#include "chatwindow.h"
#include "ui_chatwindow.h"
#include "lobbywindow.h"
#include <QApplication>
#include <QMessageBox>
#include <QDebug>
#include <QTimer>
#include <QCloseEvent>
#include <QInputDialog>

ChatWindow::ChatWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::ChatWindow),
    m_socket(nullptr)
{
    ui->setupUi(this);
    connect(ui->sendButton, &QPushButton::clicked, this, &ChatWindow::on_sendButton_clicked);
    connect(ui->leaveButton, &QPushButton::clicked, this, &ChatWindow::on_leaveButton_clicked);
    connect(ui->lineEdit, &QLineEdit::returnPressed, this, &ChatWindow::on_sendButton_clicked);
}

ChatWindow::~ChatWindow()
{
    delete ui;
}

void ChatWindow::setNickname(const QString &nickname)
{
    m_nickname = nickname;
}

void ChatWindow::setRoomName(const QString &roomName)
{
    m_roomName = roomName;
    setWindowTitle(QString("聊天室 - %1 - %2").arg(m_nickname, m_roomName));
}

void ChatWindow::setSocket(QTcpSocket *socket)
{
    m_socket = socket;
    connect(m_socket, &QTcpSocket::readyRead, this, &ChatWindow::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ChatWindow::onSocketDisconnected);
}

void ChatWindow::setIsOwner(bool isOwner)
{
    m_isOwner = isOwner;
    // 房主显示管理按钮
    ui->kickButton->setVisible(isOwner);
    ui->muteButton->setVisible(isOwner);
    // 所有人都显示退出按钮
    ui->leaveButton->setVisible(true);
}

void ChatWindow::closeEvent(QCloseEvent *event) {
    static bool closing = false;
    if (closing) {
        event->accept();
        return;
    }
    closing = true;

    if (m_closedByServer) {
        // 关键：返回大厅时重新接管socket
        if (parentWidget()) {
            LobbyWindow *lobby = qobject_cast<LobbyWindow*>(parentWidget());
            if (lobby) {
                lobby->setSocket(m_socket);   // 重新接管socket
            }
        }
        event->accept();
        return;
    }

    if (m_isOwner) {
        int ret = QMessageBox::question(this, "解散房间", "您是房主，关闭窗口将解散整个房间并断开连接。确定吗？",
                                        QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            sendPacket("DISMISS_ROOM", m_roomName);
            // 立即关闭窗口并退出程序
            m_closedByServer = true;
            event->accept();
            // 退出程序
            QApplication::quit();
        } else {
            event->ignore();
            closing = false;
        }
    } else {
        int ret = QMessageBox::question(this, "退出房间", "确定要退出房间吗？",
                                        QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            sendPacket("LEAVE_ROOM", "");
            event->ignore();
            closing = false;
            return;
        } else {
            event->ignore();
            closing = false;
            return;
        }
    }
}

void ChatWindow::on_sendButton_clicked()
{
    QString msg = ui->lineEdit->text();
    if (msg.isEmpty()) return;
    sendPacket("MSG", msg);
    ui->textEdit->append(QString("%1: %2").arg(m_nickname, msg));
    ui->lineEdit->clear();
}

void ChatWindow::on_leaveButton_clicked()
{
    if (m_isLeaving) return;
    m_isLeaving = true;
    
    if (m_isOwner) {
        int ret = QMessageBox::question(this, "解散房间", "您是房主，关闭窗口将解散整个房间并断开连接。确定吗？",
                                        QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            sendPacket("DISMISS_ROOM", m_roomName);
            // 立即关闭窗口并退出程序
            m_closedByServer = true;
            QApplication::quit();
        }
    } else {
        sendPacket("LEAVE_ROOM", "");
    }
    
    QTimer::singleShot(2000, this, [this]() { m_isLeaving = false; });
}

void ChatWindow::on_kickButton_clicked()
{
    ui->kickButton->setEnabled(false);
    bool ok;
    QString nick = QInputDialog::getText(this, "踢人", "输入要踢出的用户昵称:", QLineEdit::Normal, "", &ok);
    if (ok && !nick.isEmpty()) {
        if (nick == m_nickname) {
            QMessageBox::warning(this, "踢人失败", "不能踢自己");
            QTimer::singleShot(1000, this, [this]() { ui->kickButton->setEnabled(true); });
            return;
        }
        sendPacket("KICK", m_roomName + "|" + nick);
    }
    QTimer::singleShot(1000, this, [this]() { ui->kickButton->setEnabled(true); });
}

void ChatWindow::on_muteButton_clicked()
{
    ui->muteButton->setEnabled(false);
    bool ok;
    QString nick = QInputDialog::getText(this, "禁言", "输入要禁言的用户昵称:", QLineEdit::Normal, "", &ok);
    if (!ok || nick.isEmpty()) {
        QTimer::singleShot(1000, this, [this]() { ui->muteButton->setEnabled(true); });
        return;
    }
    if (nick == m_nickname) {
        QMessageBox::warning(this, "禁言失败", "不能禁言自己");
        QTimer::singleShot(1000, this, [this]() { ui->muteButton->setEnabled(true); });
        return;
    }
    int minutes = QInputDialog::getInt(this, "禁言时长", "分钟数:", 5, 1, 1440, 1, &ok);
    if (ok) {
        sendPacket("MUTE", m_roomName + "|" + nick + "|" + QString::number(minutes));
    }
    QTimer::singleShot(1000, this, [this]() { ui->muteButton->setEnabled(true); });
}



void ChatWindow::onSocketDisconnected()
{
    addSystemMessage("与服务器断开连接");
    QTimer::singleShot(1000, this, &ChatWindow::close);
}

void ChatWindow::onReadyRead()
{
    QByteArray data = m_socket->readAll();
    qDebug() << "Raw data received:" << data;  // 需 #include <QDebug>
    m_receiveBuffer.append(data);

    int pos;
    while ((pos = m_receiveBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_receiveBuffer.left(pos);
        m_receiveBuffer.remove(0, pos + 1);
        parseMessage(line);
    }
}

void ChatWindow::addSystemMessage(const QString &msg)
{
    ui->textEdit->append(QString("[系统] %1").arg(msg));
}

void ChatWindow::sendPacket(const QString &type, const QString &data)
{
    qDebug() << "sendPacket:" << type << data;
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        addSystemMessage("未连接到服务器");
        return;
    }
    QString packet = type + "|" + data + "\n";
    m_socket->write(packet.toUtf8());
    m_socket->flush();
}

void ChatWindow::parseMessage(const QByteArray &line)
{
    QString str = QString::fromUtf8(line).trimmed();
    QStringList parts = str.split('|');
    if (parts.isEmpty()) return;

    QString type = parts[0];
    if (type == "MSG") {
        if (parts.size() >= 2) {
            ui->textEdit->append(parts[1]);
        }
    } else if (type == "NICK") {
        addSystemMessage(parts[1]);
    } else if (type == "LEAVE") {
        addSystemMessage(parts[1]);
    } else if (type == "LEAVE_ROOM_OK") {
        addSystemMessage("已退出房间");
        m_closedByServer = true;
        if (parentWidget()) {
            LobbyWindow *lobby = qobject_cast<LobbyWindow*>(parentWidget());
            if (lobby) {
                lobby->setSocket(m_socket);   // 重新接管socket
                lobby->show();
            }
        }
        close();
    } else if (type == "ERROR") {
        addSystemMessage(parts[1]);
    } else if (type == "HISTORY") {
        if (parts.size() >= 2) {
            ui->textEdit->append(parts[1]);   // 历史消息显示
        }
    } else if (type == "HISTORY_END") {
        addSystemMessage("--- 历史消息加载完毕 ---");
    } else if (type == "KICK") {
        addSystemMessage(parts[1]);
        m_closedByServer = true;
        if (parentWidget()) {
            LobbyWindow *lobby = qobject_cast<LobbyWindow*>(parentWidget());
            if (lobby) {
                lobby->setSocket(m_socket);   // 重新接管socket
                lobby->show();
            }
        }
        close();
    } else if (type == "KICK_OK") {
        addSystemMessage(parts[1]);
        m_closedByServer = true;
        if (parentWidget()) {
            LobbyWindow *lobby = qobject_cast<LobbyWindow*>(parentWidget());
            if (lobby) {
                lobby->setSocket(m_socket);   // 重新接管socket
                lobby->show();
            }
        }
        close();
    } else if (type == "KICK_FAIL") {
        qDebug() << "Received KICK_FAIL, parts:" << parts;
        QString reason = parts.size() > 1 ? parts[1] : "未知错误";
        QMessageBox::warning(this, "踢人失败", reason);
        addSystemMessage("踢人失败: " + reason);
    } else if (type == "MUTE") {
        addSystemMessage(parts[1]);  // 禁言通知
    } else if (type == "MUTE_OK") {
        addSystemMessage(parts[1]);  // 禁言成功反馈
    } else if (type == "MUTE_FAIL") {
        qDebug() << "Received MUTE_FAIL, parts:" << parts;
        QString reason = parts.size() > 1 ? parts[1] : "未知错误";
        QMessageBox::warning(this, "禁言失败", reason);
        addSystemMessage("禁言失败: " + reason);
    } else if (type == "DISMISS") {
        addSystemMessage(parts[1]);
        m_closedByServer = true;
        if (parentWidget()) {
            LobbyWindow *lobby = qobject_cast<LobbyWindow*>(parentWidget());
            if (lobby) {
                lobby->setSocket(m_socket);   // 重新接管socket
                lobby->show();
            }
        }
        close();
    }
    else if (type == "DISMISS_OK") {
        addSystemMessage("房间已解散");
        m_closedByServer = true;
        if (parentWidget()) {
            LobbyWindow *lobby = qobject_cast<LobbyWindow*>(parentWidget());
            if (lobby) {
                lobby->setSocket(m_socket);   // 重新接管socket
                lobby->show();
            }
        }
        close();
    } else if (type == "DISMISS_FAIL") {
        addSystemMessage("解散失败: " + parts[1]);
    }else if (type == "PONG") {
        // 忽略
    }
}