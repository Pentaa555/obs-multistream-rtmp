cmake_minimum_required(VERSION 3.28)

project(obs-multistream-rtmp VERSION 0.1.1 LANGUAGES CXX)

set(OBS_VALIDATED_VERSION "32.2.2" CACHE STRING
    "OBS Studio version validated for the Linux distribution artifact")
set(TWITCH_CLIENT_ID "" CACHE STRING
    "Public Twitch application Client ID used by the system-browser OAuth flow")
set(YOUTUBE_CLIENT_ID "" CACHE STRING
    "Google OAuth Desktop-app Client ID used by the YouTube Live integration")
set(YOUTUBE_CLIENT_SECRET "" CACHE STRING
    "Optional Google OAuth Desktop-app credential for release builds; never commit it")
# Prefer an explicitly supplied CMake value, otherwise allow a release pipeline
# to inject the public-client credential through its protected environment.
if(NOT YOUTUBE_CLIENT_SECRET AND DEFINED ENV{YOUTUBE_CLIENT_SECRET})
    set(YOUTUBE_CLIENT_SECRET "$ENV{YOUTUBE_CLIENT_SECRET}")
endif()
message(STATUS "Validated OBS Studio version: ${OBS_VALIDATED_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build unit tests" ON)

include(GNUInstallDirs)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Svg)
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
endif()

# Match the plugin directory used by the OBS installation that provided the
# imported targets. OBS Linux packages may use a multiarch lib directory.
set(OBS_PLUGIN_INSTALL_DIR "" CACHE STRING
    "Installation directory for the OBS plugin binary")
if(NOT OBS_PLUGIN_INSTALL_DIR)
    if(WIN32)
        set(OBS_PLUGIN_INSTALL_DIR "obs-plugins/64bit")
    elseif(APPLE)
        set(OBS_PLUGIN_INSTALL_DIR "obs-plugins")
    else()
        set(OBS_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/obs-plugins")

        find_file(OBS_CONFIG_FILE obsconfig.h
            PATHS ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES include/obs
            NO_DEFAULT_PATH
        )
        if(OBS_CONFIG_FILE)
            file(STRINGS "${OBS_CONFIG_FILE}" OBS_PLUGIN_DESTINATION_LINE
                REGEX "^#define OBS_PLUGIN_DESTINATION ")
            if(OBS_PLUGIN_DESTINATION_LINE)
                string(REGEX REPLACE
                    "^#define OBS_PLUGIN_DESTINATION[ \t]+\\\"([^\\\"]+)\\\".*"
                    "\\1"
                    OBS_PLUGIN_INSTALL_DIR
                    "${OBS_PLUGIN_DESTINATION_LINE}"
                )
            endif()
        endif()
    endif()
endif()

message(STATUS "OBS plugin install directory: ${OBS_PLUGIN_INSTALL_DIR}")

add_library(obs-multistream-rtmp MODULE
    src/destination.cpp
    src/destination-provider.cpp
    src/destination-manager.cpp
    src/facebook-provider.cpp
    src/facebook-oauth-dialog.cpp
    src/facebook-utils.cpp
    src/loopback-oauth.cpp
    src/twitch-provider.cpp
    src/twitch-utils.cpp
    src/youtube-provider.cpp
    src/youtube-utils.cpp
    src/secure-token-store.cpp
    src/multistream-dock.cpp
    src/plugin-main.cpp
    resources/resources.qrc
)

target_include_directories(obs-multistream-rtmp PRIVATE src)
target_link_libraries(obs-multistream-rtmp PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
)
if(UNIX AND NOT APPLE)
    target_link_libraries(obs-multistream-rtmp PRIVATE PkgConfig::LIBSECRET)
endif()

target_compile_definitions(obs-multistream-rtmp PRIVATE
    PLUGIN_VERSION=\"${PROJECT_VERSION}\"
    TWITCH_CLIENT_ID=\"${TWITCH_CLIENT_ID}\"
    YOUTUBE_CLIENT_ID=\"${YOUTUBE_CLIENT_ID}\"
    QT_NO_KEYWORDS
)
if(YOUTUBE_CLIENT_SECRET)
    message(STATUS "YouTube Desktop client credential enabled for this build")
    target_compile_definitions(obs-multistream-rtmp PRIVATE
        YOUTUBE_CLIENT_SECRET=\"${YOUTUBE_CLIENT_SECRET}\"
    )
endif()

target_compile_options(obs-multistream-rtmp PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

set_target_properties(obs-multistream-rtmp PROPERTIES
    PREFIX ""
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

if(UNIX AND NOT APPLE)
    # Installed OBS libraries are one directory above obs-plugins.
    set_property(TARGET obs-multistream-rtmp PROPERTY INSTALL_RPATH "$ORIGIN/..")
endif()

install(TARGETS obs-multistream-rtmp
    LIBRARY DESTINATION "${OBS_PLUGIN_INSTALL_DIR}"
    RUNTIME DESTINATION "${OBS_PLUGIN_INSTALL_DIR}"
)
install(DIRECTORY data/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/obs-multistream-rtmp"
)
install(FILES
    LICENSE
    README.md
    docs/production-scope.md
    packaging/README-linux.md
    packaging/README-deb.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/obs-multistream-rtmp"
)

set(CPACK_PACKAGE_NAME "obs-multistream-rtmp")
set(CPACK_PACKAGE_VENDOR "Penta Multistream")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OBS Multistream RTMP plugin")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Penta Multistream <mauro8976we45@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "obs-studio (>= 32.2.2), libsecret-1-0, libqt6core6, libqt6gui6, libqt6widgets6, libqt6network6, libqt6svg6")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/packaging/README-linux.md")
include(CPack)

if(BUILD_TESTING)
    function(enable_test_assertions target)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE -UNDEBUG)
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /UNDEBUG)
        endif()
    endfunction()

    include(CTest)
    if(BUILD_TESTING)
        add_executable(destination-tests
            tests/destination-test.cpp
            src/destination.cpp
        )
        target_include_directories(destination-tests PRIVATE src)
        add_test(NAME destination-tests COMMAND destination-tests)
        enable_test_assertions(destination-tests)

        add_executable(destination-manager-tests
            tests/destination-manager-test.cpp
            src/destination-manager.cpp
            src/destination-provider.cpp
            src/destination.cpp
        )
        target_include_directories(destination-manager-tests PRIVATE src)
        target_link_libraries(destination-manager-tests PRIVATE
            OBS::libobs
            OBS::obs-frontend-api
        )
        add_test(NAME destination-manager-tests COMMAND destination-manager-tests)
        enable_test_assertions(destination-manager-tests)

        add_executable(facebook-utils-tests
            tests/facebook-utils-test.cpp
            src/facebook-utils.cpp
        )
        target_include_directories(facebook-utils-tests PRIVATE src)
        target_link_libraries(facebook-utils-tests PRIVATE Qt6::Core)
        add_test(NAME facebook-utils-tests COMMAND facebook-utils-tests)
        enable_test_assertions(facebook-utils-tests)

        add_executable(facebook-provider-tests
            tests/facebook-provider-test.cpp
            src/facebook-provider.cpp
            src/facebook-utils.cpp
            src/loopback-oauth.cpp
            src/secure-token-store.cpp
            src/destination.cpp
        )
        target_include_directories(facebook-provider-tests PRIVATE src)
        target_link_libraries(facebook-provider-tests PRIVATE
            Qt6::Core
            Qt6::Widgets
            Qt6::Network
        )
        if(UNIX AND NOT APPLE)
            target_link_libraries(facebook-provider-tests PRIVATE PkgConfig::LIBSECRET)
        endif()
        add_test(NAME facebook-provider-tests COMMAND facebook-provider-tests)
        enable_test_assertions(facebook-provider-tests)

        add_executable(twitch-utils-tests
            tests/twitch-utils-test.cpp
            src/twitch-utils.cpp
        )
        target_include_directories(twitch-utils-tests PRIVATE src)
        target_link_libraries(twitch-utils-tests PRIVATE Qt6::Core)
        add_test(NAME twitch-utils-tests COMMAND twitch-utils-tests)
        enable_test_assertions(twitch-utils-tests)

        add_executable(twitch-provider-tests
            tests/twitch-provider-test.cpp
            src/twitch-provider.cpp
            src/twitch-utils.cpp
            src/loopback-oauth.cpp
            src/secure-token-store.cpp
            src/destination.cpp
        )
        target_include_directories(twitch-provider-tests PRIVATE src)
        target_link_libraries(twitch-provider-tests PRIVATE
            Qt6::Core
            Qt6::Widgets
            Qt6::Network
            OBS::libobs
        )
        if(UNIX AND NOT APPLE)
            target_link_libraries(twitch-provider-tests PRIVATE PkgConfig::LIBSECRET)
        endif()
        add_test(NAME twitch-provider-tests COMMAND twitch-provider-tests)
        enable_test_assertions(twitch-provider-tests)

        add_executable(youtube-utils-tests
            tests/youtube-utils-test.cpp
            src/youtube-utils.cpp
        )
        target_include_directories(youtube-utils-tests PRIVATE src)
        target_link_libraries(youtube-utils-tests PRIVATE Qt6::Core)
        add_test(NAME youtube-utils-tests COMMAND youtube-utils-tests)
        enable_test_assertions(youtube-utils-tests)

        add_executable(youtube-provider-tests
            tests/youtube-provider-test.cpp
            src/youtube-provider.cpp
            src/youtube-utils.cpp
            src/loopback-oauth.cpp
            src/secure-token-store.cpp
            src/destination.cpp
        )
        target_include_directories(youtube-provider-tests PRIVATE src)
        target_link_libraries(youtube-provider-tests PRIVATE
            Qt6::Core
            Qt6::Widgets
            Qt6::Network
            OBS::libobs
        )
        if(UNIX AND NOT APPLE)
            target_link_libraries(youtube-provider-tests PRIVATE PkgConfig::LIBSECRET)
        endif()
        add_test(NAME youtube-provider-tests COMMAND youtube-provider-tests)
        enable_test_assertions(youtube-provider-tests)
    endif()
endif()
