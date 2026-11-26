project(conference)

file(GLOB_RECURSE SOURCES "src/*.cpp")

file(GLOB_RECURSE HEADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")


source_group("Header Files" FILES ${HEADER_FILES})

file(GLOB_RECURSE HEADERS "include/*.h" "include/*.hpp" "src/*.h" "src/*.hpp")

add_library(conference STATIC ${SOURCES} ${HEADERS})


target_include_directories(conference
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)


target_link_libraries(conference PUBLIC base common media gateway)




