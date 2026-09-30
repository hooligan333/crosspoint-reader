add_executable(DictPlainPagingTest
  DictPlainPagingTest.cpp
)

target_include_directories(DictPlainPagingTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(DictPlainPagingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictPlainPagingTest)
