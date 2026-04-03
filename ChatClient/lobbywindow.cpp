#include "lobbywindow.h"
#include "ui_lobbywindow.h"
#include "chatwindow.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QDebug>
#include <QTimer>

LobbyWindow::LobbyWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::LobbyWindow),
    m_socket(nullptr)
{
    ui->setupUi(this);
    connect(ui->createRoomButton, &QPushButton::clicked, this, &LobbyWindow::onCreateRoom);
    connect(ui->joinRoomButton, &QPushButton::clicked, this, &LobbyWindow::onJoinRoom);
    connect(ui->exitButton, &QPushButton::clicked, this, &LobbyWindow::onExit);
    connect(ui->roomListWidget, &QListWidget::itemDoubleClicked, this, &LobbyWindow::onJoinRoom);

    // 心跳定时器
    QTimer* pingTimer = new QTimer(this);
    connect(pingTimer, &QTimer::timeout, [this]() {
        if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
            m_socket->write("PING|\n");
            m_socket->flush();
        }
    });
    pingTimer->start(30000); // 30秒
}

LobbyWindow::~LobbyWindow()
{
    delete ui;
}

void LobbyWindow::setNickname(const QString &nickname)
{
    m_nickname = nickname;
    setWindowTitle(QString("大厅 - %1").arg(m_nickname));
}

void LobbyWindow::detachSocket()
{
    if (!m_socket) return;
    disconnect(m_socket, nullptr, this, nullptr);
}

void LobbyWindow::setSocket(QTcpSocket *socket)
{
    m_socket = socket;
    connect(m_socket, &QTcpSocket::readyRead, [this]() {
        m_receiveBuffer.append(m_socket->readAll());
        int pos;
        while ((pos = m_receiveBuffer.indexOf('\n')) != -1) {
            QByteArray line = m_receiveBuffer.left(pos);
            m_receiveBuffer.remove(0, pos + 1);
            parseMessage(line);
        }
    });
    connect(m_socket, &QTcpSocket::disconnected, this, &LobbyWindow::onSocketDisconnected);
    QTimer::singleShot(100, this, [this](){
        sendPacket("LIST_ROOMS", "");
    });
}

void LobbyWindow::onCreateRoom()
{
    bool ok;
    QString roomName = QInputDialog::getText(this, "创建房间", "房间名：", QLineEdit::Normal, "", &ok);
    if (!ok || roomName.isEmpty()) return;
    if (roomName == "大厅") {
        QMessageBox::warning(this, "提示", "不能创建名为“大厅”的房间，该名称已被保留");
        return;
    }

    QStringList items;
    items << "公开" << "私有";
    bool isPublic = (QInputDialog::getItem(this, "房间类型", "选择类型：", items, 0, false, &ok) == "公开");
    if (!ok) return;

    QString password;
    if (!isPublic) {
        password = QInputDialog::getText(this, "房间密码", "请输入密码：", QLineEdit::Password, "", &ok);
        if (!ok) return;
    }
    QString params = QString("%1|%2|%3").arg(roomName).arg(isPublic ? "1" : "0").arg(password);
    sendPacket("CREATE", params);
}

void LobbyWindow::onJoinRoom()
{
    QListWidgetItem *current = ui->roomListWidget->currentItem();
    if (!current) {
        QMessageBox::information(this, "提示", "请先选择一个房间");
        return;
    }
    QString originalName = current->data(Qt::UserRole).toString();
    if (originalName.isEmpty()) return;

    bool ok;
    QString password = QInputDialog::getText(this, "加入房间", "请输入密码（没有密码则留空）：", QLineEdit::Password, "", &ok);
    if (!ok) return;
    QString params = originalName + "|" + password;
    sendPacket("JOIN", params);
}

void LobbyWindow::onExit()
{
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->disconnectFromHost();
    }
    close();
}

void LobbyWindow::onSocketDisconnected()
{
    QMessageBox::information(this, "提示", "与服务器断开连接");
    onExit();
}

void LobbyWindow::sendPacket(const QString &type, const QString &data)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, "错误", "未连接到服务器");
        return;
    }
    QString packet = type + "|" + data + "\n";
    m_socket->write(packet.toUtf8());
    m_socket->flush();
}

void LobbyWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
        sendPacket("LIST_ROOMS", "");
    }
}

void LobbyWindow::parseMessage(const QByteArray &line)
{
    QString str = QString::fromUtf8(line).trimmed();
    qDebug() << "[DEBUG] LobbyWindow::parseMessage received:" << str;
    QStringList parts = str.split('|');
    if (parts.isEmpty()) return;

    QString type = parts[0];
    qDebug() << "[DEBUG] Message type:" << type << "parts size:" << parts.size();
    if (type == "ROOM_LIST") {
        qDebug() << "[DEBUG] Processing ROOM_LIST message";
        if (parts.size() >= 2) {
            // 房间列表从 parts[1] 开始，需要重新组合
            QString roomListData = "";
            for (int i = 1; i < parts.size(); i++) {
                if (i > 1) roomListData += "|";
                roomListData += parts[i];
            }
            qDebug() << "[DEBUG] Calling updateRoomList with:" << roomListData;
            updateRoomList(roomListData);
        } else {
            qDebug() << "[DEBUG] ROOM_LIST has insufficient parts";
        }
    } else if (type == "CREATE_OK") {
        if (parts.size() < 2) return;
        QString originalName = parts[1];
        // 创建者一定是房主
        bool isOwner = true;
        QString displayName = (originalName == "Lobby") ? "大厅" : originalName;

        // 请求历史消息
        sendPacket("GET_HISTORY", originalName);

        // 关键：先停止大厅读取socket
        detachSocket();

        ChatWindow *chatWin = new ChatWindow(this);
        chatWin->setAttribute(Qt::WA_DeleteOnClose);
        chatWin->setNickname(m_nickname);
        chatWin->setRoomName(displayName);
        chatWin->setSocket(m_socket);
        chatWin->setIsOwner(isOwner);   // 房主标志为 true
        chatWin->show();

        this->hide();
    } else if (type == "CREATE_FAIL") {
        QMessageBox::warning(this, "创建房间失败", parts[1]);
    } else if (type == "JOIN_OK") {
        if (parts.size() < 2) return;
        QString originalName = parts[1];
        
        // 如果是大厅，不创建聊天窗口，直接显示大厅
        if (originalName == "Lobby") {
            this->show();
            return;
        }
        
        bool isOwner = (parts.size() >= 3 && parts[2] == "1");   // 从协议中获取房主标志
        QString displayName = (originalName == "Lobby") ? "大厅" : originalName;

        // 请求历史消息
        sendPacket("GET_HISTORY", originalName);

        // 关键：先停止大厅读取socket
        detachSocket();

        // 创建聊天窗口
        ChatWindow *chatWin = new ChatWindow(this);
        chatWin->setAttribute(Qt::WA_DeleteOnClose);
        chatWin->setNickname(m_nickname);
        chatWin->setRoomName(displayName);
        chatWin->setSocket(m_socket);
        chatWin->setIsOwner(isOwner);   // 传递房主身份，控制按钮显示
        chatWin->show();

        // 隐藏大厅窗口
        this->hide();
    } else if (type == "JOIN_FAIL") {
        QMessageBox::warning(this, "加入房间失败", parts[1]);
    } else {
        // 其他消息忽略
    }
}

void LobbyWindow::updateRoomList(const QString &roomsData)
{
    qDebug() << "[DEBUG] updateRoomList called with data:" << roomsData;
    ui->roomListWidget->clear();
    QStringList rooms = roomsData.split(';');
    qDebug() << "[DEBUG] Split rooms:" << rooms;
    for (const QString &room : rooms) {
        if (room.isEmpty()) continue;
        QStringList attrs = room.split('|');
        qDebug() << "[DEBUG] Room attrs:" << attrs;
        if (attrs.size() >= 3) {
            QString name = attrs[0];
            QString type = attrs[1];
            QString members = attrs[2];
            QString display = QString("%1 [%2] (%3人)").arg(name, type, members);
            qDebug() << "[DEBUG] Creating item:" << display;
            QListWidgetItem *item = new QListWidgetItem(display);
            item->setData(Qt::UserRole, name);
            ui->roomListWidget->addItem(item);
        }
    }
    qDebug() << "[DEBUG] Total items in room list:" << ui->roomListWidget->count();
}