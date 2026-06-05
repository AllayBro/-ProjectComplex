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

add_executable(traffic
        src/main.cpp

        src/MainWindow.h src/MainWindow.cpp
        src/AppConfig.h src/AppConfig.cpp
        src/ModelTypes.h src/ModelTypes.cpp

        src/RunnerClient.h src/RunnerClient.cpp
        src/ResultView.h src/ResultView.cpp

        src/RegressionTab.h src/RegressionTab.cpp
        src/ClustersTab.h src/ClustersTab.cpp
        src/FullDistanceTab.h src/FullDistanceTab.cpp
        src/MapTab.h src/MapTab.cpp

        resources/icons.qrc
        src/app.rc
)

if(WIN32)
    set_target_properties(traffic PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

set_source_files_properties(src/ModelTypes.cpp PROPERTIES SKIP_AUTOMOC ON)

target_link_libraries(traffic PRIVATE
        Qt6::Widgets
        Qt6::Sql
        Qt6::Quick
        Qt6::QuickWidgets
        Qt6::Qml
        Qt6::Network
        Qt6::Location
        Qt6::Positioning
)

enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Core Test)

function(traffic_add_qt_test target_name test_source)
    add_executable(${target_name} EXCLUDE_FROM_ALL ${test_source})
    target_link_libraries(${target_name} PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(${target_name} PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endfunction()

traffic_add_qt_test(test_appconfig test/test_appconfig.cpp)
target_sources(test_appconfig PRIVATE src/AppConfig.cpp)

traffic_add_qt_test(test_modeltypes test/test_modeltypes.cpp)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
            NAME pytest_python
            COMMAND ${Python3_EXECUTABLE} -m pytest ${CMAKE_SOURCE_DIR}/test/python -c ${CMAKE_SOURCE_DIR}/pytest.ini
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(pytest_python PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/py_src"
    )
endif()

set(PY_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/py_runtime")
set(HAS_BUNDLED_PY_RUNTIME FALSE)

if(WIN32)
    if(EXISTS "${PY_RUNTIME_DIR}/python.exe" AND EXISTS "${PY_RUNTIME_DIR}/pythonw.exe")
        set(HAS_BUNDLED_PY_RUNTIME TRUE)
    endif()
endif()

if(WIN32)
    get_target_property(_qt6_widgets_loc Qt6::Widgets IMPORTED_LOCATION)
    if(NOT _qt6_widgets_loc)
        get_target_property(_qt6_widgets_loc Qt6::Widgets IMPORTED_LOCATION_DEBUG)
    endif()
    if(NOT _qt6_widgets_loc)
        get_target_property(_qt6_widgets_loc Qt6::Widgets IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT _qt6_widgets_loc)
        message(FATAL_ERROR "Cannot determine Qt6::Widgets IMPORTED_LOCATION")
    endif()

    get_filename_component(_qt6_lib_dir "${_qt6_widgets_loc}" DIRECTORY)
    get_filename_component(_qt6_prefix "${_qt6_lib_dir}/.." ABSOLUTE)
    set(_qt6_bin_dir "${_qt6_prefix}/bin")

    find_program(_windeployqt_exe NAMES windeployqt HINTS "${_qt6_bin_dir}" REQUIRED)

    if(HAS_BUNDLED_PY_RUNTIME)
        add_custom_command(TARGET traffic POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/config"
                "$<TARGET_FILE_DIR:traffic>/config"

                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${PY_RUNTIME_DIR}"
                "$<TARGET_FILE_DIR:traffic>/py"

                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PY_RUNTIME_DIR}/pythonw.exe"
                "$<TARGET_FILE_DIR:traffic>/py/traffic_py.exe"

                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/py_src"
                "$<TARGET_FILE_DIR:traffic>/python"

                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:traffic>/assets"

                COMMAND "${_windeployqt_exe}"
                --release
                --no-translations
                --compiler-runtime
                --qmldir "${CMAKE_SOURCE_DIR}"
                --dir "$<TARGET_FILE_DIR:traffic>"
                "$<TARGET_FILE:traffic>"
                VERBATIM
        )
    else()
        add_custom_command(TARGET traffic POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/config"
                "$<TARGET_FILE_DIR:traffic>/config"

                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/py_src"
                "$<TARGET_FILE_DIR:traffic>/python"

                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:traffic>/assets"

                COMMAND "${_windeployqt_exe}"
                --release
                --no-translations
                --compiler-runtime
                --qmldir "${CMAKE_SOURCE_DIR}"
                --dir "$<TARGET_FILE_DIR:traffic>"
                "$<TARGET_FILE:traffic>"
                VERBATIM
        )
    endif()
else()
    add_custom_command(TARGET traffic POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/config"
            "$<TARGET_FILE_DIR:traffic>/config"

            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/py_src"
            "$<TARGET_FILE_DIR:traffic>/python"

            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:traffic>/assets"
            VERBATIM
    )
endif()
