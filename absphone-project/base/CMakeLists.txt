project(base)

# base 不含任何平台实现：时间、线程、信号量等一律走 Boost / 标准库。
# 平台目录只保留在设备层（media/src/platform），由构建系统选目录。
file(GLOB_RECURSE SOURCES "src/*.cpp")

file(GLOB_RECURSE HEADERS "include/*.h" "include/*.hpp" "src/*.h" "src/*.hpp")

add_library(base SHARED ${SOURCES} ${HEADERS})
# 导出/导入按目标区分：编译 base 用 dllexport，消费者拿 dllimport（非 Windows 为空）
target_compile_definitions(base PRIVATE VCS_BASE_EXPORTS)

# 本地 Boost 预编译库是 vc141（VS2017）。VS2022 的 _MSC_VER 会让 Boost 头文件
# 自动链接 vc143 库名而失败；关闭自动链接，统一使用 find_package 解析到的库。
target_compile_definitions(base PUBLIC BOOST_ALL_NO_LIB)

# Find Boost (Asio + Filesystem)
find_package(Boost REQUIRED COMPONENTS filesystem system)
if(Boost_FOUND)
    message(STATUS "Boost found: version ${Boost_VERSION}")
    message(STATUS "Boost include dir: ${Boost_INCLUDE_DIRS}")
endif()

target_include_directories(base
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link Boost libraries + the platform thread library (PUBLIC: every consumer of
# base also uses std::thread / Asio, and GCC needs -pthread to link them).
target_link_libraries(base
    PUBLIC
        Boost::filesystem
        Boost::system
        Threads::Threads
)

# base is a runtime artifact (process-wide singletons live here), so it must
# ship next to the executables that link it (docs/10 §3.2.10).
install(TARGETS base
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)






