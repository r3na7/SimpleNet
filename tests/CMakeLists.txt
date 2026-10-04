include(FetchContent)

set(BUILD_GMOCK OFF)
set(INSTALL_GTEST OFF)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(simplenet_reactor_tests reactor_tests.cpp)
target_link_libraries(simplenet_reactor_tests PRIVATE SimpleNet GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(simplenet_reactor_tests
    TEST_PREFIX reactor.
    PROPERTIES TIMEOUT 10
)
