cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

set(PLUGIN_SOURCES
  src/plugin-main.cpp
  src/hr-ble-source.cpp
  src/hr-ble-source.hpp
  src/ble/ble_manager.hpp
)

if(OS_WINDOWS)
  list(APPEND PLUGIN_SOURCES src/ble/ble_manager_win.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE windowsapp)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WINRT_LEAN_AND_MEAN)
elseif(OS_MACOS)
  list(APPEND PLUGIN_SOURCES src/ble/ble_manager_mac.mm)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "-framework CoreBluetooth" "-framework Foundation")
else()
  list(APPEND PLUGIN_SOURCES src/ble/ble_manager_stub.cpp)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PLUGIN_SOURCES})

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

if(OS_WINDOWS)
  # C++/WinRT needs C++20 and conforming preprocessor on recent MSVC
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /await /EHsc)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
