cmake_minimum_required(VERSION 3.20)
project(APM_QtDashboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Q_OBJECT가 붙은 클래스는 moc(Meta-Object Compiler)가 signal/slot 디스패치 코드를
# 별도로 생성해줘야 링크가 됨 - AUTOMOC이 그 호출을 빌드 시점에 자동으로 끼워 넣는다.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)

add_executable(APM_QtDashboard
    main.cpp
    MainWindow.cpp
    MainWindow.h
    MetricsRepository.cpp
    MetricsRepository.h
    MetricsWorker.cpp
    MetricsWorker.h
)

target_link_libraries(APM_QtDashboard PRIVATE Qt6::Widgets Qt6::Sql)