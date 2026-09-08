project(kritafilters)

add_subdirectory(libkra)

# Keep the native document format, PNG, and Qt-backed still-image codecs.
# Other format-specific import/export modules belonged to Krita's general
# purpose editor surface and are intentionally not part of this application.
if(PNG_FOUND)
    add_subdirectory(png)
endif()
add_subdirectory(qimageio)
add_subdirectory(kra)
