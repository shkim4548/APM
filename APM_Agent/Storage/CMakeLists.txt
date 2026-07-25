add_library(APM_Storage STATIC
    MetricStoreFactory.cpp
)

target_include_directories(APM_Storage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# pch.h가 asio.hpp 등을 무조건 include하므로, 백엔드와 무관하게 GW2_CrossPlatformCore를
# 링크해서 그 include 경로(asio_interface 등)를 항상 받아야 함.
target_link_libraries(APM_Storage PUBLIC GW2_CrossPlatformCore protobuf::libprotobuf)

if(APM_STORAGE_BACKEND STREQUAL "SQLite")
    find_package(SQLite3 REQUIRED)
    target_sources(APM_Storage PRIVATE SqliteMetricStore.cpp)
    target_link_libraries(APM_Storage PUBLIC SQLite::SQLite3)
    target_compile_definitions(APM_Storage PUBLIC APM_STORAGE_SQLITE)
elseif(APM_STORAGE_BACKEND STREQUAL "TimescaleDB")
    target_sources(APM_Storage PRIVATE TimescaleMetricStore.cpp)
    target_compile_definitions(APM_Storage PUBLIC APM_STORAGE_TIMESCALEDB)
else()
    message(FATAL_ERROR "Unknown APM_STORAGE_BACKEND: ${APM_STORAGE_BACKEND} (SQLite 또는 TimescaleDB)")
endif()
