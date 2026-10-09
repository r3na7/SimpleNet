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

add_executable(simplenet_event_loop_work_tests event_loop_work_tests.cpp)
target_link_libraries(simplenet_event_loop_work_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_event_loop_work_tests TEST_PREFIX loopwork. PROPERTIES TIMEOUT 10)

add_executable(simplenet_event_loop_noalloc_tests event_loop_noalloc_tests.cpp)
target_link_libraries(simplenet_event_loop_noalloc_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_event_loop_noalloc_tests TEST_PREFIX loopalloc. PROPERTIES TIMEOUT 10)

add_executable(simplenet_event_loop_cleanup_tests event_loop_cleanup_tests.cpp)
target_link_libraries(simplenet_event_loop_cleanup_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_event_loop_cleanup_tests TEST_PREFIX loopcleanup. PROPERTIES TIMEOUT 10)

add_executable(simplenet_socket_tests socket_tests.cpp)
target_link_libraries(simplenet_socket_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_socket_tests TEST_PREFIX socket. PROPERTIES TIMEOUT 10)

# Interception is confined to this executable; the production library has no test hooks.
add_executable(simplenet_socket_close_tests socket_close_tests.cpp)
target_link_libraries(simplenet_socket_close_tests PRIVATE SimpleNet GTest::gtest_main)
target_link_options(simplenet_socket_close_tests PRIVATE -Wl,--wrap=close)
gtest_discover_tests(simplenet_socket_close_tests TEST_PREFIX socketclose. PROPERTIES TIMEOUT 10)

add_executable(simplenet_buffer_tests buffer_tests.cpp)
target_link_libraries(simplenet_buffer_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_buffer_tests TEST_PREFIX buffer. PROPERTIES TIMEOUT 10)

# Allocation replacement is isolated from the other tests and production API.
add_executable(simplenet_buffer_allocation_tests buffer_allocation_tests.cpp)
target_link_libraries(simplenet_buffer_allocation_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_buffer_allocation_tests TEST_PREFIX bufferalloc. PROPERTIES TIMEOUT 10)

add_executable(simplenet_tcp_connection_tests tcp_connection_tests.cpp)
target_link_libraries(simplenet_tcp_connection_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_tcp_connection_tests TEST_PREFIX tcp. PROPERTIES TIMEOUT 10)

add_executable(simplenet_tcp_connection_syscall_tests tcp_connection_syscall_tests.cpp)
target_link_libraries(simplenet_tcp_connection_syscall_tests PRIVATE SimpleNet GTest::gtest_main)
target_link_options(simplenet_tcp_connection_syscall_tests PRIVATE -Wl,--wrap=epoll_ctl -Wl,--wrap=send -Wl,--wrap=shutdown -Wl,--wrap=recv)
gtest_discover_tests(simplenet_tcp_connection_syscall_tests TEST_PREFIX tcpsys. PROPERTIES TIMEOUT 10)

add_executable(simplenet_tcp_connection_allocation_tests tcp_connection_allocation_tests.cpp)
target_link_libraries(simplenet_tcp_connection_allocation_tests PRIVATE SimpleNet GTest::gtest_main)
target_link_options(simplenet_tcp_connection_allocation_tests PRIVATE -Wl,--wrap=recv)
gtest_discover_tests(simplenet_tcp_connection_allocation_tests TEST_PREFIX tcpalloc. PROPERTIES TIMEOUT 10)
