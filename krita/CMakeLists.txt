add_subdirectory( dtd )
add_subdirectory( data )
add_subdirectory( integration )
# The focused application uses a self-contained vector identity instead of
# Krita's branding resources.

if (AI_STROKE_PAINTER_APP)
    set(AI_STROKE_PAINTER_RUNTIME_NAME "ai-stroke-painter")
    set(AI_STROKE_PAINTER_DESKTOP_ID "org.aistrokepainter.AIStrokePainter")
    set(AI_STROKE_PAINTER_MANIFEST_ID "AIStrokePainter.AIStrokePainter")
    set(AI_STROKE_PAINTER_FILE_DESCRIPTION "AI-first illustration studio")
    set(AI_STROKE_PAINTER_FOUNDATION "AI Stroke Painter contributors")
    set(AI_STROKE_PAINTER_PRODUCT_NAME "AI Stroke Painter")
else()
    set(AI_STROKE_PAINTER_RUNTIME_NAME "krita")
    set(AI_STROKE_PAINTER_DESKTOP_ID "org.kde.krita")
    set(AI_STROKE_PAINTER_MANIFEST_ID "KritaFoundation.Krita.krita")
    set(AI_STROKE_PAINTER_FILE_DESCRIPTION "Krita digital painting application")
    set(AI_STROKE_PAINTER_FOUNDATION "Krita Foundation")
    set(AI_STROKE_PAINTER_PRODUCT_NAME "Krita")
endif()

# GNU windres loses quotes while invoking its preprocessor, which prevents a
# resource-enabled target from building when the source path contains spaces.
# The focused Windows build keeps a plain C++ launcher in that environment;
# the application itself does not depend on embedded resources to start.
set(AI_STROKE_PAINTER_SKIP_MINGW_RESOURCES FALSE)
if (AI_STROKE_PAINTER_APP AND MINGW)
    set(AI_STROKE_PAINTER_SKIP_MINGW_RESOURCES TRUE)
endif()

if (ANDROID)
    if (QT_MAJOR_VERSION STREQUAL "5")
        set (QtAndroidExtras_INCLUDE_DIRS ${Qt5AndroidExtras_INCLUDE_DIRS})
        include_directories (${QtAndroidExtras_INCLUDE_DIRS})
    endif()
endif()

set(krita_SRCS main.cc)

# Set the application icon on the application
if (AI_STROKE_PAINTER_APP)
    set(ICON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/pics/ai-stroke-painter.svg")
elseif (NOT APPLE)
	file(GLOB ICON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/*-apps-krita.png")
else()
    # XCode below 26 cannot work with .icon format
    if(${XCODE_VERSION} VERSION_GREATER_EQUAL "26.0")
        find_program(XCRUN_EXECUTABLE NAMES xcrun)
        set(KRITA_ASSETS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/krita.icon")
        cmake_path(SET ASSETS_PATH "${CMAKE_CURRENT_BINARY_DIR}/assets")

        set(ASSETS_FILES "${ASSETS_PATH}/Assets.car" "${ASSETS_PATH}/krita.icns")
        add_custom_command(
          OUTPUT ${ASSETS_FILES}
          COMMAND ${CMAKE_COMMAND} ARGS -E make_directory "${ASSETS_PATH}"
          COMMAND ${XCRUN_EXECUTABLE} ARGS actool "${KRITA_ASSETS_ICON}"
            --compile ${ASSETS_PATH}
            --platform macosx --minimum-deployment-target ${MACOS_MINIMUM_VERSION}
            --app-icon krita --output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/krita_icon.plist"
            --output-format human-readable-text
            --standalone-icon-behavior all
          DEPENDS
            "${KRITA_ASSETS_ICON}"
          WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
    else()
        set(ICON_SRCS
             "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/16-apps-krita.png"
             "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/32-apps-krita.png"
             "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/48-apps-krita.png"
             "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/128-apps-krita.png"
             "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/256-apps-krita.png"
             "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/512-apps-krita.png"
             "${CMAKE_CURRENT_SOURCE_DIR}/pics/branding/${BRANDING}/1024-apps-krita.png"
        )
    endif() # XCode version
endif()

if (WIN32)
    # Private headers are needed for main.cc to enable WinTab support
    set_source_files_properties(
        main.cc
        PROPERTIES
        INCLUDE_DIRECTORIES "${Qt${QT_MAJOR_VERSION}Gui_PRIVATE_INCLUDE_DIRS}"
        )
endif()

if (WIN32)
    set(krita_windows_stub_ICONS_SRCS "")
    if (NOT AI_STROKE_PAINTER_SKIP_MINGW_RESOURCES)
        ecm_add_app_icon(krita_windows_stub_ICONS_SRCS ICONS ${ICON_SRCS} OUTFILE_BASENAME ${AI_STROKE_PAINTER_RUNTIME_NAME})

        # This is used to generate an ICO file for the file icon, We don't actually
        # want to embed this icon into an EXE.
        file(GLOB krafile_ICON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/pics/mimetypes/*-mimetypes-application-x-krita.png")
        set(krafile_dummy_ICONS_SRCS "")
        ecm_add_app_icon(krafile_dummy_ICONS_SRCS ICONS ${krafile_ICON_SRCS} OUTFILE_BASENAME kritafile)
        add_library(krafile_dummy_obj OBJECT ${krafile_dummy_ICONS_SRCS})

        # These icons are used to replace those in the shell extension package.
        install(
            FILES
                ${CMAKE_CURRENT_BINARY_DIR}/${AI_STROKE_PAINTER_RUNTIME_NAME}.ico
                ${CMAKE_CURRENT_BINARY_DIR}/kritafile.ico
            DESTINATION ${CMAKE_INSTALL_PREFIX}/installer
        )
    endif()
elseif(APPLE)
    if (AI_STROKE_PAINTER_APP)
      ecm_add_app_icon(krita_SRCS OUTFILE_BASENAME ai-stroke-painter ICONS ${ICON_SRCS})
    elseif(${XCODE_VERSION} VERSION_GREATER_EQUAL "26.0")
      set_source_files_properties(${ASSETS_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    else()
      ecm_add_app_icon(krita_SRCS OUTFILE_BASENAME krita ICONS ${ICON_SRCS})
    endif()
else()
    ecm_add_app_icon(krita_SRCS ICONS ${ICON_SRCS})
endif()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pics/ai-stroke-painter.svg"
        DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps)

# ==============================================================================
# KRITA5.XMLGUI UPDATE
#
# The krita5.xmlgui file needs to have its version number updated when it is
# modified. Failing to do so will cause users who have customized their toolbars
# to not see the changes, since that equips them with a local krita5.xmlgui file
# in their application data directory.
#
# On the other hand, if you update the file version, some customizations may
# get lost. If you have changes that are only relevant for certain operating
# systems, you can choose to only bump that system's version here to limit the
# potential blast radius.
#

# Update the version number here. If you have OS-specific changes, put them
# inside of a condition to only bump the version number on that system. If you
# make a change that applies to all systems, remove any conditions. The version
# you specify should always be one higher than any version number you see here,
# no matter what you're doing. Do not re-use versions.
if(ANDROID)
    # Android had interface scaling and supporter benefit menu options added.
    set(KRITA5_XMLGUI_VERSION 544)
else()
    # Other systems aren't affected, they keep using the previous version.
    set(KRITA5_XMLGUI_VERSION 543)
endif()

# After updating the version number properly, put the new hash that the CMake
# error lists as the "actual hash" here.
set(krita5_xmlgui_hash_expected "b74fdc4f3bafac700be2db906a424542")

# You can stop reading now, the stuff that follows doesn't need to be modified.
# ==============================================================================

# Read the krita5.xmlgui file, strip all whitespace from it and then make an
# hash from it. This will also strip whitespace inside of the XML and MD5 isn't
# a great hash, but that's fine to catch any changes a developer actually makes,
# we're not trying to build an enterprise file change detection system here.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/krita5.xmlgui" krita5_xmlgui_content)
string(REGEX REPLACE "[ \t\r\n]+" "" krita5_xmlgui_content "${krita5_xmlgui_content}")
string(MD5 krita5_xmlgui_hash_actual "${krita5_xmlgui_content}")
if(NOT krita5_xmlgui_hash_actual STREQUAL krita5_xmlgui_hash_expected)
    message(FATAL_ERROR
        "krita5.xmlgui has changed!\n"
        "Actual hash:   <${krita5_xmlgui_hash_actual}>\n"
        "Expected hash: <${krita5_xmlgui_hash_expected}>\n"
        "You have to update the hash and increase the version number.\n"
        "Look for 'KRITA5.XMLGUI UPDATE' in krita/CMakeLists.txt for instructions."
    )
endif()

# Generate a krita5.xmlgui in the build directory with the specified version
# number and then a krita.qrc that pulls in that file.
set(KRITA5_XMLGUI_PATH "${CMAKE_CURRENT_BINARY_DIR}/krita5.xmlgui")
set(KRITA_QRC_PATH "${CMAKE_CURRENT_BINARY_DIR}/krita.qrc")
set(KRITARC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/data/kritarc")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/krita5.xmlgui" "${KRITA5_XMLGUI_PATH}" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/krita.qrc" "${KRITA_QRC_PATH}" @ONLY)

# separate listing, both used by Krita and KritaSketch
set(krita_QRCS
    ${KRITA_QRC_PATH}
    ${CMAKE_SOURCE_DIR}/krita/pics/Breeze-dark/breeze-dark-icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/Breeze-light/breeze-light-icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/layerbox/layerbox-icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/layers/layers-icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/misc-light/misc-light-icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/misc-dark/misc-dark-icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/paintops/paintops-icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/tools/SVG/16/tools-svg-16-icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/svg/svg-icons.qrc
    ${CMAKE_SOURCE_DIR}/libs/flake/flake.qrc
    ${CMAKE_SOURCE_DIR}/plugins/paintops/defaultpresets/defaultpresets.qrc
    ${CMAKE_SOURCE_DIR}/libs/widgets/kritawidgets.qrc
    ${CMAKE_SOURCE_DIR}/pics/icons.qrc
    ${CMAKE_SOURCE_DIR}/krita/data/shaders/shaders.qrc
    ${CMAKE_SOURCE_DIR}/krita/data/cursors/cursors.qrc
    ${CMAKE_SOURCE_DIR}/krita/pics/ai-stroke-painter.qrc
    CACHE INTERNAL "krita_QRCS"
)

if (QT_MAJOR_VERSION STREQUAL "6")
    list(APPEND krita_QRCS ${CMAKE_SOURCE_DIR}/krita/data/mime-database/mime-database.qrc)
endif()

qt_add_resources(krita_SRCS ${krita_QRCS})

if (ANDROID AND QT_MAJOR_VERSION STREQUAL "5")
    kis_add_library(krita SHARED ${krita_SRCS})
    target_link_libraries(krita PRIVATE Qt5::AndroidExtras)
elseif (WIN32)
    # Build the main implementation into a DLL to be called by a stub EXE.
    kis_add_library(krita SHARED ${krita_SRCS})
    set_target_properties(krita PROPERTIES PREFIX "" OUTPUT_NAME "${AI_STROKE_PAINTER_RUNTIME_NAME}")
else()
    add_executable(krita ${krita_SRCS})
    set_target_properties(krita PROPERTIES OUTPUT_NAME "${AI_STROKE_PAINTER_RUNTIME_NAME}")
endif()
target_link_libraries(krita
    PRIVATE
                      kritamultiarch
                      kritaui
                      Qt${QT_MAJOR_VERSION}::Core
                      Qt${QT_MAJOR_VERSION}::Gui
                      Qt${QT_MAJOR_VERSION}::Widgets
                      Qt${QT_MAJOR_VERSION}::Xml
                      Qt${QT_MAJOR_VERSION}::Network
                      Qt${QT_MAJOR_VERSION}::PrintSupport
                      Qt${QT_MAJOR_VERSION}::Svg)
if(HAVE_KCRASH)
    target_link_libraries(krita PRIVATE KF${KF_MAJOR}::Crash)
endif()

if (APPLE)
    set(KRITA_VERSION_NUMBER_STRING "${KRITA_STABLE_VERSION_MAJOR}.${KRITA_STABLE_VERSION_MINOR}.${KRITA_VERSION_RELEASE}")
    set_target_properties(krita PROPERTIES INSTALL_RPATH "@loader_path/../../../../lib;@loader_path/../lib;
        @loader_path/../Frameworks;@executable_path/../../../../lib;@executable_path/../lib;@executable_path/../Frameworks")
    set_target_properties(krita PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.template)
    set_target_properties(krita PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "org.krita")
    set_target_properties(krita PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "Krita")
    set_target_properties(krita PROPERTIES MACOSX_BUNDLE_ICON_FILE "krita")
    set_target_properties(krita PROPERTIES MACOSX_BUNDLE_LONG_VERSION_STRING ${KRITA_VERSION_STRING})
    set_target_properties(krita PROPERTIES MACOSX_BUNDLE_SHORT_VERSION_STRING ${KRITA_VERSION_NUMBER_STRING})
    set_target_properties(krita PROPERTIES MACOSX_BUNDLE_BUNDLE_VERSION ${KRITA_VERSION_NUMBER_STRING})
    set_target_properties(krita PROPERTIES MACOSX_BUNDLE_COPYRIGHT "GNU Public License, V2 or, at your option, any later version.")
endif ()

if (NOT ANDROID)
  set(KRITAVERSION_SRCS kritaversion.cpp)
  add_executable(krita_version ${KRITAVERSION_SRCS})
  target_link_libraries(krita_version
    PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        kritaversion
  )

  if(APPLE)
      set_target_properties(krita_version PROPERTIES INSTALL_RPATH "@loader_path/../Frameworks;@executable_path/../lib;@executable_path/../Frameworks")
      set_property(TARGET krita_version PROPERTY MACOSX_BUNDLE OFF)
  endif ()

  install(TARGETS krita_version ${INSTALL_TARGETS_DEFAULT_ARGS})
endif()


if(APPLE)
    set(KRITA_ASSET_KRZ_ICNS "krita-krz.icns" "krita-kra.icns")
    add_custom_command(
      OUTPUT ${KRITA_ASSET_KRZ_ICNS}
      COMMAND ${CMAKE_COMMAND}
      ARGS -E copy
      "./pics/mimetypes/krz/krita-krz.icns"
      "./pics/mimetypes/krita-kra.icns"
      "${CMAKE_CURRENT_BINARY_DIR}"
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_source_files_properties(${KRITA_ASSET_KRZ_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    # make krita depend on assets icons
    if(${XCODE_VERSION} VERSION_GREATER_EQUAL "26.0")
      target_sources(krita PRIVATE ${ASSETS_FILES})
    endif()
    target_sources(krita PRIVATE ${KRITA_ASSET_KRZ_ICNS})

    install(TARGETS krita ${INSTALL_TARGETS_DEFAULT_ARGS} BUNDLE DESTINATION bin)
else()
    install(TARGETS krita ${INSTALL_TARGETS_DEFAULT_ARGS})
endif()
if (AI_STROKE_PAINTER_APP)
    install(FILES org.aistrokepainter.AIStrokePainter.desktop DESTINATION ${KDE_INSTALL_APPDIR})
else()
    install(FILES org.kde.krita.desktop DESTINATION ${KDE_INSTALL_APPDIR})
endif()

set(actions_files krita.action kritamenu.action)
if (ANDROID)
    # In-app-purchase-related actions are only relevant on Android. They
    # wouldn't show up in the menu anyway because the application doesn't
    # instantiate them, but they'd still appear in the keyboard shortcuts.
    list(APPEND actions_files kritamenu_android.action)
endif()
install(FILES ${actions_files} DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions)

if (AI_STROKE_PAINTER_APP)
    install(FILES org.aistrokepainter.AIStrokePainter.appdata.xml DESTINATION ${KDE_INSTALL_METAINFODIR})
else()
    install(FILES org.kde.krita.appdata.xml DESTINATION ${KDE_INSTALL_METAINFODIR})
endif()

install(DIRECTORY DESTINATION ${KDE_INSTALL_DATADIR}/krita/shortcuts)

if (WIN32)
    set(krita_windows_stub_RESOURCE_SRCS "")
    if (NOT AI_STROKE_PAINTER_SKIP_MINGW_RESOURCES)
        configure_file(versioninfo.rc.in ${CMAKE_CURRENT_BINARY_DIR}/versioninfo.rc)
        if (QT_MAJOR_VERSION STREQUAL "6")
            configure_file(krita6.exe.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/${AI_STROKE_PAINTER_RUNTIME_NAME}.exe.manifest)
        else()
            configure_file(krita.exe.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/${AI_STROKE_PAINTER_RUNTIME_NAME}.exe.manifest)
        endif()
        if (MINGW)
            # CMake does not know how to embed manifest on mingw, so we create our own RC file to be included
            file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${AI_STROKE_PAINTER_RUNTIME_NAME}.exe.manifest.rc "1 24 \"${AI_STROKE_PAINTER_RUNTIME_NAME}.exe.manifest\"\n")
            set(KRITA_MANIFEST_SRC ${CMAKE_CURRENT_BINARY_DIR}/${AI_STROKE_PAINTER_RUNTIME_NAME}.exe.manifest.rc)
        else()
            set(KRITA_MANIFEST_SRC
                ${CMAKE_CURRENT_BINARY_DIR}/${AI_STROKE_PAINTER_RUNTIME_NAME}.exe.manifest)
        endif()
        list(APPEND krita_windows_stub_RESOURCE_SRCS
            ${CMAKE_CURRENT_BINARY_DIR}/versioninfo.rc
            ${KRITA_MANIFEST_SRC}
        )
    endif()
    add_executable(krita_windows_stub_exe
        windows_stub_main.cpp
        ${krita_windows_stub_ICONS_SRCS}
        ${krita_windows_stub_RESOURCE_SRCS}
    )
    target_link_libraries(krita_windows_stub_exe PRIVATE krita)
    if (QT_MAJOR_VERSION STREQUAL "6")
        target_link_libraries(krita_windows_stub_exe PRIVATE Qt6::EntryPointPrivate)
    else()
        target_link_libraries(krita_windows_stub_exe PRIVATE Qt5::WinMain)
    endif()
    set_target_properties(krita_windows_stub_exe
        PROPERTIES
            OUTPUT_NAME "${AI_STROKE_PAINTER_RUNTIME_NAME}"
    )
    if (MSVC)
        target_link_options(krita_windows_stub_exe PRIVATE "/DEBUG:NONE")
    endif()
    install(TARGETS krita_windows_stub_exe ${INSTALL_TARGETS_DEFAULT_ARGS})

    add_executable(krita_windows_stub_com
        windows_stub_main.cpp
        ${krita_windows_stub_RESOURCE_SRCS}
    )
    target_link_libraries(krita_windows_stub_com PRIVATE krita)
    set_target_properties(krita_windows_stub_com
        PROPERTIES
            WIN32_EXECUTABLE false
            OUTPUT_NAME "${AI_STROKE_PAINTER_RUNTIME_NAME}"
            SUFFIX ".com"
    )
    if (MSVC)
        target_link_options(krita_windows_stub_com PRIVATE "/DEBUG:NONE")
        # If MSVC uses incremental linking, it will race the database
        # for krita.exe with the one for krita.com.
        add_dependencies(krita_windows_stub_com krita_windows_stub_exe)
    endif()
    install(TARGETS krita_windows_stub_com ${INSTALL_TARGETS_DEFAULT_ARGS})
endif (WIN32)
