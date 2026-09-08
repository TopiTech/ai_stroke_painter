set(ai_stroke_painter_BUNDLE_FILES
    Krita_4_Default_Resources.bundle
)

install(FILES
    ${ai_stroke_painter_BUNDLE_FILES}
    DESTINATION ${KDE_INSTALL_DATADIR}/krita/bundles
)
