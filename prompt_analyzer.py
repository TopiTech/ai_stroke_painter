"""自然言語プロンプトから描画指示（属性・構図・環境・色彩）を構造化抽出するアナライザー。

日本語・英語の両対応で、ユーザー指示の細部（髪型、髪色、瞳色、表情、服装、装飾品、
時間帯、天候、ライティング、画風）を検出し、LLMおよびプロシージャルエンジンへ注入する。
"""

from __future__ import annotations

from dataclasses import dataclass, field
import re


@dataclass(frozen=True)
class CharacterAttributes:
    """キャラクター特有の描画属性。"""

    gender: str = "unspecified"  # "unspecified", "female", "male", "neutral"
    hair_style: str = "default"  # "twintails", "ponytail", "bob", "short", "long", "wavy", "braids", "default"
    hair_color: str | None = None  # Normalized HEX or None
    eye_color: str | None = None  # Normalized HEX or None
    expression: str = "default"  # "smile", "serious", "wink", "sad", "blush", "gentle", "angry", "default"
    costume: str = "default"  # "school_uniform", "sailor", "blazer", "kimono", "hoodie", "dress", "suit", "default"
    accessories: tuple[str, ...] = ()  # ("glasses", "ribbon", "cat_ears", "hat", "hairclip", etc.)
    view_angle: str = "auto"  # "auto", "front", "three_quarters", "profile", "tilt"


@dataclass(frozen=True)
class EnvironmentAttributes:
    """背景・環境・時間帯・天候属性。"""

    setting: str = "none"  # "classroom", "room", "city", "nature", "mountain", "sea", "garden", "none"
    time_of_day: str = "day"  # "day", "sunset", "twilight", "night", "dawn"
    weather_elements: tuple[str, ...] = ()  # ("rain", "snow", "sunny", "clouds", "stars", "aurora", "sakura", etc.)


@dataclass(frozen=True)
class LightingAttributes:
    """照明・色彩トーン属性。"""

    key_light: str = "natural"  # "natural", "backlight", "rim_light", "soft", "dramatic"
    tone: str = "natural"  # "warm", "cool", "sunset", "night", "pastel", "vivid", "monochrome"


@dataclass(frozen=True)
class SemanticAttributes:
    """プロンプトから抽出された構造化描画指示。"""

    raw_prompt: str
    category: str = "character"  # "character", "creature", "landscape", "geometry", "fx"
    character: CharacterAttributes = field(default_factory=CharacterAttributes)
    environment: EnvironmentAttributes = field(default_factory=EnvironmentAttributes)
    lighting: LightingAttributes = field(default_factory=LightingAttributes)
    art_style: str = "anime"  # "anime", "watercolor", "manga", "painterly", "cyberpunk"
    explicit_tags: tuple[str, ...] = ()

    def to_directive_text(self) -> str:
        """LLMのシステムプロンプトへ注入するための描画指示ディレクティブ文を生成する。"""
        directives: list[str] = ["[PROMPT ART DIRECTION & DETECTED MOTIFS]"]

        # キャラクター要素
        c = self.character
        if self.category == "character":
            char_specs: list[str] = []
            if c.gender != "unspecified":
                char_specs.append(f"Gender: {c.gender}")
            if c.hair_style != "default":
                char_specs.append(f"Hair style: {c.hair_style.upper()}")
            if c.hair_color:
                char_specs.append(f"Hair color: {c.hair_color} (use this color on hair base/shadows)")
            if c.eye_color:
                char_specs.append(f"Eye color: {c.eye_color} (use this color on irises)")
            if c.expression != "default":
                char_specs.append(f"Facial expression: {c.expression.upper()} (reflect in eyebrows, eyes, mouth)")
            if c.costume != "default":
                char_specs.append(f"Clothing/Costume: {c.costume.upper()}")
            if c.accessories:
                char_specs.append(f"Accessories: {', '.join(a.upper() for a in c.accessories)}")
            if c.view_angle != "auto":
                char_specs.append(f"Angle/Perspective: {c.view_angle}")
            if char_specs:
                directives.append("- Character Appearance: " + " | ".join(char_specs))

        # 環境・背景要素
        e = self.environment
        env_specs: list[str] = []
        if e.setting != "none":
            env_specs.append(f"Location/Setting: {e.setting.upper()}")
        if e.time_of_day != "day":
            env_specs.append(f"Time of Day: {e.time_of_day.upper()} (adjust sky & ambient lighting)")
        if e.weather_elements:
            env_specs.append(f"Atmospheric elements: {', '.join(e.weather_elements)}")
        if env_specs:
            directives.append("- Environment & Setting: " + " | ".join(env_specs))

        # 照明・トーン
        lit_specs: list[str] = []
        if self.lighting.tone != "natural":
            lit_specs.append(f"Color Tone: {self.lighting.tone.upper()}")
        if self.lighting.key_light != "natural":
            lit_specs.append(f"Lighting Scheme: {self.lighting.key_light.upper()}")
        if lit_specs:
            directives.append("- Lighting & Mood: " + " | ".join(lit_specs))

        directives.append(
            "- Creative Autonomy: Faithfully express all detected motifs above while exercising full artistic freedom over composition, dynamic pose, camera perspective, and aesthetic styling!"
        )
        return "\n".join(directives)


# -----------------------------------------------------------------------------
# パターン辞書（日英両対応・同義語）
# -----------------------------------------------------------------------------

_GENDER_PATTERNS = [
    ("male", [r"\b(?:boy|man|men|guy|male|prince|hero|gentleman)\b", r"少年|男の子|男性|男子|青年|男|ヒーロー"]),
    ("female", [r"\b(?:girl|woman|female|lady|princess|heroine)\b", r"美少女|少女|女の子|女性|女子|ヒロイン|お嬢様"]),
]

_HAIR_STYLE_PATTERNS = [
    (
        "twintails",
        [r"\b(?:twin\s*tails?|twintails?|pigtails?|two\s*tails?)\b", r"ツインテール|ツーサイドアップ|おさげ"],
    ),
    ("ponytail", [r"\b(?:ponytail|single\s*tail|high\s*ponytail)\b", r"ポニーテール|ポニテ|サイドポニー"]),
    ("bob", [r"\b(?:bob(?:\s*cut)?|short\s*bob)\b", r"ボブ|ショートボブ|おかっぱ"]),
    ("short", [r"\b(?:short\s*hair|pixie\s*cut)\b", r"ショート(?:ヘア|カット)?|短髪"]),
    ("wavy", [r"\b(?:wavy\s*hair|curly\s*hair|curls|drills)\b", r"ウェーブ|パーマ|巻き髪|縦ロール|天然パーマ"]),
    ("braids", [r"\b(?:braids?|braided\s*hair)\b", r"三つ編み|編み込み"]),
    ("long", [r"\b(?:long\s*hair|straight\s*hair)\b", r"ロング(?:ヘア)?|ストレートロング|長髪"]),
]

_EXPRESSION_PATTERNS = [
    (
        "smile",
        [
            r"\b(?:smile|smiling|happy|cheerful|grin|laughing)\b",
            r"笑顔|微笑[みむ]|にっこり|にこにこ|笑[うった]|嬉しそう",
        ],
    ),
    ("wink", [r"\b(?:wink|winking)\b", r"ウインク|ウィンク|片目閉じ"]),
    ("sad", [r"\b(?:sad|tear|crying|sorrow|unhappy|melancholy)\b", r"悲しげ|悲しい|涙|泣く|哀愁|寂しそう"]),
    ("blush", [r"\b(?:blush|blushing|shy|embarrassed)\b", r"照れ|赤面|恥ずかしそう|はにかみ"]),
    ("serious", [r"\b(?:serious|stoic|cool|stern|calm)\b", r"真剣|キリッ|クール|真面目|凛々しい"]),
    ("angry", [r"\b(?:angry|pout|furious|annoyed)\b", r"怒り|むすっと|怒った|不機嫌"]),
    ("gentle", [r"\b(?:gentle|soft\s*smile|warm\s*expression|kind)\b", r"優しい|穏やか|柔らかな表情"]),
]

_COSTUME_PATTERNS = [
    ("sailor", [r"\b(?:sailor\s*(?:suit|uniform)?|serafuku)\b", r"セーラー服|セーラー"]),
    ("blazer", [r"\b(?:blazer|school\s*blazer)\b", r"ブレザー"]),
    (
        "school_uniform",
        [
            r"\b(?:school\s*uniform|student\s*uniform|schoolgirl|schoolboy)\b",
            r"制服|学生服|学ラン|女子高生|女子中学生|男子校生|高校生|中学生|JK",
        ],
    ),
    ("kimono", [r"\b(?:kimono|yukata|wafuku)\b", r"着物|浴衣|和服|巫女服"]),
    ("hoodie", [r"\b(?:hoodie|parka|sweatshirt)\b", r"パーカー|フーディー"]),
    ("dress", [r"\b(?:dress|one\s*piece|gown|sundress)\b", r"ドレス|ワンピース"]),
    ("suit", [r"\b(?:suit|tuxedo|formal\s*wear)\b", r"スーツ|タキシード|正装"]),
    ("armor", [r"\b(?:armor|knight|plate|cuirass)\b", r"鎧|甲冑|アーマー|騎士"]),
]

_ACCESSORY_PATTERNS = [
    ("glasses", [r"\b(?:glasses|spectacles|eyewear)\b", r"メガネ|眼鏡|めがね"]),
    ("ribbon", [r"\b(?:ribbon|hair\s*ribbon|bow)\b", r"リボン|ヘアリボン"]),
    ("cat_ears", [r"\b(?:cat\s*ears?|nekomimi)\b", r"猫耳|ネコミミ|ねこみみ"]),
    ("hairclip", [r"\b(?:hair\s*clip|hairpin|barrette)\b", r"ヘアピン|髪留め|ヘアクリップ"]),
    ("hat", [r"\b(?:hat|cap|beret|witch\s*hat)\b", r"帽子|ベレー帽|ハット|キャップ|魔女帽"]),
    ("earrings", [r"\b(?:earrings?)\b", r"イヤリング|ピアス"]),
    ("necklace", [r"\b(?:necklace|choker|pendant)\b", r"ネックレス|チョーカー|ペンダント"]),
]

_COLOR_MAPPINGS = [
    ("blonde", "#e7bd55", [r"\b(?:blonde?|yellow|golden?)\b", r"金髪|金色|ゴールド|黄色"]),
    ("black", "#292632", [r"\b(?:black|dark|raven)\b", r"黒髪|黒|漆黒"]),
    ("silver", "#e8edf5", [r"\b(?:silver|white|platinum|grey|gray)\b", r"銀髪|白髪|シルバー|白"]),
    ("pink", "#e86f9d", [r"\b(?:pink|rose)\b", r"ピンク髪|ピンク|桃色"]),
    ("blue", "#4776d0", [r"\b(?:blue|azure|cyan|navy)\b", r"青髪|青|水色|蒼"]),
    ("red", "#d94b58", [r"\b(?:red|crimson|scarlet)\b", r"赤髪|赤|紅|緋色"]),
    ("brown", "#6b4226", [r"\b(?:brown|chestnut|auburn)\b", r"茶髪|茶色|栗色"]),
    ("purple", "#8554b3", [r"\b(?:purple|violet|lavender)\b", r"紫髪|紫|パープル"]),
    ("green", "#4f9b68", [r"\b(?:green|emerald)\b", r"緑髪|緑|エメラルド"]),
]

_TIME_PATTERNS = [
    ("sunset", [r"\b(?:sunset|dusk|evening|golden\s*hour|twilight)\b", r"夕暮れ|夕焼け|夕方|黄昏|日没|茜空"]),
    ("night", [r"\b(?:night|midnight|moonlit|starlight|dark\s*sky)\b", r"夜|夜空|真夜中|月夜|星空"]),
    ("dawn", [r"\b(?:dawn|sunrise|morning)\b", r"夜明け|朝日|朝陽|早朝|朝"]),
]

_SETTING_PATTERNS = [
    ("classroom", [r"\b(?:classroom|school|blackboard|desk)\b", r"教室|学校|黒板|机"]),
    ("room", [r"\b(?:room|bedroom|interior)\b", r"部屋|室内|ベッドルーム"]),
    ("city", [r"\b(?:city|street|urban|town|alley|skyline)\b", r"都市|街並み|通り|路地|ビル街"]),
    ("nature", [r"\b(?:nature|forest|wood|woods)\b", r"自然|森|森林|木々"]),
    ("mountain", [r"\b(?:mountain|peak|ridge|cliff)\b", r"山|山岳|稜線|崖"]),
    ("sea", [r"\b(?:sea|ocean|beach|coast|shore)\b", r"海|海岸|ビーチ|砂浜|波"]),
    ("garden", [r"\b(?:garden|park|meadow)\b", r"庭|花畑|公園|草原"]),
]

_WEATHER_PATTERNS = [
    ("rain", [r"\b(?:rain|raining|rainy|drizzle)\b", r"雨|雨降り|水滴"]),
    ("snow", [r"\b(?:snow|snowing|blizzard)\b", r"雪|雪景色|吹雪|粉雪"]),
    ("sakura", [r"\b(?:sakura|cherry\s*blossom|petals?)\b", r"桜|花びら|桜吹雪"]),
    ("clouds", [r"\b(?:clouds?|cloudy|cumulus)\b", r"雲|入道雲|積乱雲|青空"]),
    ("stars", [r"\b(?:stars?|starry|constellation|galaxy)\b", r"星|星空|満天の星|天の川"]),
    ("aurora", [r"\b(?:aurora|northern\s*lights)\b", r"オーロラ"]),
]

_VIEW_ANGLE_PATTERNS = [
    ("three_quarters", [r"\b(?:three\s*quarters?|3/4|slanted|angled)\b", r"斜め|ナナメ|斜め向き|七分身"]),
    ("profile", [r"\b(?:profile|side\s*view)\b", r"横顔|プロフィール|真横"]),
    ("tilt", [r"\b(?:tilt|tilted|dynamic\s*angle)\b", r"首かしげ|傾き|アオリ|フカン"]),
]


def _match_any(patterns: list[str], text: str) -> bool:
    return any(re.search(pat, text, re.IGNORECASE) for pat in patterns)


def _extract_color_for_feature(feature_keywords: list[str], prompt: str) -> str | None:
    """指定された部位（hair, eye など）に関連付けられた色を抽出する。"""
    for color_name, hex_val, col_patterns in _COLOR_MAPPINGS:
        for feat in feature_keywords:
            for c_pat in col_patterns:
                # 英語直結 (e.g. "blonde hair")
                if re.search(rf"(?:{c_pat})\s+(?:{feat})", prompt, re.IGNORECASE):
                    return hex_val
                # 日本語直結 (e.g. "金髪", "青い瞳")
                if feat in ("hair", "髪", "前髪", "ツインテ", "ポニテ") and any(
                    k in prompt for k in (f"{color_name}髪", f"{color_name}の髪")
                ):
                    return hex_val
                if feat in ("eye", "eyes", "瞳", "目") and any(
                    k in prompt for k in (f"{color_name}の瞳", f"{color_name}目", f"{color_name}の目")
                ):
                    return hex_val
                # 近傍探索 (前後12文字以内) - c_pat と feat の両方をグループ化
                m = re.search(
                    rf"(?:(?:{c_pat}).{{0,12}}(?:{feat})|(?:{feat}).{{0,12}}(?:{c_pat}))", prompt, re.IGNORECASE
                )
                if m:
                    # ただし hair を探している時に eye の共起がある場合、あるいはその逆を二重抽出しないようガード
                    if feat in ("eye", "eyes", "瞳", "目") and any(
                        h in m.group(0) for h in ("髪", "hair", "ツインテ", "ポニテ")
                    ):
                        continue
                    return hex_val
    return None


def analyze_prompt(prompt: str) -> SemanticAttributes:
    """自然言語プロンプトを徹底解析し、構造化された描画属性を返す。"""
    normalized = prompt.strip()
    norm_lower = normalized.casefold()

    # 1. カテゴリ推定
    category = "character"
    if any(k in norm_lower for k in ("mandala", "geometry", "幾何", "マンダラ")):
        category = "geometry"
    elif any(k in norm_lower for k in ("focus lines", "speed lines", "magic circle", "集中線", "魔法陣")):
        category = "fx"
    elif any(
        k in norm_lower for k in ("cat", "dog", "dragon", "bird", "wolf", "猫", "犬", "鳥", "竜", "動物")
    ) and not any(k in norm_lower for k in ("girl", "boy", "person", "portrait", "少女", "少年", "人物")):
        category = "creature"
    elif any(
        k in norm_lower for k in ("landscape", "mountain", "ocean", "wave", "風景", "山", "海", "大波")
    ) and not any(k in norm_lower for k in ("girl", "boy", "person", "portrait", "少女", "少年", "人物")):
        category = "landscape"

    # 2. キャラクター属性抽出
    gender = "unspecified"
    for g_val, g_pats in _GENDER_PATTERNS:
        if _match_any(g_pats, normalized):
            gender = g_val
            break

    hair_style = "default"
    for h_val, h_pats in _HAIR_STYLE_PATTERNS:
        if _match_any(h_pats, normalized):
            hair_style = h_val
            break

    hair_color = _extract_color_for_feature(
        ["hair", "髪", "前髪", "ツインテ", "ポニテ", "twintail", "twintails", "ponytail", "pigtail", "braid", "bob"],
        normalized,
    )
    eye_color = _extract_color_for_feature(["eye", "eyes", "瞳", "目"], normalized)

    expression = "default"
    for exp_val, exp_pats in _EXPRESSION_PATTERNS:
        if _match_any(exp_pats, normalized):
            expression = exp_val
            break

    costume = "default"
    for cos_val, cos_pats in _COSTUME_PATTERNS:
        if _match_any(cos_pats, normalized):
            costume = cos_val
            break

    accessories: list[str] = []
    for acc_val, acc_pats in _ACCESSORY_PATTERNS:
        if _match_any(acc_pats, normalized):
            accessories.append(acc_val)

    view_angle = "auto"
    for ang_val, ang_pats in _VIEW_ANGLE_PATTERNS:
        if _match_any(ang_pats, normalized):
            view_angle = ang_val
            break

    char_attrs = CharacterAttributes(
        gender=gender,
        hair_style=hair_style,
        hair_color=hair_color,
        eye_color=eye_color,
        expression=expression,
        costume=costume,
        accessories=tuple(accessories),
        view_angle=view_angle,
    )

    # 3. 環境・時間帯・天候属性抽出
    time_of_day = "day"
    for t_val, t_pats in _TIME_PATTERNS:
        if _match_any(t_pats, normalized):
            time_of_day = t_val
            break

    setting = "none"
    for s_val, s_pats in _SETTING_PATTERNS:
        if _match_any(s_pats, normalized):
            setting = s_val
            break

    weather: list[str] = []
    for w_val, w_pats in _WEATHER_PATTERNS:
        if _match_any(w_pats, normalized):
            weather.append(w_val)

    env_attrs = EnvironmentAttributes(
        setting=setting,
        time_of_day=time_of_day,
        weather_elements=tuple(weather),
    )

    # 4. ライティング & トーン
    key_light = "natural"
    tone = "natural"
    if time_of_day == "sunset":
        tone = "sunset"
        key_light = "backlight"
    elif time_of_day == "night":
        tone = "night"
        key_light = "rim_light"
    elif any(k in norm_lower for k in ("backlight", "rim light", "逆光")):
        key_light = "backlight"

    if any(k in norm_lower for k in ("warm", "暖色", "あたたかい")):
        tone = "warm"
    elif any(k in norm_lower for k in ("cool", "寒色", "冷たい")):
        tone = "cool"
    elif any(k in norm_lower for k in ("pastel", "パステル", "ゆめかわ")):
        tone = "pastel"

    lit_attrs = LightingAttributes(
        key_light=key_light,
        tone=tone,
    )

    # 5. アートスタイル
    art_style = "anime"
    if any(k in norm_lower for k in ("watercolor", "水彩", "透明水彩")):
        art_style = "watercolor"
    elif any(k in norm_lower for k in ("manga", "monochrome", "comic", "マンガ", "漫画", "モノクロ")):
        art_style = "manga"
    elif any(k in norm_lower for k in ("cyberpunk", "cyber", "neon", "サイバー")):
        art_style = "cyberpunk"
    elif any(k in norm_lower for k in ("oil", "painterly", "厚塗り", "油絵")):
        art_style = "painterly"

    return SemanticAttributes(
        raw_prompt=normalized,
        category=category,
        character=char_attrs,
        environment=env_attrs,
        lighting=lit_attrs,
        art_style=art_style,
        explicit_tags=tuple(accessories + weather),
    )
