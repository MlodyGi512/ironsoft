find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

add_library(ekinox_config STATIC
    src/ekinox_config.cpp
)

target_include_directories(ekinox_config
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ekinox_config
    PUBLIC
        PkgConfig::JSONCPP
)

add_executable(ekinox_config_dump
    src/config_dump_main.cpp
)

target_link_libraries(ekinox_config_dump
    PRIVATE
        ekinox_config
)
