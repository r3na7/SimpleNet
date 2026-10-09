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

add_executable(simplenet_acceptor_tests acceptor_tests.cpp)
target_link_libraries(simplenet_acceptor_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_acceptor_tests TEST_PREFIX accept. PROPERTIES TIMEOUT 10)

add_executable(simplenet_acceptor_syscall_tests acceptor_syscall_tests.cpp)
target_link_libraries(simplenet_acceptor_syscall_tests PRIVATE SimpleNet GTest::gtest_main)
target_link_options(simplenet_acceptor_syscall_tests PRIVATE -Wl,--wrap=epoll_ctl -Wl,--wrap=accept4 -Wl,--wrap=getsockopt -Wl,--wrap=epoll_wait)
gtest_discover_tests(simplenet_acceptor_syscall_tests TEST_PREFIX acceptsys. PROPERTIES TIMEOUT 10)

add_executable(simplenet_acceptor_allocation_tests acceptor_allocation_tests.cpp)
target_link_libraries(simplenet_acceptor_allocation_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_acceptor_allocation_tests TEST_PREFIX acceptalloc. PROPERTIES TIMEOUT 10)

add_executable(simplenet_tcp_server_tests tcp_server_tests.cpp)
target_link_libraries(simplenet_tcp_server_tests PRIVATE SimpleNet GTest::gtest_main)
gtest_discover_tests(simplenet_tcp_server_tests TEST_PREFIX server. PROPERTIES TIMEOUT 10)

add_executable(simplenet_tcp_server_syscall_tests tcp_server_syscall_tests.cpp)
target_link_libraries(simplenet_tcp_server_syscall_tests PRIVATE SimpleNet GTest::gtest_main)
target_link_options(simplenet_tcp_server_syscall_tests PRIVATE -Wl,--wrap=epoll_ctl -Wl,--wrap=accept4 -Wl,--wrap=epoll_wait)
gtest_discover_tests(simplenet_tcp_server_syscall_tests TEST_PREFIX serversys. PROPERTIES TIMEOUT 10)

add_executable(simplenet_tcp_server_allocation_tests tcp_server_allocation_tests.cpp)
target_link_libraries(simplenet_tcp_server_allocation_tests PRIVATE SimpleNet GTest::gtest_main)
target_link_options(simplenet_tcp_server_allocation_tests PRIVATE -Wl,--wrap=accept4)
gtest_discover_tests(simplenet_tcp_server_allocation_tests TEST_PREFIX serveralloc. PROPERTIES TIMEOUT 10)

if(SIMPLENET_BUILD_EXAMPLES)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_executable(simplenet_signal_stop_tests signal_stop_tests.cpp)
    target_include_directories(simplenet_signal_stop_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples)
    target_link_libraries(simplenet_signal_stop_tests PRIVATE SimpleNet GTest::gtest_main)
    target_link_options(simplenet_signal_stop_tests PRIVATE -Wl,--wrap=signalfd -Wl,--wrap=epoll_ctl -Wl,--wrap=close)
    gtest_discover_tests(simplenet_signal_stop_tests TEST_PREFIX demosignal. PROPERTIES TIMEOUT 10)
    foreach(stop_signal INT TERM)
        string(TOLOWER ${stop_signal} signal_suffix)
        add_test(NAME demo.harness_cancel_${signal_suffix} COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/echo_harness_cancel_tests.py
            --server $<TARGET_FILE:snet_echo_server> --client ${PROJECT_SOURCE_DIR}/examples/slow_echo_client.py
            --harness ${CMAKE_CURRENT_SOURCE_DIR}/echo_demo_tests.py --signal ${stop_signal})
        set_tests_properties(demo.harness_cancel_${signal_suffix} PROPERTIES TIMEOUT 45)
    endforeach()
    add_test(NAME demo.client_setup_deadline COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/client_setup_deadline_tests.py)
    set_tests_properties(demo.client_setup_deadline PROPERTIES TIMEOUT 45)
    foreach(case client_stream client_help client_invalid_cli slow_ipv4 slow_ipv6 client_corrupt client_early_eof client_stall client_connect_failure)
        add_test(NAME demo.${case} COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/echo_demo_tests.py --server $<TARGET_FILE:snet_echo_server>
            --client ${PROJECT_SOURCE_DIR}/examples/slow_echo_client.py --case ${case})
        set_tests_properties(demo.${case} PROPERTIES TIMEOUT 45)
    endforeach()
    foreach(case help invalid_cli occupied_address echo_ipv4 echo_ipv6 signal_int signal_term signal_active startup_cleanup)
        add_test(NAME demo.${case} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/echo_demo_tests.py --server $<TARGET_FILE:snet_echo_server> --case ${case})
        set_tests_properties(demo.${case} PROPERTIES TIMEOUT 45)
    endforeach()
endif()
