#ifndef AUTHDIALOG_H
#define AUTHDIALOG_H

#include <QDialog>

namespace Ui {
class AuthDialog;
}

class AuthDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AuthDialog(QWidget *parent = nullptr);
    ~AuthDialog();

    QString getUsername() const;
    QString getPassword() const;
    bool isLogin() const;

private slots:
    void on_loginButton_clicked();
    void on_registerButton_clicked();
    void on_exitButton_clicked();

private:
    Ui::AuthDialog *ui;
    bool m_isLogin;
    bool m_isProcessing;
};

#endif