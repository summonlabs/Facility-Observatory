cmake_minimum_required(VERSION 3.20)

# An independent out-of-tree consumer of the installed Facility Observatory
# package. It deliberately does not use add_subdirectory or a source-tree path:
# if the installed package is incomplete, this fails to configure.
project(FacilityObservatoryConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FacilityObservatory 1.0 REQUIRED)

add_executable(observatory_consumer main.cpp)
target_link_libraries(observatory_consumer PRIVATE FacilityObservatory::facility_observatory)
