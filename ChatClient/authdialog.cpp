#include "authdialog.h"
#include "ui_authdialog.h"
#include <QMessageBox>
#include <QApplication>
#include <QTimer>

AuthDialog::AuthDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AuthDialog),
    m_isLogin(true),
    m_isProcessing(false)
{
    ui->setupUi(this);
    connect(ui->loginButton, &QPushButton::clicked, this, &AuthDialog::on_loginButton_clicked);
    connect(ui->registerButton, &QPushButton::clicked, this, &AuthDialog::on_registerButton_clicked);
    connect(ui->exitButton, &QPushButton::clicked, this, &AuthDialog::on_exitButton_clicked);
}

AuthDialog::~AuthDialog()
{
    delete ui;
}

QString AuthDialog::getUsername() const
{
    return ui->usernameEdit->text();
}

QString AuthDialog::getPassword() const
{
    return ui->passwordEdit->text();
}

bool AuthDialog::isLogin() const
{
    return m_isLogin;
}

void AuthDialog::on_loginButton_clicked()
{
    if (m_isProcessing) return;          // 正在处理，忽略重复点击
    m_isProcessing = true;
    if (getUsername().isEmpty() || getPassword().isEmpty()) {
        QMessageBox::warning(this, "提示", "用户名和密码不能为空");
        return;
    }
    m_isLogin = true;
    accept();

    // 稍后重置标志（防止对话框关闭后无法再次打开）
    QTimer::singleShot(500, this, [this]() { m_isProcessing = false; });
}

void AuthDialog::on_registerButton_clicked()
{
    if (m_isProcessing) return;          // 正在处理，忽略重复点击
    m_isProcessing = true;

    if (getUsername().isEmpty() || getPassword().isEmpty()) {
        QMessageBox::warning(this, "提示", "用户名和密码不能为空");
        m_isProcessing = false;
        return;
    }
    m_isLogin = false;
    accept();

    // 稍后重置标志（防止对话框关闭后无法再次打开）
    QTimer::singleShot(500, this, [this]() { m_isProcessing = false; });
}

void AuthDialog::on_exitButton_clicked()
{
    QApplication::quit();
}