cmake_minimum_required(VERSION 3.20)
project(GW2_CrossPlatformCore CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PostgreSQL(libpq) DB 레이어 - 기본 ON(GW2_Server는 항상 필요).
# APM_Agent는 SQLite 백엔드를 고르면 이 옵션을 자신의 CMakeLists.txt에서 OFF로 미리 설정함
# (add_subdirectory 호출 전에 캐시 변수를 세팅하면 option()이 그 값을 그대로 씀).
option(GW2_ENABLE_POSTGRES "PostgreSQL(libpq) DB 레이어 활성화" ON)

# Standalone Asio (Boost 없이)
include(FetchContent)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-28-2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(asio)

add_library(asio_interface INTERFACE)
target_include_directories(asio_interface INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio_interface INTERFACE ASIO_STANDALONE)

if(WIN32)
    target_link_libraries(asio_interface INTERFACE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(asio_interface INTERFACE Threads::Threads)
endif()

if(GW2_ENABLE_POSTGRES)
    # PostgresSQL(libpq)
    find_package(PostgreSQL REQUIRED)
endif()
# OpenSSL (TLS - asio::ssl::stream)
find_package(OpenSSL REQUIRED)

# 라이브러리 타겟
add_library(GW2_CrossPlatformCore STATIC)

# 각 서브폴더를 include 경로로 등록 (기존 #include "X.h" 형태 유지)
target_include_directories(GW2_CrossPlatformCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Main
    ${CMAKE_CURRENT_SOURCE_DIR}/Network
    ${CMAKE_CURRENT_SOURCE_DIR}/Thread
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
)

target_link_libraries(GW2_CrossPlatformCore PUBLIC asio_interface OpenSSL::SSL OpenSSL::Crypto)
add_subdirectory(Main)
add_subdirectory(Network)
add_subdirectory(Thread)
add_subdirectory(Utils)

if(GW2_ENABLE_POSTGRES)
    target_include_directories(GW2_CrossPlatformCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/DB)
    target_link_libraries(GW2_CrossPlatformCore PUBLIC PostgreSQL::PostgreSQL)
    target_compile_definitions(GW2_CrossPlatformCore PUBLIC GW2_ENABLE_POSTGRES)
    add_subdirectory(DB)
endif()