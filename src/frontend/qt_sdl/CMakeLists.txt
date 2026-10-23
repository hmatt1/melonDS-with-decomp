include(CMakeDependentOption)

include(FixInterfaceIncludes)

set(SOURCES_QT_SDL
    main.cpp
    main_shaders.h
    Screen.cpp
    Window.cpp
    EmuInstance.cpp
    EmuInstanceAudio.cpp
    EmuInstanceInput.cpp
    EmuThread.cpp
    CheatImportDialog.cpp
    CheatsDialog.cpp
    Config.cpp
    DateTimeDialog.cpp
    EmuSettingsDialog.cpp
    PowerManagement/PowerManagementDialog.cpp
    PowerManagement/resources/battery.qrc
    InputConfig/InputConfigDialog.cpp
    InputConfig/MapButton.h
    InputConfig/resources/ds.qrc
    VideoSettingsDialog.cpp
    CameraSettingsDialog.cpp
    AudioSettingsDialog.cpp
    FirmwareSettingsDialog.cpp
    PathSettingsDialog.cpp
    MPSettingsDialog.cpp
    WifiSettingsDialog.cpp
    InterfaceSettingsDialog.cpp
    ROMInfoDialog.cpp
    RAMInfoDialog.cpp
    TitleManagerDialog.cpp
    OSD_shaders.h
    font.h
    Platform.cpp
    Platform_AAC.cpp
    QPathInput.h
    SaveManager.cpp
    CameraManager.cpp
    AboutDialog.cpp
    AboutDialog.h
    AboutDialog.ui
    ARCodeQDecl.h

    ArchiveUtil.h
    ArchiveUtil.cpp

    ../ScreenLayout.cpp
    ../mic_blow.h

    ../glad/glad.c
    ../duckstation/gl/context.cpp

    ${CMAKE_SOURCE_DIR}/res/melon.qrc

    CLI.h
    CLI.cpp

    LANDialog.cpp
    NetplayDialog.cpp

    workbench/WBHost.cpp
    workbench/WBQueries.cpp
    workbench/GameArchive.cpp
    workbench/GameArchive.h
    workbench/Containers.cpp
    workbench/Containers.h
    workbench/KnowledgeStore.cpp
    workbench/KbQueries.cpp
    workbench/ApiServer.cpp
    workbench/ApiConnections.cpp
    workbench/WorkbenchWindow.cpp
    workbench/WBLayout.cpp
    workbench/WBClassify.cpp
    workbench/WBAnalysis.cpp
    workbench/WBHelp.cpp
    workbench/WBCoverage.cpp
    workbench/ViewPanels.cpp
    workbench/PanelsA.cpp
    workbench/PanelsB.cpp
    workbench/Dialogs.cpp
    workbench/WBStatusWidget.cpp
    workbench/WBColors.cpp
    workbench/WBCompare.cpp
    workbench/WBCompare.h
    workbench/WBGfx.cpp
    workbench/WBGfx.h
    workbench/WBAssets.cpp
    workbench/WBGraphics.cpp
    workbench/WBAssets.h
    workbench/WBCompareUi.cpp
    workbench/WBCodeEdit.cpp
    workbench/WBCodeEdit.h
    workbench/compare.qrc
    workbench/icons.qrc
    workbench/kb/kb.qrc
    workbench/Panels.h
    workbench/WorkbenchWindow.h
    workbench/WBHost.h
    workbench/ApiServer.h
    workbench/ApiConnections.h
    workbench/Dialogs.h
    workbench/KnowledgeStore.h
    workbench/KbQueries.h
    workbench/WBStatusWidget.h
    workbench/WBColors.h
)

option(USE_QT6 "Use Qt 6 instead of Qt 5" ON)

if (USE_QT6)
    find_package(Qt6 COMPONENTS Core Gui Widgets Network Multimedia OpenGL OpenGLWidgets Svg REQUIRED)
    set(QT_LINK_LIBS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::OpenGL Qt6::OpenGLWidgets)
else()
    find_package(Qt5 COMPONENTS Core Gui Widgets Network Multimedia Svg REQUIRED)
    set(QT_LINK_LIBS Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::Multimedia)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

if (BUILD_STATIC)
    list(APPEND PKG_CONFIG_EXECUTABLE "--static")
endif()

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(LibArchive REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(Zstd REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(Faad REQUIRED IMPORTED_TARGET faad2)
pkg_check_modules(SQLite3 REQUIRED IMPORTED_TARGET sqlite3)
find_package(ZLIB REQUIRED)    # workbench: inflating game archives

fix_interface_includes(PkgConfig::SDL2 PkgConfig::LibArchive)

add_compile_definitions(ARCHIVE_SUPPORT_ENABLED)

# DS Decomp Workbench: API types (C++) and the Python bindings are generated from the shared schema
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(WB_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/../../workbench/schema/workbench.schema.json")
set(WB_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/workbench/wbgen.py")
set(WB_API_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/wbgen")
set(WB_PY_OUT "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/workbench/python/dswb/_generated.py")
add_custom_command(
    OUTPUT "${WB_API_GEN_DIR}/WBApi.h" "${WB_API_GEN_DIR}/WBApi.cpp" "${WB_PY_OUT}"
    COMMAND Python3::Interpreter "${WB_GENERATOR}" --schema "${WB_SCHEMA}" --cpp-api "${WB_API_GEN_DIR}" --python "${WB_PY_OUT}"
    DEPENDS "${WB_SCHEMA}" "${WB_GENERATOR}"
    COMMENT "Generating workbench API types and Python bindings")
list(APPEND SOURCES_QT_SDL "${WB_API_GEN_DIR}/WBApi.h" "${WB_API_GEN_DIR}/WBApi.cpp")

add_executable(melonDS ${SOURCES_QT_SDL})
target_include_directories(melonDS PRIVATE "${WB_API_GEN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/workbench")

# workbench unit tests (ctest -R workbench)
add_executable(wbtests EXCLUDE_FROM_ALL
    workbench/tests/wbtests.cpp
    workbench/ApiConnections.cpp
    workbench/KnowledgeStore.cpp
    workbench/KbQueries.cpp
    workbench/WBClassify.cpp
    "${WB_API_GEN_DIR}/WBApi.cpp"
    workbench/KnowledgeStore.h
    workbench/GameArchive.cpp
    workbench/Containers.cpp
    workbench/WBCompare.cpp
    workbench/WBGfx.cpp
    workbench/kb/kb.qrc
    ../../workbench/Expr.cpp
    ../../workbench/Disasm.cpp
    ../../workbench/FlowDisasm.cpp
    ../../workbench/Explain.cpp
    ../../workbench/InstrOps.cpp
    ../../workbench/NitroROM.cpp
    ../../workbench/Recorder.cpp
    ../../workbench/Origin.cpp
    ../../workbench/Debugger.cpp
    ../../workbench/Session.cpp
    ../../sha1/sha1.c)
target_include_directories(wbtests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/workbench" "${WB_API_GEN_DIR}" $<TARGET_PROPERTY:core,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(wbtests PRIVATE $<TARGET_PROPERTY:core,INTERFACE_COMPILE_DEFINITIONS>)
add_dependencies(wbtests core)

add_subdirectory("../../net"
    ${CMAKE_BINARY_DIR}/net)

target_link_libraries(melonDS PRIVATE net-utils)

if (WIN32)
    target_link_libraries(melonDS PUBLIC opengl32)

    target_sources(melonDS PRIVATE
        ../duckstation/gl/context_wgl.cpp

        ../glad/glad_wgl.c
    )

    if (MINGW AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set_property(TARGET melonDS PROPERTY AUTORCC_OPTIONS "--no-zstd")
    endif()

    if (BUILD_STATIC)
        find_package(SDL2 CONFIG COMPONENTS SDL2main)
        if (TARGET SDL2::SDL2main)
            target_link_libraries(melonDS PRIVATE SDL2::SDL2main)
        endif()
    endif()
elseif (APPLE)
    if (NOT USE_QT6)
        find_library(COCOA_LIB Cocoa)
        target_link_libraries(melonDS PRIVATE ${COCOA_LIB})
    endif()
    find_library(OPENGL_LIB OpenGL)
    target_link_libraries(melonDS PRIVATE ${OPENGL_LIB})
    target_sources(melonDS PRIVATE
        ../duckstation/gl/context_agl.mm
    )
    set_source_files_properties(
        ../duckstation/gl/context_agl.mm
        PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations"
    )
else()
    find_package(X11 REQUIRED)
    find_package(EGL REQUIRED)

    option(ENABLE_WAYLAND "Enable Wayland support" ON)

    if (ENABLE_WAYLAND)
        # we only need ECM for Wayland
        # so we only require it from here
        find_package(ECM REQUIRED NO_MODULE)
        list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")

        find_package(Wayland REQUIRED Client)

        target_compile_definitions(melonDS PRIVATE WAYLAND_ENABLED)

        target_sources(melonDS PRIVATE
            ../duckstation/gl/context_egl_wayland.cpp
        )
    endif()

    target_sources(melonDS PRIVATE
        ../duckstation/gl/context_egl.cpp
        ../duckstation/gl/context_egl_x11.cpp
        ../duckstation/gl/context_glx.cpp
        ../duckstation/gl/x11_window.cpp

        ../glad/glad_egl.c
        ../glad/glad_glx.c
    )
    target_link_libraries(melonDS PRIVATE "${X11_LIBRARIES}" "${EGL_LIBRARIES}")
    target_include_directories(melonDS PRIVATE "${X11_INCLUDE_DIR}")
    add_compile_definitions(QAPPLICATION_CLASS=QApplication)
endif()


if (BUILD_STATIC)
    qt_import_plugins(melonDS INCLUDE Qt::QSvgPlugin)
    if (WIN32 AND USE_QT6)
        qt_import_plugins(melonDS INCLUDE Qt::QModernWindowsStylePlugin)
        # The TIFF plugin pulls jbig, lerc and libdeflate, which melonDS never decodes.
        qt_import_plugins(melonDS EXCLUDE Qt::QTiffPlugin)
    endif()
    if (USE_VCPKG AND UNIX AND NOT APPLE)
        pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
        target_link_libraries(melonDS PRIVATE PkgConfig::ALSA)
    elseif (NOT APPLE)
        target_link_options(melonDS PRIVATE -static)
    endif()
endif()

target_include_directories(melonDS PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/..")

if (USE_QT6)
    target_include_directories(melonDS PUBLIC ${Qt6Gui_PRIVATE_INCLUDE_DIRS})
else()
    target_include_directories(melonDS PUBLIC ${Qt5Gui_PRIVATE_INCLUDE_DIRS})
endif()
target_link_libraries(melonDS PRIVATE core)
target_link_libraries(melonDS PRIVATE PkgConfig::SDL2 PkgConfig::LibArchive PkgConfig::Zstd PkgConfig::Faad PkgConfig::SQLite3 ZLIB::ZLIB)
# workbench: Qt Advanced Docking System (MSYS2: mingw-w64-ucrt-x86_64-qt-advanced-docking-system)
find_package(qtadvanceddocking-qt6 REQUIRED)
target_link_libraries(melonDS PRIVATE ads::qtadvanceddocking-qt6)
target_link_libraries(melonDS PRIVATE ${QT_LINK_LIBS} ${CMAKE_DL_LIBS})
if (BUILD_STATIC AND MINGW)
    # FreeType, HarfBuzz and their static dependencies refer to each other. One more
    # pass, in pkg-config's static order, closes the references Qt left open.
    pkg_check_modules(FONT_STATIC REQUIRED IMPORTED_TARGET freetype2)
    target_link_libraries(melonDS PRIVATE PkgConfig::FONT_STATIC)
endif()
target_link_libraries(wbtests PRIVATE PkgConfig::SQLite3 ZLIB::ZLIB ${QT_LINK_LIBS})
enable_testing()
add_test(NAME workbench_unit COMMAND wbtests)

if (WIN32)
    option(PORTABLE "Make a portable build that looks for its configuration in the current directory" ON)

    if (PORTABLE)
        target_compile_definitions(melonDS PRIVATE WIN32_PORTABLE)
    endif()

    string(REPLACE . , MELON_RC_VERSION ${melonDS_VERSION})
    configure_file("${CMAKE_SOURCE_DIR}/res/melon.rc.in" "${CMAKE_BINARY_DIR}/res/melon.rc")
    target_sources(melonDS PUBLIC "${CMAKE_BINARY_DIR}/res/melon.rc")
    target_include_directories(melonDS PRIVATE "${CMAKE_BINARY_DIR}/res")

    if (${ARCHITECTURE} STREQUAL x86_64)
        set(WIN32_ARCHITECTURE amd64)
    elseif (${ARCHITECTURE} STREQUAL x86)
        set(WIN32_ARCHITECTURE x86)
    elseif (${ARCHITECTURE} MATCHES "ARM(64)?")
        string(TOLOWER "${ARCHITECTURE}" WIN32_ARCHITECTURE)
    endif()


    target_link_libraries(melonDS PRIVATE ws2_32 iphlpapi)
    set_target_properties(melonDS PROPERTIES WIN32_EXECUTABLE $<IF:$<CONFIG:Debug>,OFF,ON>)

    if (MINGW)
        configure_file("${CMAKE_SOURCE_DIR}/res/xp.manifest.in" "${CMAKE_BINARY_DIR}/res/xp.manifest")
    else()
        # Work around faad2 unconditionally linking libm even on Windows
        if (USE_VCPKG)
            add_library(m STATIC)
            target_link_libraries(melonDS PUBLIC m)
        endif()
    endif()
endif()

if (APPLE)
    # Copy icon into the bundle
    set(RESOURCE_FILES "${CMAKE_SOURCE_DIR}/res/melon.icns")
    target_sources(melonDS PUBLIC "${RESOURCE_FILES}")

    set_target_properties(melonDS PROPERTIES
        MACOSX_BUNDLE true
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/res/melon.plist.in
        OUTPUT_NAME melonDS
        RESOURCE "${RESOURCE_FILES}")


    option(MACOS_BUNDLE_LIBS "Bundle libraries with the app on macOS" OFF)
    option(MACOS_BUILD_DMG "Build DMG image of the macOS application bundle" OFF)

    if (MACOS_BUNDLE_LIBS)
        if (MACOS_BUILD_DMG)
            set(DMGARG "--dmg")
        endif()
        add_custom_command(TARGET melonDS POST_BUILD
            COMMAND ${CMAKE_SOURCE_DIR}/tools/mac-libs.rb ${DMGARG} ${CMAKE_BINARY_DIR}
            COMMENT "Bundling macOS libraries...")
    endif()
endif()

if (UNIX AND NOT APPLE)
    foreach(SIZE 16 32 48 64 128 256)
        install(FILES ${CMAKE_SOURCE_DIR}/res/icon/melon_${SIZE}x${SIZE}.png
            DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/${SIZE}x${SIZE}/apps
            RENAME net.kuribo64.melonDS.png)
    endforeach()

    install(FILES ${CMAKE_SOURCE_DIR}/res/net.kuribo64.melonDS.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications)
    install(TARGETS melonDS BUNDLE DESTINATION ${CMAKE_BINARY_DIR} RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)

    if (NOT USE_QT6)
        set_target_properties(melonDS PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION OFF
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
    endif()
elseif(APPLE)
    install(TARGETS melonDS BUNDLE DESTINATION "${CMAKE_INSTALL_PREFIX}/Applications")
endif()

if (ENABLE_OGLRENDERER)
    set(MELONDS_GL_HEADER \"frontend/glad/glad.h\" CACHE STRING "Path to a header that contains OpenGL function and type declarations.")

    target_compile_definitions(melonDS PUBLIC OGLRENDERER_ENABLED)
    target_compile_definitions(melonDS PUBLIC MELONDS_GL_HEADER=${MELONDS_GL_HEADER})
    target_compile_definitions(core PUBLIC MELONDS_GL_HEADER=${MELONDS_GL_HEADER})
endif()
