"""Krita プラグインのエントリーポイント。Krita 外ではドメイン層だけを利用可能にする。"""

from .version import PLUGIN_VERSION

__version__ = PLUGIN_VERSION
__all__ = ["__version__"]

try:
    from krita import DockWidgetFactory, DockWidgetFactoryBase, Krita
except ImportError:
    # CI とローカルのセルフテストでは Krita は存在しない。
    pass
else:
    from .docker import AIStrokePainterDocker

    DOCKER_ID = "ai_stroke_painter_docker"
    dock_position = getattr(
        getattr(DockWidgetFactoryBase, "DockPosition", None),
        "DockRight",
        getattr(DockWidgetFactoryBase, "DockRight", None),
    )
    Krita.instance().addDockWidgetFactory(DockWidgetFactory(DOCKER_ID, dock_position, AIStrokePainterDocker))
