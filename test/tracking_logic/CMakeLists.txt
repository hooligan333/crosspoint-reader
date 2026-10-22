add_executable(TrackingLogicTest
  TrackingLogicTest.cpp
  ${REPO_ROOT}/lib/Tracking/TrackingConfig.cpp
  ${REPO_ROOT}/lib/Tracking/TrackingFormat.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)

target_include_directories(TrackingLogicTest PRIVATE
  ${REPO_ROOT}/lib/Tracking
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)

# The library is whole-file #ifdef'd on the feature flag.
target_compile_definitions(TrackingLogicTest PRIVATE CROSSPOINT_TRACKING)

target_link_libraries(TrackingLogicTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TrackingLogicTest)
