#include "MainWindow.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle("APM Qt Dashboard - step 0/1");

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    _clickCountLabel = new QLabel("Clicked 0 times", central);
    _clickButton = new QPushButton("Click me", central);

    layout->addWidget(_clickCountLabel);
    layout->addWidget(_clickButton);
    setCentralWidget(central);

    connect(_clickButton, &QPushButton::clicked, this, &MainWindow::OnButtonClicked);
}

void MainWindow::OnButtonClicked()
{
    ++_clickCount;
    _clickCountLabel->setText(QString("Clicked %1 times").arg(_clickCount));
}