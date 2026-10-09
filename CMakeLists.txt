cmake_minimum_required(VERSION 3.20)

project(
    SimpleNet
    VERSION 0.1.0
    LANGUAGES CXX
)

add_library(SimpleNet STATIC
    src/TcpServer.cpp
    src/Acceptor.cpp
    src/TcpConnection.cpp
    src/Buffer.cpp
    src/Socket.cpp
    src/Address.cpp
    src/Channel.cpp
    src/Poller.cpp
    src/EventLoop.cpp
    src/detail/LoopWork.cpp
    src/detail/LoopCleanup.cpp
)

target_include_directories(SimpleNet
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/simplenet
)

target_compile_features(SimpleNet PUBLIC cxx_std_20)

option(SIMPLENET_BUILD_EXAMPLES "Build runnable TCP demonstrations" OFF)
if(SIMPLENET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

option(SIMPLENET_BUILD_DOCS "Enable the Doxygen documentation target" ON)

if(SIMPLENET_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(Doxygen_FOUND)
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
            "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            @ONLY
        )
        add_custom_target(docs
            COMMAND Doxygen::doxygen "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Generating SimpleNet documentation."
            VERBATIM
        )
    else()
        message(STATUS "Doxygen not found: the docs target is disabled")
    endif()
endif()
