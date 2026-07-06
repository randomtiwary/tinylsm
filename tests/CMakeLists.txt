include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)

# For Windows: prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE tinylsm GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(smoke_test)
