# ADR-008 — Moteur 2D

- **Statut** : accepté le 2026-10-16 (G3, jalon J7 de l'ADR-006).
- **Contexte** : le démarrage par les vrais gestionnaires (J5) va jusqu'à `drawSoftware` :
  `Surface.lockCanvas` donne un `Canvas` sur le tampon de la fenêtre et `DecorView.draw` appelle
  les natives de dessin (`BaseCanvas.nDrawRect` pour le fond, puis texte, chemins, bitmaps,
  9-patchs, dégradés…). Dans Android, ces natives sont celles de HWUI : `SkiaCanvas`, une mince
  couche sur un `SkCanvas` de Skia. Le `Canvas` du pont ne gardait jusqu'ici que son état
  (matrice, découpe), sans pixels. Il faut un moteur 2D logiciel qui dessine comme Android :
  mêmes couvertures d'anticrénelage, mêmes mélanges, mêmes filtres de bitmaps, mêmes glyphes.

## Décision

1. **Skia lui-même**, la copie d'AOSP à l'étiquette `android-17.0.0_r1`, intégrée sous
   `runtime/third_party/skia` sans modification et réduite au rendu CPU : cœur, effets, shaders,
   filtres d'image, SkSL (effets d'exécution, `RuntimeShader`), opérations sur chemins,
   utilitaires, port FreeType, skcms. Ni GPU, ni codecs, ni PDF/SVG. Environ 500 unités de
   traduction et 1 100 en-têtes (13 Mo), obtenus par les listes `gn/*.gni` de Skia et la fermeture
   des inclusions.
2. **La configuration d'Android** (`SkUserConfigManual.h`, `SK_BUILD_FOR_ANDROID_FRAMEWORK`) : elle
   fixe des comportements visibles (faux gras FreeType au diviseur 34, `FT_LOAD_NO_AUTOHINT`, ordre
   hérité des matrices locales des shaders, `SkCamera` d'Android). Deux en-têtes d'Android sont
   remplacés par des substituts (`android/log.h` pour `SK_ABORT`, `cutils/trace.h`).
3. **Les natives de dessin reprennent `SkiaCanvas` de HWUI** dans le pont : chaque native de
   `Canvas`/`BaseCanvas` devient l'appel `SkCanvas` qu'en fait HWUI ; la `Paint` du pont donne
   `SkPaint` et `SkFont` comme `Paint` de HWUI ; le texte passe par la mise en page du moteur de
   texte (minikin, ADR-007) puis `SkCanvas::drawGlyphs`, avec des `SkTypeface` FreeType créées par
   `SkFontMgr_New_Custom_Empty()` (le `FreeTypeFontMgr` de HWUI) ; les bitmaps et les surfaces de
   fenêtre sont des `SkBitmap` posés sur la mémoire des pixels du pont.
4. **Rendu logiciel seulement** (ADR-006, §3) : `drawSoftware` ; `RenderNode` reste un magasin de
   propriétés et l'accélération matérielle (listes d'affichage, `ThreadedRenderer`) est hors G3.

## Alternatives écartées

- **Un rastériseur maison** reprenant les algorithmes de Skia (conversion de balayage
  anticrénelée, contours, mélanges, filtrage, dégradés, flous, effets de chemin) : des dizaines de
  milliers de lignes à réécrire, sans jamais garantir les mêmes pixels.
- **Blend2D, Cairo, ThorVG, AGG** : d'autres couvertures et d'autres règles de mélange ou de
  filtrage, et un adaptateur à écrire pour chaque notion de Skia absente (filtres de couleur,
  `RuntimeShader`, `SkCamera`, lattices…).
- **Direct2D** : propre à Windows, autres arrondis ; les tests sous wine ne refléteraient pas le
  rendu de Windows.

## Conséquences

- Un composant tiers de plus (BSD, cité dans le README) ; sa mise à jour suit les versions
  d'Android (`tools/dev/vendor_skia.py`). La construction prend environ quatre minutes de plus sur
  quatre cœurs.
- GCC sous Windows : `SK_ALWAYS_INLINE` est défini pour GCC (Skia suppose `__forceinline` de MSVC),
  et la chaîne Haswell (`SkOpts_hsw.cpp`, choisie à l'exécution) est assemblée avec des
  déplacements AVX non alignés, GCC n'alignant pas la pile à 32 octets (bogue 54412). Comme
  upstream, les fonctions en ligne compilées dans cette unité peuvent porter des instructions AVX2 :
  un processeur sans AVX2 (antérieur à 2013) n'est pas visé.
- Le moteur de texte garde sa mesure (port du scaler FreeType de Skia, ADR-007) ; Skia dessine avec
  son propre scaler. Un test unitaire vérifie que les deux donnent les mêmes avances ; la mesure
  pourra passer à Skia si elles divergent un jour.
- `N32` est configuré en RGBA (`SK_R32_SHIFT` 0, l'ordre d'Android ; Skia choisirait BGRA sous
  Windows) : les pixels des bitmaps et des tampons de fenêtre ont les octets qu'Android donne aux
  applications (`copyPixelsToBuffer`), et la présentation dans une fenêtre Win32 (J8) les
  réordonne.
- Les natives d'état de `Canvas` (matrice, découpe) sont passées sur `SkCanvas`. Celles de `Path`,
  `Matrix` et `Region` gardent leur modèle (formules de Skia réécrites) et sont converties en
  `SkPath`, `SkMatrix` et `SkRegion` au moment de dessiner ; elles pourront passer sur Skia
  elles aussi.
- Les effets des `Paint` (`Shader`, `ColorFilter`, `MaskFilter`, `PathEffect`) sont des objets
  Skia ; la `Paint` en prend une référence quand on les pose, comme celle de HWUI. Les effets AGSL
  (`RuntimeShader`, `RuntimeColorFilter`, `RuntimeXfermode`) restent à brancher sur SkSL.
