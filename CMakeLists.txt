cmake_minimum_required(VERSION 4.0.2)
project(ProjectComplex LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC_COMPILER_PREDEFINES OFF)
if(DEFINED Python_EXECUTABLE)
    set(_clion_python_executable "${Python_EXECUTABLE}")
endif()

find_package(Qt6 REQUIRED COMPONENTS
        Widgets
        Sql
        Quick
        QuickWidgets
        Qml
        Network
        Location
        Positioning
)

add_executable(vk_qt_app
        src/main.cpp

        src/MainWindow.h src/MainWindow.cpp
        src/AppConfig.h  src/AppConfig.cpp
        src/ModelTypes.h src/ModelTypes.cpp

        src/RunnerClient.h src/RunnerClient.cpp
        src/ResultView.h   src/ResultView.cpp

        src/RegressionTab.h   src/RegressionTab.cpp
        src/ClustersTab.h     src/ClustersTab.cpp
        src/FullDistanceTab.h src/FullDistanceTab.cpp
        src/MapTab.h          src/MapTab.cpp

        resources/icons.qrc
        src/app.rc
)

set_source_files_properties(src/ModelTypes.cpp PROPERTIES SKIP_AUTOMOC ON)

target_link_libraries(vk_qt_app PRIVATE
        Qt6::Widgets
        Qt6::Sql
        Qt6::Quick
        Qt6::QuickWidgets
        Qt6::Qml
        Qt6::Network
        Qt6::Location
        Qt6::Positioning
)

if (WIN32)
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
    set(_qt6_bin_dir "${_qt6_prefix}/bin")

    find_program(_windeployqt_exe NAMES windeployqt HINTS "${_qt6_bin_dir}" REQUIRED)

    add_custom_command(TARGET vk_qt_app POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/config"
            "$<TARGET_FILE_DIR:vk_qt_app>/config"

            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/python"
            "$<TARGET_FILE_DIR:vk_qt_app>/python"

            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:vk_qt_app>/assets"

            COMMAND "${_windeployqt_exe}"
            --release
            --no-translations
            --compiler-runtime
            --qmldir "${CMAKE_SOURCE_DIR}"
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

            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:vk_qt_app>/assets"
            VERBATIM
    )
endif()