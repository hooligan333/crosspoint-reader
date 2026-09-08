add_executable(DeckSyncTest
  DeckSyncTest.cpp
  ${REPO_ROOT}/src/util/DestFolder.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(DeckSyncTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/FsHelpers
)

# Both units under test are whole-file #ifdef'd on the feature flag.
target_compile_definitions(DeckSyncTest PRIVATE CROSSPOINT_FLASHCARDS)

target_link_libraries(DeckSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DeckSyncTest)
