set(NIFTEST
    niftest.cpp
)
source_group(components\\nif\\tests FILES ${NIFTEST})

# Main executable
openmw_add_executable(niftest
    ${NIFTEST}
)

target_link_libraries(niftest
  components
)

if (BUILD_WITH_CODE_COVERAGE)
  add_definitions (--coverage)
  target_link_libraries(niftest gcov)
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.16 AND MSVC)
    target_precompile_headers(niftest PRIVATE <filesystem>)
endif()
