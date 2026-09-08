"""パレットの色名を、背景・主役・線・アクセントの値役割へ変換する。"""

from __future__ import annotations

from dataclasses import dataclass


def _rgb(color: str) -> tuple[float, float, float]:
    raw = color.lstrip("#")[:6]
    if len(raw) == 3:
        raw = "".join(character * 2 for character in raw)
    try:
        return tuple(int(raw[index : index + 2], 16) / 255.0 for index in (0, 2, 4))  # type: ignore[return-value]
    except (TypeError, ValueError):
        return 0.5, 0.5, 0.5


def _hex(rgb: tuple[float, float, float]) -> str:
    return "#" + "".join(f"{round(max(0.0, min(1.0, channel)) * 255):02x}" for channel in rgb)


def mix_colors(first: str, second: str, phase: float) -> str:
    a = _rgb(first)
    b = _rgb(second)
    t = max(0.0, min(1.0, phase))
    return _hex(tuple(a[index] * (1.0 - t) + b[index] * t for index in range(3)))  # type: ignore[arg-type]


def relative_luminance(color: str) -> float:
    def linear(channel: float) -> float:
        return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4

    red, green, blue = (linear(channel) for channel in _rgb(color))
    return 0.2126 * red + 0.7152 * green + 0.0722 * blue


def contrast_ratio(first: str, second: str) -> float:
    lighter, darker = sorted((relative_luminance(first), relative_luminance(second)), reverse=True)
    return (lighter + 0.05) / (darker + 0.05)


def _nearest_palette_color(
    target: str,
    palette: tuple[str, ...],
    *,
    contrast_with: str | None = None,
    minimum_contrast: float = 1.0,
) -> str:
    eligible = (
        tuple(color for color in palette if contrast_ratio(color, contrast_with) >= minimum_contrast)
        if contrast_with is not None
        else palette
    )
    candidates = eligible or palette
    target_rgb = _rgb(target)
    return min(
        candidates,
        key=lambda color: sum((channel - target_rgb[index]) ** 2 for index, channel in enumerate(_rgb(color))),
    )


@dataclass(frozen=True)
class ColorPlan:
    palette_name: str
    background: str
    background_deep: str
    background_detail: str
    subject_base: str
    subject_mid: str
    subject_shadow: str
    lineart: str
    accent: str
    highlight: str
    dark_background: bool

    def as_dict(self) -> dict[str, object]:
        return {
            "palette_name": self.palette_name,
            "background": self.background,
            "background_deep": self.background_deep,
            "background_detail": self.background_detail,
            "subject_base": self.subject_base,
            "subject_mid": self.subject_mid,
            "subject_shadow": self.subject_shadow,
            "lineart": self.lineart,
            "accent": self.accent,
            "highlight": self.highlight,
            "dark_background": self.dark_background,
            "subject_background_contrast": round(contrast_ratio(self.subject_base, self.background), 3),
            "line_subject_contrast": round(contrast_ratio(self.lineart, self.subject_base), 3),
            "accent_background_contrast": round(contrast_ratio(self.accent, self.background), 3),
        }


def build_color_plan(palette_name: str, colors: dict[str, str]) -> ColorPlan:
    dark_background = palette_name in {"cyberpunk", "dark_fantasy", "cyber_gold"}
    light_background = colors.get("sky_horizon", colors.get("skin_base", "#f5f5f5"))
    dark_value = colors.get("sky_zenith", colors.get("cloth_shadow", colors.get("hair_shadow", "#202020")))
    background = dark_value if dark_background else light_background
    background_deep = colors.get("cloth_shadow", dark_value) if dark_background else dark_value
    lineart = colors.get("lineart", "#202020")
    subject_base = colors.get("skin_base", colors.get("hair_main", "#d0d0d0"))
    if not dark_background and contrast_ratio(subject_base, background) < 1.35:
        for phase in (0.14, 0.20, 0.28, 0.36, 0.44):
            candidate = mix_colors(light_background, dark_value, phase)
            background = candidate
            if contrast_ratio(subject_base, candidate) >= 1.35:
                break
    if contrast_ratio(lineart, subject_base) < 3.0:
        black_ratio = contrast_ratio("#111111", subject_base)
        white_ratio = contrast_ratio("#f8f8f8", subject_base)
        lineart = "#111111" if black_ratio >= white_ratio else "#f8f8f8"
    accent = colors.get("fx", colors.get("highlight", "#ffd700"))
    if contrast_ratio(accent, background) < 1.8:
        candidates = (
            accent,
            colors.get("highlight", "#ffffff"),
            colors.get("hair_shadow", lineart),
            colors.get("cloth_main", lineart),
            lineart,
        )
        accent = max(candidates, key=lambda color: contrast_ratio(color, background))
    background_detail = (
        mix_colors(background, accent, 0.32) if dark_background else mix_colors(background, background_deep, 0.34)
    )
    subject_mid = colors.get("hair_main", colors.get("cloth_main", subject_base))
    subject_shadow = colors.get("hair_shadow", colors.get("skin_shadow", background_deep))
    highlight = colors.get("highlight", "#ffffff")

    # 無彩色・水墨パレットは色域そのものが表現契約なので、中間色も登録色へ量子化する。
    if palette_name in {"monochrome", "sumie", "sumi_e", "ink"}:
        palette_colors = tuple(dict.fromkeys(color.lower() for color in colors.values()))
        subject_base = _nearest_palette_color(subject_base, palette_colors)
        background = _nearest_palette_color(
            background,
            palette_colors,
            contrast_with=subject_base,
            minimum_contrast=1.35,
        )
        background_deep = _nearest_palette_color(background_deep, palette_colors)
        background_detail = _nearest_palette_color(background_detail, palette_colors)
        subject_mid = _nearest_palette_color(subject_mid, palette_colors)
        subject_shadow = _nearest_palette_color(subject_shadow, palette_colors)
        lineart = _nearest_palette_color(
            lineart,
            palette_colors,
            contrast_with=subject_base,
            minimum_contrast=3.0,
        )
        accent = _nearest_palette_color(
            accent,
            palette_colors,
            contrast_with=background,
            minimum_contrast=1.8,
        )
        highlight = _nearest_palette_color(highlight, palette_colors)
    return ColorPlan(
        palette_name=palette_name,
        background=background,
        background_deep=background_deep,
        background_detail=background_detail,
        subject_base=subject_base,
        subject_mid=subject_mid,
        subject_shadow=subject_shadow,
        lineart=lineart,
        accent=accent,
        highlight=highlight,
        dark_background=dark_background,
    )
