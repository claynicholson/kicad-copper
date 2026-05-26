# CopperPackaging.cmake
#
# CPack configuration for kicad-copper Windows releases.
# Produces:
#   - kicad-copper-<version>-win64.exe   (NSIS installer)
#   - kicad-copper-<version>-win64.zip   (portable archive)
#
# Only takes effect on Windows. Intentionally additive over upstream KiCad's
# CMakeLists.txt so merges from upstream do not collide.

if( NOT WIN32 )
    return()
endif()

# Read VERSION file if present so a single source of truth drives the package
# version, falling back to project() defaults if the file is absent (e.g. when
# a contributor builds from a tarball without a VERSION file).
# NOTE: this file is intentionally NOT named "VERSION" because the compiler's
# include path covers the repo root on Windows (case-insensitive FS), so
# `#include <version>` (C++20 std header) would otherwise resolve to it and
# the build would fail with "too many decimal points in number".
set( COPPER_VERSION_FILE "${CMAKE_SOURCE_DIR}/VERSION.txt" )
if( EXISTS "${COPPER_VERSION_FILE}" )
    file( READ "${COPPER_VERSION_FILE}" COPPER_VERSION_RAW )
    string( STRIP "${COPPER_VERSION_RAW}" COPPER_VERSION )
else()
    set( COPPER_VERSION "0.0.0" )
endif()

# An override via CMake -DCOPPER_VERSION=x.y.z always wins (used by CI to inject
# the tag-derived version without modifying the file in-tree).
if( DEFINED ENV{COPPER_VERSION} AND NOT "$ENV{COPPER_VERSION}" STREQUAL "" )
    set( COPPER_VERSION "$ENV{COPPER_VERSION}" )
endif()

message( STATUS "kicad-copper packaging version: ${COPPER_VERSION}" )

# Split into major/minor/patch for CPack
string( REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _copper_match "${COPPER_VERSION}" )
if( _copper_match )
    set( COPPER_VERSION_MAJOR "${CMAKE_MATCH_1}" )
    set( COPPER_VERSION_MINOR "${CMAKE_MATCH_2}" )
    set( COPPER_VERSION_PATCH "${CMAKE_MATCH_3}" )
else()
    set( COPPER_VERSION_MAJOR "0" )
    set( COPPER_VERSION_MINOR "0" )
    set( COPPER_VERSION_PATCH "0" )
endif()

set( CPACK_PACKAGE_NAME            "KiCad Copper" )
set( CPACK_PACKAGE_VENDOR          "kicad-copper" )
set( CPACK_PACKAGE_VERSION         "${COPPER_VERSION}" )
set( CPACK_PACKAGE_VERSION_MAJOR   "${COPPER_VERSION_MAJOR}" )
set( CPACK_PACKAGE_VERSION_MINOR   "${COPPER_VERSION_MINOR}" )
set( CPACK_PACKAGE_VERSION_PATCH   "${COPPER_VERSION_PATCH}" )
set( CPACK_PACKAGE_DESCRIPTION_SUMMARY
     "KiCad fork with an embedded Copper chat panel and schematic-generation backend integration." )
set( CPACK_PACKAGE_HOMEPAGE_URL    "https://github.com/claynicholson/kicad-copper" )
set( CPACK_PACKAGE_INSTALL_DIRECTORY "KiCad Copper" )
set( CPACK_PACKAGE_FILE_NAME       "kicad-copper-${COPPER_VERSION}-win64" )

set( CPACK_RESOURCE_FILE_LICENSE   "${CMAKE_SOURCE_DIR}/LICENSE" )
set( CPACK_RESOURCE_FILE_README    "${CMAKE_SOURCE_DIR}/README.md" )

# Generators: NSIS (single .exe installer) + ZIP (portable). NSIS must be on
# PATH when `cpack` runs; the CI installs it via MSYS2.
set( CPACK_GENERATOR               "NSIS;ZIP" )

set( CPACK_NSIS_PACKAGE_NAME       "KiCad Copper ${COPPER_VERSION}" )
set( CPACK_NSIS_DISPLAY_NAME       "KiCad Copper ${COPPER_VERSION}" )
set( CPACK_NSIS_INSTALL_ROOT       "$PROGRAMFILES64" )
set( CPACK_NSIS_MODIFY_PATH        ON )
set( CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON )
set( CPACK_NSIS_URL_INFO_ABOUT     "${CPACK_PACKAGE_HOMEPAGE_URL}" )
set( CPACK_NSIS_HELP_LINK          "${CPACK_PACKAGE_HOMEPAGE_URL}" )
set( CPACK_NSIS_CONTACT            "${CPACK_PACKAGE_HOMEPAGE_URL}/issues" )

# Components: pull in everything CMake installs. Upstream uses install() rules
# without grouping into components so we let CPack collect them under default
# component groups.
set( CPACK_MONOLITHIC_INSTALL      ON )

include( CPack )
