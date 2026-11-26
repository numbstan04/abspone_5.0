project(common)

# common is header-only: cross-module value types (MediaConfig, IModuleFactory)
# that depend on nothing but base. Anything with a .cpp belongs in the module
# that owns the behaviour, not here.
add_library(common INTERFACE)

target_include_directories(common
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(common INTERFACE base)
