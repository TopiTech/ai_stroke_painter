# V9 Atomic Ink baseline

S0 計測の錨。V9 実装後の回帰は `KisAiAtomicInkTest` と
`KisAiQualityBenchGateTest::testAtomicInkRatioOnPortrait` /
`testEyeSymmetryWarningsOnPortrait` で機械検証する。

代表プロンプト:

1. 黒髪ショートボブの少女、窓辺で微笑む (anime_lineart_heavy)
2. 夕暮れの桜並木と石畳の坂道 (watercolor_soft)
3. 甲冑を纏った騎士、剣を地に突き刺して佇む (ink_sketch_bold)

受け入れ:

- 複合 Kind の直接ラスタが 0 (`atomicStrokeRatio == 1.0`)
- 顔プロンプトの目対称警告が 0
- ゴールデン 32 の aggregate 非悪化 (`testBenchmarkFullGatePass`)
