# 每个子目录产出一个可执行文件，新增 Demo 时在列表里加一项。
set(NETPROXY_APP_DIRS
    demo
)

foreach(app_dir IN LISTS NETPROXY_APP_DIRS)
    add_subdirectory(${app_dir})
endforeach()
