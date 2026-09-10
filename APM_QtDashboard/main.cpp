#include <QApplication>

#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    // 워커 쓰레드에서 UI 쓰레드로 결과를 큐 연결로 넘기기 전에 컨테이너 타입 등록
    qRegisterMetaType<QVector<MetricsSample>>("QVector<MetricsSample>");
    qRegisterMetaType<QVector<AlertSample>>("QVector<AlertSample>");

    MainWindow window;
    window.resize(720, 480);
    window.show();

    return app.exec();
}
