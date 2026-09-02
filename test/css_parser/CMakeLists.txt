add_executable(CssParserTest
  CssParserTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
)

target_include_directories(CssParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Memory
  # CssParser.cpp reads the allocation-gate heap gauge for its low-memory
  # cache-retry path; the stubs above stand in for the IDF side of it.
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(CssParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssParserTest)

# Second build of the same suite with the fork's CSS class-rules feature
# enabled: the border-left tests and the flag-aware kStyleLengthFieldCount in
# CssParserTest.cpp are #ifdef CROSSPOINT_CSS_CLASS_RULES and are otherwise
# never compiled. Run both targets to cover both wire formats (flags-off must
# stay byte-identical upstream; flagged adds the borderLeftWidth field).
add_executable(CssParserClassRulesTest
  CssParserTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
)

target_compile_definitions(CssParserClassRulesTest PRIVATE
  CROSSPOINT_CSS_CLASS_RULES=1
)

target_include_directories(CssParserClassRulesTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(CssParserClassRulesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssParserClassRulesTest)
