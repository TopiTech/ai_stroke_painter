# Runtime data is intentionally limited to native document compatibility and
# colour management. A single baseline bundle remains an internal dependency
# of Krita's resource database; its manual-painting UI is not exposed.
add_subdirectory( bundles )
add_subdirectory( profiles )
add_subdirectory( shortcuts )
add_subdirectory( metadata/schemas )
