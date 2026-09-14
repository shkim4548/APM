add_library(APM_Storage STATIC
    MetricStoreFactory.cpp
    AgentAlertStore.cpp
)

target_include_directories(APM_Storage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# pch.h가 asio.hpp 등을 무조건 include하므로, 백엔드와 무관하게 GW2_CrossPlatformCore를
# 링크해서 그 include 경로(asio_interface 등)를 항상 받아야 함.
target_link_libraries(APM_Storage PUBLIC GW2_CrossPlatformCore protobuf::libprotobuf)

# Agent의 로컬 알림 저장(AgentAlertStore)은 Console/Collector의 지표 저장 백엔드 선택과
# 무관하게 항상 SQLite다(알림 몇 건 저장하려고 TimescaleDB까지 갈 이유가 없음) - 그래서
# 이 find_package/링크는 아래 backend 분기 밖으로 뺐다. 2026-09-14 이전에는
# APM_STORAGE_BACKEND=TimescaleDB를 고르면 SQLite3가 전혀 필요 없었는데, 이제는
# 그 경우에도 SQLite3가 필수 의존성이 된다(AgentAlertStore 때문) - 의도된 변화.
find_package(SQLite3 REQUIRED)
target_link_libraries(APM_Storage PUBLIC SQLite::SQLite3)

if(APM_STORAGE_BACKEND STREQUAL "SQLite")
    target_sources(APM_Storage PRIVATE SqliteMetricStore.cpp)
    target_compile_definitions(APM_Storage PUBLIC APM_STORAGE_SQLITE)
elseif(APM_STORAGE_BACKEND STREQUAL "TimescaleDB")
    target_sources(APM_Storage PRIVATE TimescaleMetricStore.cpp)
    target_compile_definitions(APM_Storage PUBLIC APM_STORAGE_TIMESCALEDB)
else()
    message(FATAL_ERROR "Unknown APM_STORAGE_BACKEND: ${APM_STORAGE_BACKEND} (SQLite 또는 TimescaleDB)")
endif()
