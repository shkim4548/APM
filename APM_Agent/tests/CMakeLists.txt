add_executable(APM_Common_Tests
    AesGcmTests.cpp
    LocalAlertEvaluatorTests.cpp
    ../Agent/ThresholdSet.cpp
    ../Agent/LocalAlertEvaluator.cpp
    ../Agent/LogLevel.cpp
)
target_include_directories(APM_Common_Tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
target_link_libraries(APM_Common_Tests PRIVATE APM_Common APM_Storage GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(APM_Common_Tests)
