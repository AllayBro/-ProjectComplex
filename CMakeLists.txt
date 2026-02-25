cmake_minimum_required(VERSION 3.20)
project(ProjectComplex LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)

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

target_link_libraries(vk_qt_app PRIVATE Qt6::Widgets Qt6::Sql)

if (WIN32)
    # Берём префикс Qt из пути к Qt6::Widgets (у вас это .../lib/libQt6Widgets.a)
    get_target_property(_qt6_widgets_loc Qt6::Widgets IMPORTED_LOCATION)
    if (NOT _qt6_widgets_loc)
        get_target_property(_qt6_widgets_loc Qt6::Widgets IMPORTED_LOCATION_DEBUG)
    endif()
    if (NOT _qt6_widgets_loc)
        get_target_property(_qt6_widgets_loc Qt6::Widgets IMPORTED_LOCATION_RELEASE)
    endif()
    if (NOT _qt6_widgets_loc)
        message(FATAL_ERROR "Cannot determine Qt6::Widgets IMPORTED_LOCATION")
    endif()

    get_filename_component(_qt6_lib_dir "${_qt6_widgets_loc}" DIRECTORY)
    get_filename_component(_qt6_prefix  "${_qt6_lib_dir}/.." ABSOLUTE)

    set(_qt6_bin_dir       "${_qt6_prefix}/bin")
    set(_qt6_plugins_dir   "${_qt6_prefix}/plugins")
    set(_qt6_platforms_dir "${_qt6_plugins_dir}/platforms")

    find_program(_windeployqt_exe NAMES windeployqt HINTS "${_qt6_bin_dir}" REQUIRED)

    add_custom_command(TARGET vk_qt_app POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/config"
            "$<TARGET_FILE_DIR:vk_qt_app>/config"

            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/python"
            "$<TARGET_FILE_DIR:vk_qt_app>/python"

            COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${_qt6_bin_dir}"
            "QT_PLUGIN_PATH=${_qt6_plugins_dir}"
            "QT_QPA_PLATFORM_PLUGIN_PATH=${_qt6_platforms_dir}"
            --
            "${_windeployqt_exe}"
            --no-translations
            --compiler-runtime
            --dir "$<TARGET_FILE_DIR:vk_qt_app>"
            "$<TARGET_FILE:vk_qt_app>"
            VERBATIM
    )
else()
    add_custom_command(TARGET vk_qt_app POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/config"
            "$<TARGET_FILE_DIR:vk_qt_app>/config"

            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/python"
            "$<TARGET_FILE_DIR:vk_qt_app>/python"
            VERBATIM
    )
endif()