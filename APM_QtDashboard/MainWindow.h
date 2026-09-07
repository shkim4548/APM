#pragma once
#include <QMainWindow>

class QLabel;
class QPushButton;

// 검증용 최소 창 : QPushButton::clicked 신호를 이 클래스의 슬롯에 연결
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void OnButtonClicked();

private:
    QLabel* _clickCountLabel;
    QPushButton* _clickButton;
    int _clickCount = 0;
};