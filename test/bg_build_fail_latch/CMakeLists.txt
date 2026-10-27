add_executable(BgBuildFailLatchTest BgBuildFailLatchTest.cpp)

target_include_directories(BgBuildFailLatchTest PRIVATE ../../src)

target_link_libraries(BgBuildFailLatchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BgBuildFailLatchTest)
