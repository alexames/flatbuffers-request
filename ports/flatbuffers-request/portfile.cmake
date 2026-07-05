# Port for the alexames/vcpkg-registry. Copy this directory into that registry's
# ports/ tree and add a versions/ entry to publish a new version.
#
# On release: set REF to the tagged commit and fill SHA512 with the value
# printed by a first build (or `vcpkg hash <tarball>`).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO alexames/flatbuffers-request
    REF "v${VERSION}"
    SHA512 0
    HEAD_REF main
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tools FBREQUEST_BUILD_TOOLS
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DFBREQUEST_BUILD_TESTS=OFF
        -DFBREQUEST_BUILD_FUZZERS=OFF
        -DFBREQUEST_INSTALL=ON
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME FlatbuffersRequest
    CONFIG_PATH lib/cmake/FlatbuffersRequest
)

# Relocate the CLI (when the tools feature is on) from bin/ to tools/<port>/.
if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES fbrequest AUTO_CLEAN)
endif()

# A static library ships no headers or cmake config in the debug tree.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
