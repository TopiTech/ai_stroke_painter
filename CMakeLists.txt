cmake_minimum_required(VERSION 3.19.0 FATAL_ERROR)
option(AI_STROKE_STANDALONE_TESTS "Build standalone AI stroke tests without full Krita dependency stack" OFF)
if (AI_STROKE_STANDALONE_TESTS)
    project(ai_stroke_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Test)
    set(CMAKE_AUTOMOC ON)
    enable_testing()

    add_definitions(-DAI_STROKE_STANDALONE)

    add_library(ai_stroke_core STATIC
        libs/ui/aiillustration/KisAiPromptAnalyzer.cpp
        libs/ui/aiillustration/KisAiStrokeTypeChecker.cpp
        libs/ui/aiillustration/KisAiStrokeQualityUtils.cpp
        libs/ui/aiillustration/KisAiStrokeProgram.cpp
        libs/ui/aiillustration/KisAiStrokeRenderer.cpp
        libs/ui/aiillustration/KisAiIllustrationRenderer.cpp
        libs/ui/aiillustration/KisAiSceneSpec.cpp
        libs/ui/aiillustration/KisAiLayoutEngine.cpp
        libs/ui/aiillustration/KisAiLightRig.cpp
    )
    target_include_directories(ai_stroke_core PUBLIC
        libs/ui
    )
    target_link_libraries(ai_stroke_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network)

    add_executable(KisAiStrokeProgramTest
        libs/ui/tests/KisAiStrokeProgramTest.cpp
        libs/ui/tests/KisAiTestUtils.cpp
    )
    target_include_directories(KisAiStrokeProgramTest PRIVATE
        libs/ui
        libs/ui/tests
    )
    target_link_libraries(KisAiStrokeProgramTest PRIVATE ai_stroke_core Qt6::Test)
    add_test(NAME KisAiStrokeProgramTest COMMAND KisAiStrokeProgramTest)

    add_executable(KisAiStrokeRendererTest
        libs/ui/tests/KisAiStrokeRendererTest.cpp
        libs/ui/tests/KisAiTestUtils.cpp
    )
    target_include_directories(KisAiStrokeRendererTest PRIVATE
        libs/ui
        libs/ui/tests
    )
    target_link_libraries(KisAiStrokeRendererTest PRIVATE ai_stroke_core Qt6::Test)
    add_test(NAME KisAiStrokeRendererTest COMMAND KisAiStrokeRendererTest)

    add_executable(KisAiIllustrationRendererTest
        libs/ui/tests/KisAiIllustrationRendererTest.cpp
    )
    target_include_directories(KisAiIllustrationRendererTest PRIVATE
        libs/ui
        libs/ui/tests
    )
    target_link_libraries(KisAiIllustrationRendererTest PRIVATE ai_stroke_core Qt6::Test)
    add_test(NAME KisAiIllustrationRendererTest COMMAND KisAiIllustrationRendererTest)
    set_tests_properties(
        KisAiStrokeProgramTest
        KisAiStrokeRendererTest
        KisAiIllustrationRendererTest
        PROPERTIES
            LABELS "AIStroke"
            TIMEOUT 60
    )

    # Qt and MinGW DLLs live outside the build tree on Windows. Keep the
    # documented `ctest --test-dir build-test` command self-contained instead
    # of requiring callers to prepare PATH manually.
    if (WIN32)
        get_filename_component(ai_stroke_compiler_runtime_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(ai_stroke_test_runtime_path
            "$<TARGET_FILE_DIR:Qt6::Core>\\;${ai_stroke_compiler_runtime_dir}\\;$ENV{PATH}")
        set_tests_properties(
            KisAiStrokeProgramTest
            KisAiStrokeRendererTest
            KisAiIllustrationRendererTest
            PROPERTIES ENVIRONMENT "PATH=${ai_stroke_test_runtime_path}")
    endif()

    return()
endif()

set(SUPPORTED_LANGUAGES CXX C)

if (APPLE)
    list (APPEND SUPPORTED_LANGUAGES OBJC OBJCXX)
endif()

project(krita LANGUAGES ${SUPPORTED_LANGUAGES})
message(STATUS "Using CMake version: ${CMAKE_VERSION}")

# This repository is a focused AI illustration application.  Keep the normal
# test build opt-in so a distribution configure does not accidentally pull in
# the large upstream Krita test suite.
option(BUILD_TESTING "Build test targets" OFF)

# Select the Qt version we want to use
# Qt5 is the default, set QT_MAJOR_VERSION to 6 to look for Qt6
option(BUILD_WITH_QT6 "Build against Qt 6" OFF)

# This repository is a focused AI illustration application, not a selectable
# Krita build variant.  Keep a compile-time marker for the small amount of
# shared infrastructure that has an AI-specific code path.
set(AI_STROKE_PAINTER_APP ON)
add_compile_definitions(AI_STROKE_PAINTER_APP=1)

option(AI_STROKE_PAINTER_BUILD_UPSTREAM_TESTS
    "Build and register upstream Krita tests in addition to the focused AI tests"
    OFF)
mark_as_advanced(AI_STROKE_PAINTER_BUILD_UPSTREAM_TESTS)

set(AI_STROKE_PAINTER_ENABLE_UPSTREAM_TESTS OFF)
if (BUILD_TESTING AND AI_STROKE_PAINTER_BUILD_UPSTREAM_TESTS)
    set(AI_STROKE_PAINTER_ENABLE_UPSTREAM_TESTS ON)
endif()

if (BUILD_TESTING)
    if (AI_STROKE_PAINTER_ENABLE_UPSTREAM_TESTS)
        message(STATUS "AI Stroke Painter upstream Krita tests are enabled")
    else()
        message(STATUS "AI Stroke Painter focused test suite is enabled")
    endif()
endif()

if (BUILD_WITH_QT6)
set(MIN_QT_VERSION 6.0.0)
set(MIN_FRAMEWORKS_VERSION 6.0.0)
else()
set(MIN_QT_VERSION 5.15.0)
set(MIN_FRAMEWORKS_VERSION 5.44.0)
endif()

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (POLICY CMP0002)
    cmake_policy(SET CMP0002 NEW)
endif()

if (POLICY CMP0017)
    cmake_policy(SET CMP0017 NEW)
endif ()

if (POLICY CMP0020)
    cmake_policy(SET CMP0020 NEW)
endif ()

if (POLICY CMP0022)
    cmake_policy(SET CMP0022 NEW)
endif ()

if (POLICY CMP0026)
    cmake_policy(SET CMP0026 NEW)
endif()

if (POLICY CMP0042)
    cmake_policy(SET CMP0042 NEW)
endif()

if (POLICY CMP0046)
    cmake_policy(SET CMP0046 NEW)
endif ()

if (POLICY CMP0059)
    cmake_policy(SET CMP0059 NEW)
endif()

if (POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif()

if (POLICY CMP0054)
    cmake_policy(SET CMP0054 NEW)
endif()

if (POLICY CMP0064)
    cmake_policy(SET CMP0064 NEW)
endif()

if (POLICY CMP0071)
    cmake_policy(SET CMP0071 NEW)
endif()

if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# in CMake >=3.30; BoostConfig.cmake is shipped only by Boost >=1.82
if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (APPLE)
    add_subdirectory(packaging/macos)

    if(NOT CMAKE_GENERATOR MATCHES "Xcode")
      execute_process(COMMAND
        xcodebuild -version -sdk macosx ProductVersion
        OUTPUT_VARIABLE XCODE_VERSION)
    endif()
    message(STATUS "XCode version: ${XCODE_VERSION}")
    set(MACOSX_RPATH TRUE)
    set(APPLE_SUPPRESS_X11_WARNING TRUE)
    set(KDE_SKIP_RPATH_SETTINGS TRUE)
    set(CMAKE_FIND_FRAMEWORK LAST)
    set(CMAKE_MACOSX_RPATH 1)
    set(BUILD_WITH_INSTALL_RPATH 1)
    set(MACOS_GUI_TEST "GUI")
    if (BUILD_WITH_QT6)
        set(MACOS_MINIMUM_VERSION 12)
    else()
        set(MACOS_MINIMUM_VERSION 10.15)
    endif()
    add_definitions(-mmacosx-version-min=${MACOS_MINIMUM_VERSION} -Wno-macro-redefined -Wno-deprecated-register)
    if (CMAKE_OSX_ARCHITECTURES)
        message(STATUS "CMake OSX architectures: ${CMAKE_OSX_ARCHITECTURES}")
    endif()

endif()

function(macos_test_fixrpath)
    if (APPLE AND BUILD_TESTING)
    foreach(TEST IN LISTS ARGN)
        set_property(TARGET ${TEST}
                     PROPERTY BUILD_RPATH "@loader_path/../../../../lib;@loader_path/../lib;@loader_path/../Frameworks;${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_LIBDIR}")
    endforeach()
    endif()
endfunction()

if (CMAKE_COMPILER_IS_GNUCXX AND NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.9 AND NOT WIN32)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-suggest-override> -Wextra -Wno-class-memaccess)
endif()

 ######################
#######################
## Constants defines ##
#######################
######################
if (BUILD_WITH_QT6)
set(KRITA_VERSION_STRING "6.1.0-prealpha")
set(KRITA_STABLE_VERSION_MAJOR 6)
set(KRITA_STABLE_VERSION_MINOR 1)
else()
# define common versions of Krita applications, used to generate kritaversion.h
# update these version for every release:
set(KRITA_VERSION_STRING "5.4.0-prealpha")
# Major version: 3 for 3.x, 4 for 4.x, etc.
set(KRITA_STABLE_VERSION_MAJOR 5)
# Minor version: 0 for 4.0, 1 for 4.1, etc.
set(KRITA_STABLE_VERSION_MINOR 4)
endif()

# Bugfix release version, or 0 for before the first stable release
set(KRITA_VERSION_RELEASE 0)
# the 4th digit, really only used for the Windows installer:
# - [Pre-]Alpha: Starts from 0, increment 1 per release
# - Beta: Starts from 50, increment 1 per release
# - Stable: Set to 100, bump to 101 if emergency update is needed
set(KRITA_VERSION_REVISION 0)

# In MSI packages we cannot use the fourth digit for release numbering,
# since this digit is used by Microsoft Store internally. Hence we cannot
# release hotfix releases (like 6.0.1.1, 6.0.1.2) on Windows. Therefore we
# use the same approach as on Android, we just number all the official
# releases in the same X.Y.* branch sequentially. We use a simple formula
# for that, e.g. 5.4.2.3 will be released as 5.4.10203.0.
math(EXPR KRITA_MSI_VERSION_RELEASE_TRANSITIONAL "10000 + 100 * ${KRITA_VERSION_RELEASE} + ${KRITA_VERSION_REVISION}")

# Uncomment the following if this is currently in the "stable" branch.
# Do not uncomment for master branch.
#set(KRITA_STABLE_BRANCH 1)
set(KRITA_ALPHA 1) # uncomment only for Alpha
#set(KRITA_BETA 1) # uncomment only for Beta
#set(KRITA_RC 1) # uncomment only for RC

if(NOT DEFINED KRITA_ALPHA AND NOT DEFINED KRITA_BETA AND NOT DEFINED KRITA_RC)
    set(KRITA_STABLE 1) # do not edit
endif()

message(STATUS "Krita version: ${KRITA_VERSION_STRING}")

# Define the generic version of the Krita libraries here
# This makes it easy to advance it when the next Krita release comes.
#
# In general Krita does **not** guarantee any binary compatibility over C++
# libraries between the releases, so there is not reason to update SOVERSION
# too often.
#
# The only reason why we use SOVERSION is to allow multiple **major** versions
# of Krita to coexist in the same installation, e.g. Krita 5 and Krita 6.
#
# We don't support simultaneous installations of, say, Krita 5.1 and Krita 5.2
# (one should use AppImage for things like that)
#
# Historical note:
# 
# We had a mess with versions previously, we used to increase the versions with
# every minor release only. It caused multiple problems: 1) the soversions of 
# different major releases did overlap; 2) the soversions of different bugfix
# releases were the same, even tough they were not binary compatible.
#
# In Krita 6 we just stop updating SOVERSION on minor releases and declare that
# we have no binary compatibility between releases.
#
# Krita 5.0: SOVERSION -> 17
# Krita 5.1: SOVERSION -> 18
# Krita 5.2: SOVERSION -> 19
# Krita 5.3: SOVERSION -> 20
#
# Krita 6.x: SOVERSION -> 21
# Krita 7.x: SOVERSION -> 22
#
if(KRITA_STABLE_VERSION_MAJOR EQUAL 5) 
    # In Krita 5 days we increased the SOVERSION with every minor release
    if(KRITA_STABLE_VERSION_MINOR LESS_EQUAL 3)
        math(EXPR GENERIC_KRITA_LIB_VERSION_MAJOR "${KRITA_STABLE_VERSION_MINOR} + 17")
    else()
        set(GENERIC_KRITA_LIB_VERSION_MAJOR 20)
    endif()
elseif(KRITA_STABLE_VERSION_MAJOR GREATER_EQUAL 6)
    # In Krita 6 days we increased the SOVERSION only with every **major** release.
    # We don't guarantee any ABI compatibility between the releases, so there is no
    # use increasing SOVERSION too often
    math(EXPR GENERIC_KRITA_LIB_VERSION_MAJOR "${KRITA_STABLE_VERSION_MAJOR} - 6 + 21")
endif()

set(GENERIC_KRITA_LIB_VERSION "${GENERIC_KRITA_LIB_VERSION_MAJOR}.0.0")
set(GENERIC_KRITA_LIB_SOVERSION "${GENERIC_KRITA_LIB_VERSION_MAJOR}")

LIST (APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")
LIST (APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/kde_macro")

# fetch git revision for the current build

set(KRITA_GIT_SHA1_STRING "")
set(KRITA_GIT_BRANCH_STRING "")

include(GetGitRevisionDescription)
get_git_head_hash(GIT_SHA1)
get_git_branch(GIT_BRANCH)

if(GIT_SHA1)
   string(SUBSTRING ${GIT_SHA1} 0 7 GIT_SHA1)
   set(KRITA_GIT_SHA1_STRING ${GIT_SHA1} CACHE STRING "Git commit of the current build" FORCE)
   if(NOT GIT_BRANCH)
       set(GIT_BRANCH "(detached HEAD)")
   endif()
   set(KRITA_GIT_BRANCH_STRING ${GIT_BRANCH} CACHE STRING "Git branch of the current build" FORCE)
endif()

# create test make targets
enable_testing()
# collect list of broken tests, empty here to start fresh with each cmake run
set(KRITA_BROKEN_TESTS "" CACHE INTERNAL "KRITA_BROKEN_TESTS")
# Keep track of all test target (need special attention on macos)
set(KRITA_TESTS_TARGET "" CACHE INTERNAL "KRITA_TESTS_TARGET")

 ############
#############
## Options ##
#############
############

include(FeatureSummary)

if (WIN32)
option(USE_DRMINGW "Support the Dr. Mingw crash handler (only on windows)" ON)
add_feature_info("Dr. Mingw" USE_DRMINGW "Enable the Dr. Mingw crash handler")
    if (MINGW)
        option(USE_MINGW_HARDENING_LINKER "Enable DEP (NX), ASLR and high-entropy ASLR linker flags (mingw-w64)" ON)
        add_feature_info("Linker Security Flags" USE_MINGW_HARDENING_LINKER "Enable DEP (NX), ASLR and high-entropy ASLR linker flags")
        if (USE_MINGW_HARDENING_LINKER)
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--dynamicbase -Wl,--nxcompat -Wl,--disable-auto-image-base")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--dynamicbase -Wl,--nxcompat -Wl,--disable-auto-image-base")
            set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--dynamicbase -Wl,--nxcompat -Wl,--disable-auto-image-base")
            # Enable high-entropy ASLR for 64-bit
            # The image base has to be >4GB for HEASLR to be enabled.
            # The values used here are kind of arbitrary.
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--high-entropy-va -Wl,--image-base,0x140000000")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--high-entropy-va -Wl,--image-base,0x180000000")
            set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--high-entropy-va -Wl,--image-base,0x180000000")
        else (USE_MINGW_HARDENING_LINKER)
            message(WARNING "Linker Security Flags not enabled!")
        endif (USE_MINGW_HARDENING_LINKER)

        # Clang does not generate DWARF arranges data by default, which makes
        # DrMingw not able to parse the DWARF debug symbols. Add -gdwarf-aranges
        # explicitly.
        # See: https://github.com/jrfonseca/drmingw/issues/42#issuecomment-516614561
        if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
            set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -gdwarf-aranges")
        endif ()
        if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -gdwarf-aranges")
        endif ()
    elseif (MSVC)
        # Increase the stack size to match MinGW's. Prevents crashes with GMic.
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:4194304")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /STACK:4194304")
        set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /STACK:4194304")
        option(USE_CONTROL_FLOW_GUARD "Enable Control Flow Guard hardening (MSVC)" ON)
        add_feature_info("Linker Security Flags" USE_CONTROL_FLOW_GUARD "Enable Control Flow Guard")
        if (USE_CONTROL_FLOW_GUARD)
            add_compile_options(/guard:cf)
            add_link_options(/GUARD:CF)
        endif (USE_CONTROL_FLOW_GUARD)
    endif (MINGW)
elseif(ANDROID)
    # Increase the stack size to match MinGW's. Prevents crashes with GMic.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,stack-size=4194304")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,stack-size=4194304")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,stack-size=4194304")

    if (${CMAKE_ANDROID_NDK_VERSION} VERSION_GREATER_EQUAL "27.0.0"
            AND (CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" OR CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64"))
        message(STATUS "Enable 16K page alignment for Android NDK 27")
        set(PAGE_ALIGN_16K_LINKER_FLAGS -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
        add_link_options(${PAGE_ALIGN_16K_LINKER_FLAGS})
    endif()
endif ()

option(HIDE_SAFE_ASSERTS "Don't show message box for \"safe\" asserts, just ignore them automatically and dump a message to the terminal." ON)
add_feature_info("Hide safe asserts" HIDE_SAFE_ASSERTS "Don't show message box for \"safe\" asserts, just ignore them automatically and dump a message to the terminal.")

option(INSTALL_BENCHMARKS "Install benchmarks into the installation root to make them packagable" OFF)
add_feature_info("Install benchmarks" INSTALL_BENCHMARKS "Install benchmarks into the installation root to make them packagable")

option(CRASH_ON_SAFE_ASSERTS "Crash unconditionally whenever a \"safe\" assert happens. Useful for running unittests" OFF)
add_feature_info("Crash on safe asserts" CRASH_ON_SAFE_ASSERTS "Crash unconditionally whenever a \"safe\" assert happens. Useful for running unittests")

configure_file(config-safe-asserts.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-safe-asserts.h)

option(USE_LOCK_FREE_HASH_TABLE "Use lock free hash table instead of blocking." ON)
configure_file(config-hash-table-implementation.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-hash-table-implementation.h)
add_feature_info("Lock free hash table" USE_LOCK_FREE_HASH_TABLE "Use lock free hash table instead of blocking.")

option(FOUNDATION_BUILD "A Foundation build is a binary release build that can package some extra things like color themes. Linux distributions that build and install Krita into a default system location should not define this option to true." OFF)
add_feature_info("Foundation Build" FOUNDATION_BUILD "A Foundation build is a binary release build that can package some extra things like color themes. Linux distributions that build and install Krita into a default system location should not define this option to true.")

option(KRITA_ENABLE_BROKEN_TESTS "Enable tests that are marked as broken" OFF)
add_feature_info("Enable Broken Tests" KRITA_ENABLE_BROKEN_TESTS "Runs broken test when \"make test\" is invoked (use -DKRITA_ENABLE_BROKEN_TESTS=ON to enable).")

option(LIMIT_LONG_TESTS "Run long running unittests in a limited quick mode" ON)
configure_file(config-limit-long-tests.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-limit-long-tests.h)
add_feature_info("Limit long tests" LIMIT_LONG_TESTS "Run long running unittests in a limited quick mode")

option(BUILD_KRITA_QT_DESIGNER_PLUGINS "Build Qt Designer plugins for Krita widgets" OFF)
add_feature_info("Build Qt Designer plugins" BUILD_KRITA_QT_DESIGNER_PLUGINS "Builds Qt Designer plugins for Krita widgets (use -DBUILD_KRITA_QT_DESIGNER_PLUGINS=ON to enable).")

option(ENABLE_UPDATERS "Enable updaters/update notifications" ON)
configure_file(config-updaters.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-updaters.h)
add_feature_info("Enable updaters" ENABLE_UPDATERS "Enable updaters/update notifications.")

option(KRITA_ENABLE_PCH "Enable precompiled headers support" OFF)
add_feature_info("Precompiled Headers" KRITA_ENABLE_PCH "precompiled headers make build process faster on some systems")

option(AI_ENABLE_UNITY_BUILD "Enable CMake Unity Build for fast compilation" OFF)
if (AI_ENABLE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
    if (NOT DEFINED CMAKE_UNITY_BUILD_BATCH_SIZE)
        set(CMAKE_UNITY_BUILD_BATCH_SIZE 8)
    endif()
    add_feature_info("Unity Build" AI_ENABLE_UNITY_BUILD "Unity Build batches C++ translation units for 50-70% faster builds")
endif()

option(USE_EXTERNAL_RAQM "Fetch Raqm dependency online instead of the embedded one" OFF)

if(ANDROID)
option(ANDROID_ENABLE_STDIO_FORWARDING "Enable forwarding of qDebug() and stdio messages to logcat" OFF)
configure_file(config-android-stdio-forwarding.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-android-stdio-forwarding.h)
add_feature_info("Enable debug logging on Android" ANDROID_ENABLE_STDIO_FORWARDING "Enable forwarding of qDebug() and stdio messages to logcat")
endif()

set(ADDRESS_SANITIZER_ENABLED FALSE)
if (ECM_ENABLE_SANITIZERS MATCHES address)
    set(ADDRESS_SANITIZER_ENABLED TRUE)
endif()

add_feature_info("ASAN address sanitizer" ADDRESS_SANITIZER_ENABLED "crash Krita if it violates address access rules (-DECM_ENABLE_SANITIZERS=address)")

# We don't want people to accidentally build unstable Krita versions if they
# didn't mean to do so. This serves as an extra confirmation. You will get
# errors during the configure step if you enable an unstable option.
set(ALLOW_UNSTABLE "" CACHE STRING
    "Semicolon-separated set of features that make Krita unstable, but you want to build with them anyway")
string(TOUPPER "${ALLOW_UNSTABLE}" ALLOW_UNSTABLE)
add_feature_info("Unstable features allowed" ON "${ALLOW_UNSTABLE}")

if(BUILD_WITH_QT6 AND NOT "QT6" IN_LIST ALLOW_UNSTABLE)
    message(FATAL_ERROR
        "Building Krita with Qt6 is not yet production-ready. Please don't "
        "package this and ship it as a stable version of the software.\n"
        "If you want to continue regardless, pass:\n"
        "    -DALLOW_UNSTABLE=QT6"
    )
endif()

# Branding. Available options: default, Beta, Plus, Next. Can be set from command line
if ("${BRANDING}" STREQUAL "")
    if (DEFINED KRITA_STABLE)
        set(BRANDING "default")
    elseif (DEFINED KRITA_BETA OR DEFINED KRITA_RC)
        set(BRANDING "Beta")
    elseif (DEFINED KRITA_STABLE_BRANCH)
        # Alpha/pre-alpha in stable branch
        set(BRANDING "Plus")
    else ()
        # Alpha/pre-alpha in master branch or other experiments
        set(BRANDING "Next")
    endif ()
endif()

# Qt6 isn't stable, don't make it look like it is.
if(BRANDING STREQUAL "default" AND BUILD_WITH_QT6)
    set(BRANDING "Beta")
endif()

message(STATUS "Branding selected: ${BRANDING}")
message(STATUS "Building the AI Stroke Painter application")

include(MacroJPEG)

 ########################
#########################
## Look for KDE and Qt ##
#########################
########################

if (BUILD_WITH_QT6)
    set(QT_MAJOR_VERSION 6)
    find_package(ECM 6.0 REQUIRED CONFIG)
else()
    set(QT_MAJOR_VERSION 5)
    find_package(ECM 5.91 REQUIRED CONFIG)
endif()

# KDE Frameworks
set(KF_MAJOR ${QT_MAJOR_VERSION})

if (KF_MAJOR STREQUAL "6")
    set(KF_DEP_VERSION "6.0.0")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH})

include(ECMOptionalAddSubdirectory)
include(ECMAddAppIcon)
include(ECMSetupVersion)
include(ECMMarkNonGuiExecutable)
include(ECMGenerateHeaders)
include(GenerateExportHeader)
include(ECMMarkAsTest)
include(ECMInstallIcons)

include(CMakePackageConfigHelpers)
include(WriteBasicConfigVersionFile)
include(CheckFunctionExists)
include(CheckCXXSymbolExists)

set (KDE_COMPILERSETTINGS_LEVEL "5.0")
set (KDE_QT_MODERNCODE_DEFINITIONS_LEVEL "5.0")
set (KDE_SKIP_PEDANTIC_WARNINGS_SETTINGS TRUE)
set (KDE_SKIP_MISSING_INCLUDE_DIRS_WARNINGS_SETTINGS TRUE)
set (KDE_SKIP_NULLPTR_WARNINGS_SETTINGS TRUE)

include(KDECompilerSettings)

include(KDEInstallDirs)

include(KDECMakeSettings)

if (WIN32)

# Qt6 targets Windows10, so we shouldn't change it in any way
if (QT_MAJOR_VERSION STREQUAL "5")
    # KDECompilerSettings sets Windows Vista as the default,
    # while MSVC's default is 0x0A00 (_WIN32_WINNT_WIN10, sdkddkver.h) and
    # MinGW's is 0x0601 (_WIN32_WINNT_WIN7, _mingw.h).
    # Both are enough to supply the APIs we need in main.cc, but since we
    # need the Windows 8 APIs anyway for the surface orientation and Store API,
    # we set the minimum here.
    remove_definitions(-D_WIN32_WINNT=0x0600 -DWINVER=0x0600 -D_WIN32_IE=0x0600)
    add_definitions(-D_WIN32_WINNT=0x0602 -DWINVER=0x0602  -D_WIN32_IE=0x0602)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    if (MSVC) # or CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
        # KDE's CompilerSettings module sets -Wall -Wextra for Clang.
        # However, -Wall on clang-cl maps to -Weverything, which turns on way too
        # much, so we're using -W4 instead, which is mapped to clang's -Wall -Wextra.
        # Source: https://hg.mozilla.org/mozilla-central/rev/ffb7bfbfc328
        string(REPLACE "-Wall -Wextra" "-W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(REPLACE "-Wall -Wextra" "-W4" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
        # Allow KoColorSpaceMaths operators to build.
        add_compile_options("/permissive")
    endif()

    # Remove these invalid flags.
    string(REPLACE "-fno-common" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REPLACE "-fno-operator-names" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "-fdiagnostics-color=always" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "-Wl,--no-undefined" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
    string(REPLACE "-Wl,--no-undefined" "" CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")
    string(REPLACE "-Wl,--fatal-warnings" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
    string(REPLACE "-Wl,--fatal-warnings" "" CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")
endif()
endif()

# do not reorder to be alphabetical: this is the order in which the frameworks
# depend on each other.
find_package(KF${KF_MAJOR} ${MIN_FRAMEWORKS_VERSION} REQUIRED COMPONENTS
        Config
        WidgetsAddons
        Completion
        CoreAddons
        GuiAddons
        I18n
        ItemViews
)

find_package(Qt${QT_MAJOR_VERSION} ${MIN_QT_VERSION}
        REQUIRED COMPONENTS
        Core
        Gui
        Widgets
        Xml
        Network
        PrintSupport
        Svg
        Test
        Concurrent
        Sql
)

if (ANDROID)
    if (QT_MAJOR_VERSION STREQUAL "5")
        find_package(Qt${QT_MAJOR_VERSION} ${MIN_QT_VERSION}
            REQUIRED COMPONENTS
            AndroidExtras
        )
    endif()
endif()

if (QT_MAJOR_VERSION STREQUAL "6")
    find_package(Qt${QT_MAJOR_VERSION} ${MIN_QT_VERSION}
        REQUIRED COMPONENTS
        Core5Compat
        OpenGL
        OpenGLWidgets
        SvgWidgets
    )

    find_package(Qt${QT_MAJOR_VERSION} ${MIN_QT_VERSION}
        OPTIONAL_COMPONENTS
        WaylandClient
    )

    find_package(KF6 ${MIN_FRAMEWORKS_VERSION}
        REQUIRED COMPONENTS
        ColorScheme
    )

    # Qt 6.10 has split this into it's own CMake module, it's no longer included with Gui
    if (Qt6Gui_VERSION VERSION_GREATER_EQUAL "6.10.0")
        find_package(Qt6GuiPrivate ${REQUIRED_QT_VERSION} REQUIRED NO_MODULE)
        get_target_property(Qt6Gui_PRIVATE_INCLUDE_DIRS Qt6::GuiPrivate INTERFACE_INCLUDE_DIRECTORIES)

        # QtGui depends on private headers form QtCore as well
        find_package(Qt6CorePrivate ${REQUIRED_QT_VERSION} REQUIRED NO_MODULE)
        get_target_property(Qt6Core_PRIVATE_INCLUDE_DIRS Qt6::CorePrivate INTERFACE_INCLUDE_DIRECTORIES)

        # Currently we depend on the Qt6Gui_PRIVATE_INCLUDE_DIRS variable to selectively include
        # the needed headers, but it will fail to compile in 6.10 since the private headers exist
        # in separate directories. So we'll just shove them into the variable we expect for now.
        list(APPEND Qt6Gui_PRIVATE_INCLUDE_DIRS ${Qt6Core_PRIVATE_INCLUDE_DIRS})
    endif()
endif()

set(HAVE_WAYLAND FALSE)
if (TARGET Qt::WaylandClient)
    message(STATUS "Found Qt::WaylandClient, enabling kritawayland platform")
    set(HAVE_WAYLAND TRUE)

    # Qt 6.10 has split the private module, see above for a better explanation
    if (Qt6Gui_VERSION VERSION_GREATER_EQUAL "6.10.0")
        find_package(Qt6WaylandClientPrivate ${REQUIRED_QT_VERSION} REQUIRED NO_MODULE)
        get_target_property(Qt6WaylandClient_PRIVATE_INCLUDE_DIRS Qt6::WaylandClientPrivate INTERFACE_INCLUDE_DIRECTORIES)
    endif()
endif()

option(KRITA_USE_SURFACE_COLOR_MANAGEMENT_API "Use per-surface color management API (e.g. when using Wayland)" ${HAVE_WAYLAND})
add_feature_info("Enable per-surface color management API" KRITA_USE_SURFACE_COLOR_MANAGEMENT_API "adds support for an independent color space management for the canvas")

if (NOT HAVE_WAYLAND AND KRITA_USE_SURFACE_COLOR_MANAGEMENT_API)
    message(WARNING "KRITA_USE_SURFACE_COLOR_MANAGEMENT_API was set when there is no Wayland support, surface management will be useless as no plugins are built")
endif()

configure_file(config-use-surface-color-management-api.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-use-surface-color-management-api.h)

#
# Check for the presence of custom patches in Qt
#
set(CMAKE_REQUIRED_INCLUDES ${Qt${QT_MAJOR_VERSION}Gui_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${Qt${QT_MAJOR_VERSION}Gui_LIBRARIES})

check_cxx_symbol_exists(QT_HAS_ENTER_LEAVE_PATCH "QtGui/qevent.h" KRITA_QT_HAS_ENTER_LEAVE_PATCH)

if (QT_MAJOR_VERSION STREQUAL "5")
    # in Qt5 we didn't have a separate definition for key-press/release patch
    set(KRITA_QT_HAS_UNBALANCED_KEY_PRESS_RELEASE_PATCH ${KRITA_QT_HAS_ENTER_LEAVE_PATCH})
else()
    check_cxx_symbol_exists(QT_HAS_UNBALANCED_KEY_PRESS_RELEASE_PATCH "QtGui/qevent.h" KRITA_QT_HAS_UNBALANCED_KEY_PRESS_RELEASE_PATCH)
endif()

unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)


if (QT_MAJOR_VERSION STREQUAL "5")
    # in Qt5 we didn't have a separate definition for key-press/release patch
    set(KRITA_QT_HAS_UPDATE_COMPRESSION_PATCH FALSE)
else()
    set(CMAKE_REQUIRED_INCLUDES ${Qt6Gui_INCLUDE_DIRS} ${Qt6Gui_PRIVATE_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${Qt6Gui_LIBRARIES})

    CHECK_CXX_SOURCE_COMPILES("
#include <QtGui/rhi/qrhi.h>
int main(int, char *[]) {
bool (QRhi::*x)() const;
x = &QRhi::isLastFrameCompletedOnGPU;
Q_UNUSED(x);
return 0;
}
"
    KRITA_QT_HAS_UPDATE_COMPRESSION_PATCH
    )

    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)

endif()

option(KRITA_QT_HAS_ENTER_LEAVE_PATCH "Qt has a custom patch for unbalanced enter/leave events" ON)
add_feature_info("Qt: Unbalanced Enter/Leave patch" KRITA_QT_HAS_ENTER_LEAVE_PATCH "Krita will disable internal workarounds for unbalanced Enter/Leave")

option(KRITA_QT_HAS_UNBALANCED_KEY_PRESS_RELEASE_PATCH "Qt has a custom patch for unbalanced KeyPress/KeyRelease events in non-latin layouts" ON)
add_feature_info("Qt: Unbalanced KeyPress/KeyRelease in non-latin layout patch" KRITA_QT_HAS_UNBALANCED_KEY_PRESS_RELEASE_PATCH "Krita will disable internal workarounds for unbalanced KeyPress/KeyRelease events")

option(KRITA_QT_HAS_UPDATE_COMPRESSION_PATCH "Qt has a custom patch for limiting FPS to the screen framerate" ON)
add_feature_info("Qt: Compressed updates patch" KRITA_QT_HAS_UPDATE_COMPRESSION_PATCH "Krita will compress the screen updates to the maximum FPS of available displays")


#
# Check for the presence the Wintab switch in Qt5
#
if (WIN32 AND QT_MAJOR_VERSION STREQUAL "5")
    set(CMAKE_REQUIRED_INCLUDES ${Qt5Core_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${Qt5Core_LIBRARIES})

    CHECK_CXX_SOURCE_COMPILES("
#include <QCoreApplication>
int main(int argc, char *argv[]) {
QCoreApplication::setAttribute(Qt::AA_MSWindowsUseWinTabAPI);
}
"
    QT5_HAS_WINTAB_SWITCH
    )

    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)

#QT5_HAS_WINTAB_SWITCH

    configure_file(config_qt5_has_wintab_switch.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config_qt5_has_wintab_switch.h)
endif ()



if (WIN32)
    if (QT_MAJOR_VERSION STREQUAL "5")
        set(CMAKE_REQUIRED_INCLUDES ${Qt5Core_INCLUDE_DIRS} ${Qt5Gui_INCLUDE_DIRS} ${Qt5Widgets_INCLUDE_DIRS})
        set(CMAKE_REQUIRED_LIBRARIES ${Qt5Core_LIBRARIES} ${Qt5Gui_LIBRARIES} ${Qt5Widgets_LIBRARIES})

        CHECK_CXX_SOURCE_COMPILES("
            #include <QSurfaceFormat>
            int main(int argc, char *argv[]) {
                QSurfaceFormat fmt;
                fmt.setColorSpace(QSurfaceFormat::scRGBColorSpace);
                fmt.setColorSpace(QSurfaceFormat::bt2020PQColorSpace);
            }
            "
            HAVE_HDR
        )

        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)

    else()

        set(CMAKE_REQUIRED_INCLUDES ${Qt6Core_INCLUDE_DIRS} ${Qt6Gui_INCLUDE_DIRS} ${Qt6Widgets_INCLUDE_DIRS} ${Qt6OpenGLWidgets_INCLUDE_DIRS})
        set(CMAKE_REQUIRED_LIBRARIES ${Qt6Core_LIBRARIES} ${Qt6Gui_LIBRARIES} ${Qt6Widgets_LIBRARIES} ${Qt6OpenGLWidgets_LIBRARIES})

        CHECK_CXX_SOURCE_COMPILES("
            #include <QOpenGLWidget>
            #include <QColorSpace>
            int main(int argc, char *argv[]) {
                QOpenGLWidget *w = nullptr;
                w->setTextureColorSpace(QColorSpace());
            }
            "
            HAVE_HDR
        )

        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)

    endif()
endif()
configure_file(config-hdr.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-hdr.h)

set(CMAKE_REQUIRED_INCLUDES ${Qt5Core_INCLUDE_DIRS} ${Qt5Gui_INCLUDE_DIRS} ${Qt5Widgets_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${Qt5Core_LIBRARIES} ${Qt5Gui_LIBRARIES} ${Qt5Widgets_LIBRARIES})

CHECK_CXX_SOURCE_COMPILES("
#include <QGuiApplication>
int main(int argc, char *argv[]) {
QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);
QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
}
"
HAVE_HIGH_DPI_SCALE_FACTOR_ROUNDING_POLICY
)
configure_file(config-high-dpi-scale-factor-rounding-policy.h.in ${CMAKE_CURRENT_BINARY_DIR}/config-high-dpi-scale-factor-rounding-policy.h)

CHECK_CXX_SOURCE_COMPILES("
#include <QMdiArea>
int main(int argc, char *argv[]) {
QMdiArea area;
area.setOption(QMdiArea::AlwaysShowSubwindowNameInTitleBar);
}
"
HAVE_QMDIAREA_ALWAYS_SHOW_SUBWINDOW_TITLE
)
configure_file(config-qmdiarea-always-show-subwindow-title.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-qmdiarea-always-show-subwindow-title.h)

if (WIN32)
    CHECK_CXX_SOURCE_COMPILES("
#include <QtPlatformHeaders/QWindowsWindowFunctions>
int main(int argc, char *argv[]) {
QWindowsWindowFunctions::setHasBorderInFullScreenDefault(true);
}
"
    HAVE_SET_HAS_BORDER_IN_FULL_SCREEN_DEFAULT
    )
    configure_file(config-set-has-border-in-full-screen-default.h.in ${CMAKE_CURRENT_BINARY_DIR}/config-set-has-border-in-full-screen-default.h)

endif (WIN32)


CHECK_CXX_SOURCE_COMPILES("
#include <QCoreApplication>
int main(int argc, char *argv[]) {
QCoreApplication::setKritaAttribute(KRITA_QATTRIBUTE_ANDROID_EMULATE_MOUSE_BUTTONS_FOR_PAGE_UP_DOWN);
}
"
KRITA_QT_HAS_ANDROID_EMULATE_MOUSE_BUTTONS_FOR_PAGE_UP_DOWN
)

CHECK_CXX_SOURCE_COMPILES("
#include <QCoreApplication>
int main(int argc, char *argv[]) {
QCoreApplication::setKritaAttribute(KRITA_QATTRIBUTE_ANDROID_IGNORE_HISTORIC_TABLET_EVENTS);
}
"
KRITA_QT_HAS_ANDROID_IGNORE_HISTORIC_TABLET_EVENTS
)

CHECK_CXX_SOURCE_COMPILES("
#include <QCoreApplication>
int main(int argc, char *argv[]) {
QCoreApplication::setKritaAttribute(KRITA_QATTRIBUTE_ANDROID_EMULATE_MOUSE_BUTTONS_FOR_HIGH_FUNCTION_KEYS);
}
"
KRITA_QT_HAS_ANDROID_EMULATE_MOUSE_BUTTONS_FOR_HIGH_FUNCTION_KEYS
)

CHECK_CXX_SOURCE_COMPILES("
#include <QScroller>
#ifndef KRITA_QSCROLLER_PATCH
#error \"QScroller filter patch not present\"
#endif
int main(int argc, char *argv[]) {}
"
KRITA_QT_HAS_QSCROLLER_FILTER
)

CHECK_CXX_SOURCE_COMPILES("
#include <QCoreApplication>
int main(int argc, char *argv[]) {
return Qt::ANDROID_INPUT_PLATFORM_DATA_SOFT_INPUT_ADJUST_NOTHING;
}
"
KRITA_QT_HAS_ANDROID_INPUT_PLATFORM_DATA_SOFT_INPUT_ADJUST_NOTHING
)


set(CMAKE_REQUIRED_INCLUDES ${Qt${QT_MAJOR_VERSION}Gui_INCLUDE_DIRS} ${Qt${QT_MAJOR_VERSION}Gui_PRIVATE_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${Qt${QT_MAJOR_VERSION}Gui_LIBRARIES})

CHECK_CXX_SOURCE_COMPILES("
#include <qpa/qplatformscreen.h>
int main(int argc, char *argv[]) {
QPlatformScreen *screen = nullptr;
screen->setDensityAdjustment(1.0);
return screen->densityAdjustment();
}
"
KRITA_QT_HAS_ANDROID_QPLATFORMSCREEN_DENSITY_ADJUSTMENT
)

configure_file(config-qt-patches-present.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-qt-patches-present.h)

unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)


include (MacroAddFileDependencies)
include (MacroBoolTo01)
include (MacroEnsureOutOfSourceBuild)
macro_ensure_out_of_source_build("Compiling Krita inside the source directory is not possible. Please refer to the build instruction https://community.kde.org/Krita#Build_Instructions")


# Note: OPTIONAL_COMPONENTS does not seem to be reliable
# (as of ECM 5.15.0, CMake 3.2)

find_package(Qt${QT_MAJOR_VERSION}Quick ${MIN_QT_VERSION})
set_package_properties(Qt${QT_MAJOR_VERSION}Quick PROPERTIES
        DESCRIPTION "QtQuick"
        URL "https://www.qt.io/"
        TYPE OPTIONAL
        PURPOSE "Optionally used for the touch gui for Krita")

macro_bool_to_01(Qt${QT_MAJOR_VERSION}Quick_FOUND HAVE_QT_QUICK)

find_package(Qt${QT_MAJOR_VERSION}QuickWidgets ${MIN_QT_VERSION})
set_package_properties(Qt${QT_MAJOR_VERSION}QuickWidgets PROPERTIES
        DESCRIPTION "QtQuickWidgets"
        URL "https://www.qt.io/"
        TYPE OPTIONAL
        PURPOSE "Optionally used for the touch gui for Krita")

if (Qt${QT_MAJOR_VERSION}QuickWidgets_FOUND)
    # This is needed because Qt dependencies aren't added by ECM but by androideployqt,
    # so it doesn't benefit from our patch in ECM.
    list (APPEND ANDROID_EXTRA_LIBS $<TARGET_FILE:Qt${QT_MAJOR_VERSION}::QuickWidgets>)

    find_package(Qt${QT_MAJOR_VERSION}QuickControls2 ${MIN_QT_VERSION})
    set_package_properties(Qt${QT_MAJOR_VERSION}QuickControls2 PROPERTIES
            DESCRIPTION "QtQuickControls"
            URL "https://www.qt.io/"
            TYPE OPTIONAL
            PURPOSE "Optionally used for the touch gui for Krita")

endif()


if (NOT WIN32 AND NOT APPLE AND NOT ANDROID AND NOT HAIKU)
    if (QT_MAJOR_VERSION STREQUAL "5")
        find_package(Qt${QT_MAJOR_VERSION} ${MIN_QT_VERSION} REQUIRED X11Extras)
    endif()

    find_package(Qt${QT_MAJOR_VERSION}DBus ${MIN_QT_VERSION})
    set_package_properties(Qt${QT_MAJOR_VERSION}DBus PROPERTIES
        DESCRIPTION "Qt DBUS integration"
        URL "https://www.qt.io/"
        TYPE OPTIONAL
        PURPOSE "Optionally used to provide a dbus api on Linux")
    macro_bool_to_01(Qt${QT_MAJOR_VERSION}DBus_FOUND HAVE_DBUS)

    find_package(KF${KF_MAJOR}Crash ${MIN_FRAMEWORKS_VERSION})
    macro_bool_to_01(KF${KF_MAJOR}Crash_FOUND HAVE_KCRASH)
    set_package_properties(KF${KF_MAJOR}Crash PROPERTIES
        DESCRIPTION "KDE's Crash Handler"
        URL "https://api.kde.org/kcrash-index.html"
        TYPE OPTIONAL
        PURPOSE "Optionally used to provide crash reporting on Linux")

    find_package(X11 REQUIRED COMPONENTS Xinput)
    set(HAVE_X11 TRUE)
    add_definitions(-DHAVE_X11)

else()
    set(HAVE_DBUS FALSE)
    set(HAVE_X11 FALSE)
endif()


add_definitions(
  -DQT_USE_QSTRINGBUILDER
  -DQT_NO_SIGNALS_SLOTS_KEYWORDS
  -DQT_NO_URL_CAST_FROM_STRING
  -DQT_USE_FAST_CONCATENATION 
  -DQT_USE_FAST_OPERATOR_PLUS
)

# MSVC is unable to disambiguate between definitions of QVector<QPointF> 
# and QPolygonF. This is a known upstream bug e.g.:
# - https://phabricator.kde.org/D21314
# - https://codereview.qt-project.org/c/qt/qtbase/+/180229
# Starting with Qt 5.13, it is impossible to use strict iterators
# wholesale because of:
# https://github.com/qt/qtbase/commit/972f8845a85d6a07140025e4257cb8a1a2699b5d
if (NOT (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
  add_definitions(-DQT_STRICT_ITERATORS)
endif()

add_compile_definitions(
    QT_DISABLE_DEPRECATED_BEFORE=0x050E00
)

add_definitions(-DTRANSLATION_DOMAIN=\"ai-stroke-painter\")

#
# The reason for this mode is that the Debug mode disable inlining
#
if(CMAKE_COMPILER_IS_GNUCXX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -fext-numeric-literals")
endif()

option(KRITA_DEVS "For Krita developers. This modifies the DEBUG build type to use -O3 -g, while still enabling Q_ASSERT. This is necessary because the Qt5 cmake modules normally append QT_NO_DEBUG to any build type that is not labeled Debug")
if (KRITA_DEVS)
    set(CMAKE_CXX_FLAGS_DEBUG "-O3 -g" CACHE STRING "" FORCE)
endif()

if(UNIX)
    set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES};m")
endif()

if(WIN32)
    if(MSVC)
        # C4522: 'class' : multiple assignment operators specified
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -wd4522 -wd4138 /Zc:__cplusplus")
        # libs\ui\dialogs\kis_about_application.cpp : fatal error C1128: number of sections exceeded object file format limit: compile with /bigobj
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj")

        # Enable intrinsics
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Oi")
        # Favor fast code
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Ot")
        # Function-level linking
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Gy")

        # Our code is UTF-8 encoded.
        add_compile_options(/utf-8)
    elseif(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Krita's template-heavy canvas code exceeds the default COFF section
        # limit with GNU MinGW. Use the extended object format for a reliable
        # standalone Windows build.
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wa,-mbig-obj>)
    endif()
endif()

if (MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang") # Clang/CL is incompatible with this flag
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /RELEASE")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /RELEASE")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /RELEASE")
endif()

# Force enable a good many optimizations
if (MSVC AND NOT BUILD_TESTING)
    # Aggressive inlining (Release)
    string(REPLACE "Ob2" "Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    # Aggressive inlining (RelWithDebInfo)
    string(REPLACE "Ob1" "Ob3" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    # Whole program optimization
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GL")
    # For all objects:
    # - strip unused functions
    # - fold identical functions
    # - link-time code generation
    string(REPLACE "INCREMENTAL" "INCREMENTAL:NO" CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "INCREMENTAL" "INCREMENTAL:NO" CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "INCREMENTAL" "INCREMENTAL:NO" CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO}")
    set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO} /OPT:ICF,REF /LTCG")
    set(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO} /OPT:ICF,REF /LTCG")
    set(CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO} /OPT:ICF,REF /LTCG")
endif()

# KDECompilerSettings adds the `--export-all-symbols` linker flag.
# We don't really need it.
if(MINGW)
    string(REPLACE "-Wl,--export-all-symbols" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
    string(REPLACE "-Wl,--export-all-symbols" "" CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")
endif(MINGW)

if(MINGW)
    # Hack CMake's variables to tell AR to create thin archives to reduce unnecessary writes.
    # Source of definition: https://github.com/Kitware/CMake/blob/v3.14.1/Modules/Platform/Windows-GNU.cmake#L128
    # Thin archives: https://sourceware.org/binutils/docs/binutils/ar.html#index-thin-archives
    macro(mingw_use_thin_archive lang)
        foreach(rule CREATE_SHARED_MODULE CREATE_SHARED_LIBRARY LINK_EXECUTABLE)
            string(REGEX REPLACE "(<CMAKE_AR> [^ T]+) " "\\1T " CMAKE_${lang}_${rule} "${CMAKE_${lang}_${rule}}")
        endforeach()
    endmacro()
    mingw_use_thin_archive(CXX)
endif(MINGW)

# enable exceptions globally
# WARNING: in MSVC this will NOT catch exceptions thrown through C code
# see: 
# - https://learn.microsoft.com/en-us/cpp/build/reference/eh-exception-handling-model?view=msvc-170#arguments
# - https://invent.kde.org/frameworks/extra-cmake-modules/-/blob/v5.101.0/kde-modules/KDECompilerSettings.cmake#L502-526
kde_enable_exceptions()

function(krita_select_pch_file targetname pch_file_name)
    #TODO: make kritaimage and kritaglobal targets to link to its own PCH

    if (targetname STREQUAL "kritaglobal")
        set(${pch_file_name} kis_qt_only_pch.h PARENT_SCOPE)
    elseif (targetname STREQUAL "kritaimage")
        set(${pch_file_name} kis_global_pch.h PARENT_SCOPE)
    else ()
        set(immediate_deps $<TARGET_PROPERTY:${targetname},LINK_LIBRARIES>)
        set(depends_on_kritaui $<IN_LIST:kritaui,${immediate_deps}>)
        set(depends_on_kritaimage $<IN_LIST:kritaimage,${immediate_deps}>)
        set(depends_on_kritalibkis $<IN_LIST:kritalibkis,${immediate_deps}>)
        set(depends_on_kritalibpaintop $<IN_LIST:kritalibpaintop,${immediate_deps}>)

        set(${pch_file_name} $<IF:$<OR:${depends_on_kritalibpaintop},${depends_on_kritalibkis},${depends_on_kritaui},${depends_on_kritaimage}>,kis_image_pch.h,kis_global_pch.h> PARENT_SCOPE)
    endif()
endfunction()

macro(kis_add_library)
    # the first run strips only options specific to kis_add_library
    # to create arguments for real add_library call
    cmake_parse_arguments(REAL "" PCH_FILE "" ${ARGV} )
    add_library(${REAL_UNPARSED_ARGUMENTS})

    if (KRITA_ENABLE_PCH)
        # the second run strips out all the option supported by the function
        # to let the script to calculate the number of source files in
        # the library
        set(options INTERFACE STATIC SHARED MODULE EXCLUDE_FROM_ALL OBJECT ALIAS)
        set(one_value_keywords PCH_FILE)
        set(multi_value_keywords)
        cmake_parse_arguments(LOCAL "${options}" "${one_value_keywords}" "" ${ARGV} )

        list(LENGTH LOCAL_UNPARSED_ARGUMENTS source_count)
        list(GET REAL_UNPARSED_ARGUMENTS 0 NEW_LIBRARY_TARGET)

        if (NOT LOCAL_PCH_FILE)
            krita_select_pch_file(${NEW_LIBRARY_TARGET} LOCAL_PCH_FILE)
            #file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/out/${NEW_LIBRARY_TARGET}.txt" CONTENT ${LOCAL_PCH_FILE})
        endif()

        # if the number of sources in the library is greater
        # than a threshold value, then we can try to use PCH
        # for this library

        if (${source_count} GREATER "2" AND
            NOT LOCAL_INTERFACE AND
            NOT LOCAL_ALIAS AND
            LOCAL_PCH_FILE)

            set_property(TARGET ${NEW_LIBRARY_TARGET} PROPERTY PCH_WARN_INVALID TRUE )
            target_precompile_headers(${NEW_LIBRARY_TARGET} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/pch/${LOCAL_PCH_FILE}>")
        endif()
    endif()
endmacro()

# overcome some platform incompatibilities
if(WIN32)
    if (MSVC)
        include_directories(${CMAKE_CURRENT_SOURCE_DIR}/winquirks)
        add_definitions(-D_USE_MATH_DEFINES)
        add_definitions(-DNOMINMAX)
    endif()
endif()

# set custom krita plugin installdir
if (ANDROID)
    # use default ABI
    if (NOT DEFINED ANDROID_ABI)
        set (ANDROID_ABI arm64-v8a)
    endif()
    if (NOT DEFINED ANDROID_SDK_ROOT)
        message (FATAL_ERROR "ANDROID_SDK_ROOT argument is not set")
    endif()

    set (KRITA_PLUGIN_INSTALL_DIR ${KDE_INSTALL_LIBDIR})
    # set (KDE_INSTALL_DATADIR ${CMAKE_INSTALL_PREFIX}/assets)

    # From ECM
    # Because Qt changes library suffix (to include arch), this changes name of
    # libraries that depend on Qt, but its Find file may not necessarily be
    # adapted for this.
    set(CMAKE_FIND_LIBRARY_SUFFIXES "_${CMAKE_ANDROID_ARCH_ABI}.so" ".so" ".a")
else()
    set (KRITA_PLUGIN_INSTALL_DIR ${KDE_INSTALL_LIBDIR}/kritaplugins)
endif()


 ###########################
############################
## Required dependencies  ##
############################
###########################

# FIXME: Still hardcoded
if (ANDROID)
    set (Boost_COMPILER clang)
    find_package(unwindstack REQUIRED)
endif()

find_package(PNG REQUIRED "1.6.0")

if (APPLE)
    # this is not added correctly on OSX -- see https://forum.kde.org/viewtopic.php?f=139&t=101867&p=221242#p221242
    include_directories(SYSTEM ${PNG_INCLUDE_DIR})
endif()

if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    string(REGEX REPLACE "([0-9])\\.([0-9])(\\.[0-9])?" "\\1"
        KRITA_boost_COMPILER_VERSION ${CMAKE_CXX_COMPILER_VERSION})
    set(Boost_COMPILER "mgw${KRITA_boost_COMPILER_VERSION}")
endif()

find_package(Boost 1.69 REQUIRED)
set_package_properties(Boost PROPERTIES
    DESCRIPTION "Boost provides free peer-reviewed portable C++ source libraries."
    URL "https://www.boost.org"
    TYPE REQUIRED)
target_link_libraries(Boost::boost
    INTERFACE
        Boost::disable_autolinking
)

# Disallow usage of std::unary_function. 
# See https://github.com/boostorg/container_hash/issues/22
# and https://releases.llvm.org/15.0.0/projects/libcxx/docs/ReleaseNotes.html#deprecations-and-removals
if(Boost_VERSION VERSION_LESS "1.81.0")
target_compile_definitions(Boost::boost
    INTERFACE
        BOOST_NO_CXX98_FUNCTION_BASE
)
endif()

find_package(Immer REQUIRED)
find_package(Zug REQUIRED)
find_package(Lager REQUIRED)

##
## Test for GNU Scientific Library
##
find_package(GSL)
set_package_properties(GSL PROPERTIES
    URL "https://www.gnu.org/software/gsl"
    TYPE RECOMMENDED
    PURPOSE "Required by Krita's Transform tool.")
macro_bool_to_01(GSL_FOUND HAVE_GSL)
configure_file(config-gsl.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-gsl.h )

 ###########################
############################
## Optional dependencies  ##
############################
###########################


# WebP, SeExpr and the corresponding Krita plug-ins are not part of the
# AI-first application.  Keep the generated config header deterministic
# rather than probing build-machine-specific optional dependencies.
set(KSeExpr_FOUND FALSE)
set(HAVE_SEEXPR 0)
configure_file(config-seexpr.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-seexpr.h )

find_package(ZLIB REQUIRED)
set_package_properties(ZLIB PROPERTIES
    DESCRIPTION "Compression library"
    URL "https://www.zlib.net/"
    TYPE REQUIRED
    PURPOSE "Required by Krita's PNG and PSD support")
macro_bool_to_01(ZLIB_FOUND HAVE_ZLIB)

# HDR colour spaces and OpenEXR I/O belonged to Krita's broad editing stack.
# Generated AI layers and the retained PNG/KRA paths use the standard colour
# pipeline, so omit that optional dependency entirely.
set(OpenEXR_FOUND FALSE)
set(HAVE_OPENEXR 0)
set(LINK_OPENEXR_LIB "")

# The application keeps native .kra and PNG paths only.  Do not discover the
# TIFF/PSD-TIFF stack that belonged to Krita's general import/export surface.
set(TIFF_FOUND FALSE)
set(TIFF_HAS_PSD_TAGS FALSE)
set(TIFF_CAN_WRITE_PSD_TAGS FALSE)
configure_file(config-tiff.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-tiff.h)

find_package(JPEG)
set_package_properties(JPEG PROPERTIES
    DESCRIPTION "Free library for JPEG image compression. Note that libjpeg8 is NOT supported."
    URL "https://www.libjpeg-turbo.org"
    TYPE OPTIONAL
    PURPOSE "Required by the Krita JPEG filter")
if (JPEG_FOUND)
    macro_bool_to_01(JPEG_FOUND HAVE_JPEG)
endif()

find_package(libjpeg-turbo 2.1.3 COMPONENTS turbojpeg)
set_package_properties(libjpeg-turbo PROPERTIES
    DESCRIPTION "libjpeg-turbo is a JPEG image codec that uses SIMD instructions (MMX, SSE2, AVX2, Neon, AltiVec) to accelerate baseline JPEG compression and decompression on x86, x86-64, Arm, and PowerPC systems, as well as progressive JPEG compression on x86 and x86-64 systems."
    URL "https://www.libjpeg-turbo.org"
    TYPE OPTIONAL
    PURPOSE "Required by the Krita JPEG and TIFF filters")
macro_bool_to_01(libjpeg-turbo_FOUND HAVE_JPEG_TURBO)
configure_file(config-jpeg.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-jpeg.h )

if(libjpeg-turbo_FOUND)
    set(JPEG_TURBO_LIBRARIES libjpeg-turbo::turbojpeg)
endif()

# General-purpose codec, convolution, and LUT-docker dependencies are not
# used by the retained document formats or the native AI workflow.
set(FFTW3_FOUND FALSE)
set(HAVE_FFTW3 0)
set(OpenColorIO_FOUND FALSE)
set(HAVE_OCIO 0)
configure_file(config-ocio.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-ocio.h)

# Scripting plugins are intentionally not part of this application.
set(HAVE_PYTHONLIBS 0)
set(HAVE_SIP 0)
set(HAVE_PYQT5 0)
set(HAVE_PYQT6 0)

# Animation/video support is outside the scope of the illustration app.
set(Mlt7_FOUND FALSE)
set(HAVE_MLT 0)
configure_file(config-mlt.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-mlt.h)

# MyPaint is a general brush-engine dependency.  Generation creates native AI
# layers instead, so it is excluded from this application.
set(LibMyPaint_FOUND FALSE)
set(HAVE_LIBMYPAINT 0)
configure_file(config-mypaint.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-mypaint.h )

##
## Look for OpenGL
##
# TODO: see if there is a better check for QtGui being built with opengl support (and thus the QOpenGL* classes)
if (QT_MAJOR_VERSION STREQUAL "5")
    if(Qt5Gui_OPENGL_IMPLEMENTATION)
        message(STATUS "Found QtGui OpenGL support")
    else()
        message(FATAL_ERROR  "Did NOT find QtGui OpenGL support. Check your Qt configuration. You cannot build Krita without Qt OpenGL support.")
    endif()
endif()

##
## Test for eigen3
##
find_package(Eigen3 5 NO_MODULE)
if(NOT Eigen3_FOUND)
    find_package(Eigen3 3.3 REQUIRED NO_MODULE)
endif()
set_package_properties(Eigen3 PROPERTIES
    DESCRIPTION "C++ template library for linear algebra"
    URL "http://eigen.tuxfamily.org"
    TYPE REQUIRED)

##
## Test for exiv2
##
find_package(LibExiv2 0.16 REQUIRED)

##
## Test for lcms
##
find_package(LCMS2 2.4 REQUIRED)
set_package_properties(LCMS2 PROPERTIES
    DESCRIPTION "LittleCMS Color management engine"
    URL "http://www.littlecms.com"
    TYPE REQUIRED
    PURPOSE "Will be used for color management and is necessary for Krita")
if(LCMS2_FOUND)
    if(NOT ${LCMS2_VERSION} VERSION_LESS 2.4 )
        set(HAVE_LCMS24 TRUE)
    endif()
    set(HAVE_REQUIRED_LCMS_VERSION TRUE)
    set(HAVE_LCMS2 TRUE)

    include(CheckIncludeFileCXX)

    set(CMAKE_REQUIRED_INCLUDES ${LCMS2_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${LCMS2_LIBRARIES})

    if(LCMS2_FAST_FLOAT_INCLUDE_DIR)
        message(STATUS "Found LittleCMS's fast float plugin")
        set(HAVE_LCMS_FAST_FLOAT_PLUGIN 1)
    endif()
endif()

##
## Test for xsimd
##
## xsimd has `SameMajorVersion` compatibility option set
## in `write_basic_package_version_file`, so we cannot just
## write a version range in the find_package call. Instead we
## should try all the supported versions manually
##
## We're trying out more recent versions first to make
## set_package_properties report the lowest one if no
## xsimd is found
##
foreach(xsimd_version 14 13 12 11 10 9 8.1.0)
    if(NOT xsimd_FOUND)
        find_package(xsimd ${xsimd_version})
    endif()
endforeach()

set_package_properties(xsimd PROPERTIES
    DESCRIPTION "C++ wrappers for SIMD intrinsics"
    URL "https://github.com/xtensor-stack/xsimd"
    TYPE REQUIRED
    PURPOSE "Required by Krita for vectorization")

if (xsimd_FOUND)
    macro(ko_compile_for_all_implementations_no_scalar _objs _src)
        if("aarch64" IN_LIST XSIMD_ARCH)
            xsimd_compile_for_all_implementations(${_objs} ${_src} FLAGS ${xsimd_ARCHITECTURE_FLAGS} ONLY NEON64)
        endif()

        if ("arm" IN_LIST XSIMD_ARCH)
            xsimd_compile_for_all_implementations(${_objs} ${_src} FLAGS ${xsimd_ARCHITECTURE_FLAGS} ONLY NEON)
        endif()

        if ("x86" IN_LIST XSIMD_ARCH OR "x86-64" IN_LIST XSIMD_ARCH)
            xsimd_compile_for_all_implementations(${_objs} ${_src} FLAGS ${xsimd_ARCHITECTURE_FLAGS} ONLY SSE2 SSSE3 SSE4_1 AVX AVX2+FMA)
        endif()
    endmacro()

    macro(ko_compile_for_all_implementations _objs _src)
        xsimd_compile_for_all_implementations(${_objs} ${_src} FLAGS ${xsimd_ARCHITECTURE_FLAGS} ONLY Scalar)
        ko_compile_for_all_implementations_no_scalar(${_objs} ${_src})
    endmacro()
else()
    # Make noop functions as they are used in nested cmake files
    # without guards
    macro(ko_compile_for_all_implementations_no_scalar _objs _src)
    endmacro()

    macro(ko_compile_for_all_implementations _objs _src)
    endmacro()
endif()

##
## Test endianness
##
include (TestBigEndian)
test_big_endian(CMAKE_WORDS_BIGENDIAN)

##
## Test for libav on Android
##
if(ANDROID)
    find_package(PkgConfig REQUIRED)
    set_package_properties(PkgConfig PROPERTIES
        DESCRIPTION "pkg-config"
        URL "https://www.freedesktop.org/wiki/Software/pkg-config/"
        TYPE REQUIRED
        PURPOSE "Needed to find ffmpeg libraries on Android."
    )

    pkg_check_modules(LibAV REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavfilter
        libavformat
        libavutil
        libswscale
    )
    if(TARGET PkgConfig::LibAV)
        add_library(LibAV ALIAS PkgConfig::LibAV)
    endif()

    # The PkgConfig and FeatureSummary modules don't play nice with each other
    # on their own, so this has to be set manually for the summary to show up.
    if(TARGET LibAV)
        set_property(GLOBAL APPEND PROPERTY PACKAGES_FOUND LibAV)
    else()
        set_property(GLOBAL APPEND PROPERTY PACKAGES_NOT_FOUND LibAV)
    endif()

    set_package_properties(LibAV PROPERTIES
        DESCRIPTION "FFmpeg libraries"
        URL "https://ffmpeg.org/"
        TYPE REQUIRED
        PURPOSE "Needed to render animations and timelapses on Android."
    )
endif()

##
## Test for quazip
##
find_package(QUAZIP 0.6)
set_package_properties(QUAZIP PROPERTIES
    DESCRIPTION "A library for reading and writing zip files"
    URL "https://stachenov.github.io/quazip/"
    TYPE REQUIRED
    PURPOSE "Needed for reading and writing KRA and ORA files"
)

##
## Check for freetype
##
find_package(Freetype 2.11.0 REQUIRED)
set_package_properties(Freetype PROPERTIES
    DESCRIPTION "A library for rendering glyphs"
    URL "https://download.savannah.gnu.org/releases/freetype"
    TYPE REQUIRED
    PURPOSE "Needed for rendering text vector shapes."
)

##
## Check for HarfBuzz
##
if (QT_MAJOR_VERSION STREQUAL "6")
    find_package(HarfBuzz 2.7.4 REQUIRED)
else()
    find_package(HarfBuzz 4.0 REQUIRED)
endif()
set_package_properties(HarfBuzz PROPERTIES
    DESCRIPTION "OpenType text shaping engine"
    URL "https://harfbuzz.github.io"
    TYPE REQUIRED
    PURPOSE "Needed for rendering text vector shapes."
)
if (HarfBuzz_FOUND)
    list(APPEND ANDROID_EXTRA_LIBS ${HarfBuzz_LIBRARIES})
endif()

# Note: We now build a patched raqm from source with Krita.

##
## Check for fontconfig
##
find_package(Fontconfig 2.13.1 REQUIRED)
set_package_properties(Fontconfig PROPERTIES
    TYPE REQUIRED
    PURPOSE "Needed for getting font file information."
)

##
## Check for libunibreak
##
find_package(libunibreak 5.0 REQUIRED)
set_package_properties(libunibreak PROPERTIES
    DESCRIPTION "Implementation of the line breaking and word breaking algorithms as described in Unicode Standard Annex 14 and Unicode Standard Annex 29"
    URL "https://github.com/adah1972/libunibreak"
    TYPE REQUIRED
    PURPOSE "Needed for rendering text vector shapes."
)

##
## Build vendored libraries
##

# Allow specification of a directory with pre-downloaded
# requirements. This download directory is used for 3rdparty_vendor
# download

if(DEFINED ENV{EXTERNALS_DOWNLOAD_DIR})
    set(EXTERNALS_DOWNLOAD_DIR $ENV{EXTERNALS_DOWNLOAD_DIR})
endif()

if(NOT IS_DIRECTORY ${EXTERNALS_DOWNLOAD_DIR})
    message(WARNING "No externals download dir set, default location inside build tree is used. Use -DEXTERNALS_DOWNLOAD_DIR to override")
else()
    file(TO_CMAKE_PATH "${EXTERNALS_DOWNLOAD_DIR}" EXTERNALS_DOWNLOAD_DIR)
endif()

add_subdirectory(3rdparty_vendor)

##
## Test for Atomics
##
include(CheckAtomic)

 ############################
#############################
## Add Krita helper macros ##
#############################
############################

include(KritaAddBrokenUnitTest)
include(MacroKritaAddBenchmark)

 ####################
#####################
## Define includes ##
#####################
####################

# for config.h and <toplevel/foo.h> includes (if any?)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_BINARY_DIR}
)

if (BUILD_TESTING)
    add_subdirectory(sdk/tests)
endif()

add_subdirectory(libs)
# The upstream QML module collection powered optional Krita workspaces.
add_subdirectory(plugins)
# Upstream Krita performance benchmarks have been moved to source/benchmarks
# for future reference and optimization experiments.
# if (BUILD_TESTING)
#     add_subdirectory(benchmarks)
# endif()
add_subdirectory(krita)

configure_file(KoConfig.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/KoConfig.h )
configure_file(KoTestConfig.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/KoTestConfig.h )
configure_file(config_convolution.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config_convolution.h)

check_function_exists(powf HAVE_POWF)
configure_file(config-powf.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-powf.h)

# The upstream NSIS/MSIX projects package Krita-specific icons, names, and
# shell integration. Distribution packaging for this focused application is
# deliberately kept separate from the source build.

message("\nBroken tests:")
foreach(tst ${KRITA_BROKEN_TESTS})
    message("    * ${tst}")
endforeach()

if(APPLE)
    # fix macos test rpath
    macos_test_fixrpath(${KRITA_TESTS_TARGET})
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

if (ANDROID)
  # ECM passes this to Qt, which was already capable of detecting it..
  set (ANDROID_SDK_COMPILE_API "36")

  # To assist stdcpp-path in deployment.json file
  set(ANDROID_SYSROOT_PREFIX "${ANDROID_TOOLCHAIN_ROOT}/sysroot/usr" CACHE STRING "" FORCE)

  # copied regex from ECM's toolchain
  string(REGEX REPLACE "-(clang)?([0-9].[0-9])?$" "" ECM_ANDROID_STL_ARCH ${ANDROID_TOOLCHAIN_NAME})

  if (NOT (CMAKE_CXX_STANDARD_LIBRARIES MATCHES "[^ ]*c\\+\\+[^ ]*\\.so"))

    set (KRITA_ANDROID_STL_PATH "${ANDROID_TOOLCHAIN_ROOT}/sysroot/usr/lib/${ECM_ANDROID_STL_ARCH}/lib${ANDROID_STL}.so")
    if (NOT EXISTS ${KRITA_ANDROID_STL_PATH})
      message(FATAL_ERROR "STL does not exist at the right location, please use NDK r20+")
    endif()

    set (CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} \"${KRITA_ANDROID_STL_PATH}\"" CACHE STRING "" FORCE)
  endif()
endif()

if(DEFINED QTANDROID_EXPORTED_TARGET AND NOT TARGET "create-apk")
    set (_CMAKE_ANDROID_DIR "${ECM_DIR}/../toolchain")
    list(LENGTH QTANDROID_EXPORTED_TARGET targetsCount)
    include(${_CMAKE_ANDROID_DIR}/ECMAndroidDeployQt.cmake)

    math(EXPR last "${targetsCount}-1")
    foreach(idx RANGE 0 ${last})
        list(GET QTANDROID_EXPORTED_TARGET ${idx} exportedTarget)
        list(GET ANDROID_APK_DIR ${idx} APK_DIR)
        if(APK_DIR AND NOT EXISTS "${ANDROID_APK_DIR}/AndroidManifest.xml" AND IS_ABSOLUTE ANDROID_APK_DIR)
            message(FATAL_ERROR "Cannot find ${APK_DIR}/AndroidManifest.xml according to ANDROID_APK_DIR. ${ANDROID_APK_DIR} ${exportedTarget}")
        elseif(NOT APK_DIR)
            get_filename_component(_qt5Core_install_prefix "${Qt5Core_DIR}/../../../" ABSOLUTE)
            set(APK_DIR "${_qt5Core_install_prefix}/src/android/templates/")
        endif()

        ecm_androiddeployqt("${exportedTarget}" "${ECM_ADDITIONAL_FIND_ROOT_PATH}")
        set_target_properties(create-apk-${exportedTarget} PROPERTIES ANDROID_APK_DIR "${APK_DIR}")
    endforeach()
elseif(ANDROID)
    message(STATUS "You can export a target by specifying -DQTANDROID_EXPORTED_TARGET=<targetname> and -DANDROID_APK_DIR=<paths>")
endif()

# The application uses its own Japanese-first UI strings and does not ship the
# upstream Krita translation catalogue.
