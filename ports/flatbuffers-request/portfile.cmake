# Port for the alexames/vcpkg-registry. Copy this directory into that registry's
# ports/ tree and add a versions/ entry to publish a new version.
#
# On release: set REF to the tag and fill SHA512 with the value printed by a
# first build (or `vcpkg hash <tarball>`).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO alexames/flatbuffers-request
    REF "v${VERSION}"
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DFBREQUEST_BUILD_TESTS=OFF
        -DFBREQUEST_BUILD_FUZZERS=OFF
        -DFBREQUEST_INSTALL=ON
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME FlatbuffersRequest
    CONFIG_PATH lib/cmake/FlatbuffersRequest
)

# A static library ships no headers in the debug tree.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
