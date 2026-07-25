add_executable(APM_Common_Tests
    AesGcmTests.cpp
)
target_link_libraries(APM_Common_Tests PRIVATE APM_Common GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(APM_Common_Tests)
