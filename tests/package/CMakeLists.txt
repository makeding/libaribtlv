cmake_minimum_required(VERSION 3.16)
project(aribtlv-package-consumer LANGUAGES CXX)

find_package(aribtlv CONFIG REQUIRED)

add_executable(aribtlv-package-consumer main.cpp)
target_link_libraries(aribtlv-package-consumer PRIVATE aribtlv::aribtlv)

