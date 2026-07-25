add_library(APM_Common STATIC
    ApmSession.cpp
    AriaCipher.cpp
    AesGcmCipher.cpp
    AesGcmPayload.cpp
    HmacUtil.cpp
    SecurePayload.cpp
    KeyLoader.cpp
    PrivilegeDrop.cpp
    ResourceCollector.cpp
    MetricScheduler.cpp
    ResilientSender.cpp
    PacketHandler.cpp
    ../Protocol/Metric.pb.cc
)

target_include_directories(APM_Common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(APM_Common PUBLIC GW2_CrossPlatformCore protobuf::libprotobuf)

if(WIN32)
    # ResourceCollector::ComputeNetworkUsage(GetIfTable2/FreeMibTable)가 필요로 함 -
    # Linux는 /proc/net/dev를 읽어서 별도 링크가 필요 없지만 Windows는 IP Helper API 필요.
    target_link_libraries(APM_Common PUBLIC iphlpapi)
endif()