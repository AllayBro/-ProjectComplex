cmake_minimum_required(VERSION 4.0)
project(ProjectComplex LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(vk_qt_app
        src/main.cpp
        src/MainWindow.h src/MainWindow.cpp
        src/AppConfig.h src/AppConfig.cpp
        src/ModelTypes.h
        src/RunnerClient.h src/RunnerClient.cpp
        src/ResultView.h src/ResultView.cpp
        src/RegressionTab.h src/RegressionTab.cpp
        src/ClustersTab.h src/ClustersTab.cpp
        src/FullDistanceTab.h src/FullDistanceTab.cpp
)

target_link_libraries(vk_qt_app PRIVATE Qt6::Widgets)

if (WIN32)
    get_target_property(_qt6_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt6_bin_dir "${_qt6_qmake_exe}" DIRECTORY)
    set(_windeployqt_exe "${_qt6_bin_dir}/windeployqt.exe")

    # Пути к плагинам Qt (нужно, чтобы запустился САМ windeployqt)
    get_filename_component(_qt6_prefix "${_qt6_bin_dir}/.." ABSOLUTE)
    set(_qt6_plugins_dir   "${_qt6_prefix}/plugins")
    set(_qt6_platforms_dir "${_qt6_plugins_dir}/platforms")

    add_custom_command(TARGET vk_qt_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/config
            $<TARGET_FILE_DIR:vk_qt_app>/config
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/python
            $<TARGET_FILE_DIR:vk_qt_app>/python

            # ВАЖНО: задаём окружение, чтобы windeployqt нашёл платформенный плагин
            COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${_qt6_bin_dir};$ENV{PATH}"
            "QT_PLUGIN_PATH=${_qt6_plugins_dir}"
            "QT_QPA_PLATFORM_PLUGIN_PATH=${_qt6_platforms_dir}"
            "${_windeployqt_exe}" --no-translations --compiler-runtime --debug
            --dir "$<TARGET_FILE_DIR:vk_qt_app>"
            "$<TARGET_FILE:vk_qt_app>"
    )
else()
    add_custom_command(TARGET vk_qt_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/config
            $<TARGET_FILE_DIR:vk_qt_app>/config
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/python
            $<TARGET_FILE_DIR:vk_qt_app>/python
    )
endif()