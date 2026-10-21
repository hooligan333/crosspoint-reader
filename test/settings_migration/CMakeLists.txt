add_executable(SettingsActionMigrationTest SettingsActionMigrationTest.cpp)

target_include_directories(SettingsActionMigrationTest PRIVATE ../../src)
# GoBack only exists in the enum under the fork flag the migration belongs to.
target_compile_definitions(SettingsActionMigrationTest PRIVATE CROSSPOINT_HOME_TAP_GO_BACK)

target_link_libraries(SettingsActionMigrationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SettingsActionMigrationTest)
