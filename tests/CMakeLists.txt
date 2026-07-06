include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE
)

# For Windows: prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

function(tinylsm_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE tinylsm GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

tinylsm_add_test(smoke_test)
tinylsm_add_test(status_test)
tinylsm_add_test(coding_test)
tinylsm_add_test(crc32c_test)
tinylsm_add_test(internal_key_test)
tinylsm_add_test(block_test)
tinylsm_add_test(table_test)
tinylsm_add_test(env_test)
tinylsm_add_test(filename_test)
tinylsm_add_test(version_edit_test)
tinylsm_add_test(version_set_test)
