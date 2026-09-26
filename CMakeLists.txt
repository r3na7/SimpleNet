cmake_minimum_required(VERSION 3.20)

project(
    SimpleNet
    VERSION 0.1.0
    LANGUAGES CXX
)

add_library(SimpleNet STATIC
    src/Address.cpp
    src/Channel.cpp
    src/Poller.cpp
    src/EventLoop.cpp
)

target_include_directories(SimpleNet
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/simplenet
)

target_compile_features(SimpleNet PUBLIC cxx_std_20)