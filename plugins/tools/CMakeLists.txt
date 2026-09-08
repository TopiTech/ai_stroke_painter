add_subdirectory(basictools)
add_subdirectory(selectiontools)

install(FILES
    tools.action
DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions)
