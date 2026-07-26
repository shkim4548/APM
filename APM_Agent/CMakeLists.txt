cmake_minimum_required(VERSION 3.20)
project(APM_Agent CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 소스 파일이 전부 UTF-8(주석/GTest 한글 테스트 이름 포함)인데, MSVC는 기본적으로
# 시스템 코드페이지(한글 Windows면 949)로 소스를 해석함 - /utf-8 없이는 한글 식별자가
# 깨져 컴파일 에러가 남(Linux/GCC는 기본이 UTF-8이라 이 문제가 없었음).
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Protobuf REQUIRED)

# 메트릭 저장 백엔드 선택 - 컴파일 타임 고정(런타임 전환 아님).
# SQLite를 고르면 TimescaleDB(Postgres) 관련 라이브러리는 전혀 링크되지 않음 - 미설치 상태에서도 빌드+실행 가능.
set(APM_STORAGE_BACKEND "SQLite" CACHE STRING "메트릭 저장 백엔드: SQLite 또는 TimescaleDB")
set_property(CACHE APM_STORAGE_BACKEND PROPERTY STRINGS SQLite TimescaleDB)

# GW2_CrossPlatformCore의 Postgres 레이어는 TimescaleDB를 고를 때만 켠다
# (add_subdirectory보다 먼저 캐시 변수를 세팅해야 그 안의 option()이 이 값을 씀).
if(APM_STORAGE_BACKEND STREQUAL "TimescaleDB")
    set(GW2_ENABLE_POSTGRES ON CACHE BOOL "" FORCE)
else()
    set(GW2_ENABLE_POSTGRES OFF CACHE BOOL "" FORCE)
endif()

# GW2_CrossPlatformCore를 공유 라이브러리로 재사용 (Asio, OpenSSL 링크 포함)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../GW2_CrossPlatformCore ${CMAKE_CURRENT_BINARY_DIR}/GW2_CrossPlatformCore)

add_subdirectory(Common)
add_subdirectory(Storage)

add_executable(Collector
    Collector/main.cpp
    Collector/CollectorConfig.cpp
    pch.cpp
)
target_include_directories(Collector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(Collector PRIVATE APM_Common APM_Storage)

add_executable(Agent
    Agent/main.cpp
    pch.cpp
)
target_link_libraries(Agent PRIVATE APM_Common)

add_executable(LoadTester
    LoadTester/main.cpp
    LoadTester/LoadTesterConfig.cpp
    LoadTester/LoadTestStats.cpp
    LoadTester/SimulatedAgent.cpp
    LoadTester/LoadTester.cpp
    pch.cpp
)
target_link_libraries(LoadTester PRIVATE APM_Common)

# 단위 테스트는 선택 사항(GTest) - 기본 ON이지만 libgtest-dev가 없는 환경에서도
# Collector/Agent 빌드 자체는 막히면 안 되므로(이 프로젝트의 기존 원칙 - 예상치 못한
# 빌드 요구사항 금지), find_package 실패 시 조용히 건너뛰고 안내만 출력한다.
option(APM_BUILD_TESTS "GoogleTest 기반 단위 테스트 빌드 (libgtest-dev 필요)" ON)
if(APM_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "APM_BUILD_TESTS=ON이지만 GTest를 찾지 못함 - 'sudo apt install libgtest-dev' 후 재시도하세요. 테스트는 건너뜁니다.")
    endif()
endif()