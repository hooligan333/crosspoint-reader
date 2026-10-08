add_executable(DictSwitchLayoutTest
  DictSwitchLayoutTest.cpp
)

target_include_directories(DictSwitchLayoutTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_compile_definitions(DictSwitchLayoutTest PRIVATE
  TRANSLATIONS_DIR="${REPO_ROOT}/lib/I18n/translations"
)

target_link_libraries(DictSwitchLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictSwitchLayoutTest)
