add_executable(BleRadioPolicyTest BleRadioPolicyTest.cpp)

target_include_directories(BleRadioPolicyTest PRIVATE ../../src ../../lib/BlePageTurner/include
  ../../freeink-sdk/libs/network/BleKeyboardHost/include)

target_link_libraries(BleRadioPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BleRadioPolicyTest)
