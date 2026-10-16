# ADR-007 — Moteur de texte

- **Statut** : accepté le 2026-10-16 (G3, jalon J6 de l'ADR-006). Le moteur 2D (rendu des
  glyphes et du reste, J7) fera l'objet d'un ADR distinct.
- **Contexte** : le démarrage par les vrais gestionnaires (J5) atteint la première traversée de
  `ViewRootImpl`. Dès la mesure du premier `TextView`, le framework appelle les natives de mesure
  du texte : `Paint.nGetFontMetricsInt`, `nGetRunAdvance`, `nGetTextAdvances`…, puis celles de
  `MeasuredText` et de `LineBreaker` pour `StaticLayout`. Sans elles, aucune vue avec du texte ne
  se mesure. Les polices sont celles de l'image (`/system/fonts`, décrites par
  `/system/etc/font_fallback.xml`) : Roboto y est une police variable (`wght`, `wdth`, `ital`) sans
  instructions d'indication, Noto couvre les autres écritures (TrueType, CFF, collections `.ttc`,
  emoji en bitmaps `CBDT`).

## Comment Android mesure le texte

Dans HWUI, minikin choisit pour chaque caractère la famille de la collection qui le couvre (score
de couverture, de langue et de variante), découpe le texte en mots et met chaque mot en forme avec
HarfBuzz. L'avance de chaque glyphe ne vient pas de HarfBuzz mais de Skia (`SkFont::getWidths`),
c'est-à-dire de FreeType chargé avec l'indication demandée par la `Paint` : par défaut
`FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_AUTOHINT`, d'où des avances arrondies au pixel ; sans
indication (texte incliné, `HINTING_OFF`, très grandes tailles), les avances linéaires. Les
métriques verticales (`ascent`, `descent`, `top`, `bottom`, `leading`) viennent du scaler FreeType
de Skia (`hhea`, ou `OS/2` typographique si `USE_TYPO_METRICS`). HarfBuzz ajoute le crénage et les
substitutions `GPOS`/`GSUB`, à l'échelle `taille × 256`.

## Décision

1. **Les mêmes composants qu'Android**, intégrés au dépôt sous `runtime/third_party`, sans
   modification et réduits aux fichiers utiles :
   - **FreeType 2.13.2**, la copie d'AOSP avec sa configuration (`ftoption.h` : interpréteur
     TrueType v35, polices variables, bitmaps intégrés) ; sans l'auto-hinter, que HWUI n'utilise
     pas ;
   - **HarfBuzz 10.2.0**, la version d'AOSP, avec ses fonctions de police OpenType et ses données
     Unicode intégrées (pas d'ICU4C).
2. **La logique de minikin et du scaler FreeType de Skia est réécrite en C++** d'après le code
   d'AOSP (Apache 2.0 et BSD), et non reprise telle quelle : minikin dépend d'ICU4C, de Skia et de
   `libutils`. Sont reproduits : le choix des chargements FreeType (`load flags`, matrice et taille
   de Skia, canonicalisation au-delà de 256 px), les métriques de police, les avances et bornes des
   glyphes, la sélection de famille et les « fakery » (gras et italique synthétiques, axes de
   variation), la mise en forme par mots (`LayoutSplitter`), l'espacement des lettres et des mots.
3. **Unicode** : propriétés et écritures par HarfBuzz (`hb-ucd`) ; ce qui demande ICU au-delà
   (bidirectionnel, coupures de mots et de lignes, propriétés emoji) passe par l'ICU4J de la
   libcore (`android.icu`), déjà chargé par la VM, avec un chemin direct pour le cas courant
   (texte de gauche à droite).
4. **Données de police** : les octets d'une police sont ceux que Java a projetés (tampon direct
   de `Font.Builder`). Le moteur les partage avec la région de mémoire native au premier usage
   (pas de copie pour une projection en lecture seule) ; ils restent valides tant qu'une police
   les utilise.
5. **Concurrence** : FreeType n'est pas réentrant sur une même face ; un verrou unique protège le
   moteur (le texte se mesure surtout sur le thread principal).
6. **Coupure de lignes** : `MeasuredText`, `WordBreaker`, les deux algorithmes de minikin
   (glouton pour `BREAK_STRATEGY_SIMPLE` et le texte à tabulations, optimal pour `HIGH_QUALITY` et
   `BALANCED`) et `Hyphenator` sont portés avec leurs types numériques, pour que les mêmes lignes
   sortent. L'itérateur de lignes d'ICU (`BreakIterator.getLineInstance` avec les options
   `-u-lb-`/`-u-lw-`) donne les frontières d'un paragraphe en une passe, mises en cache, que le
   port parcourt comme `ubrk_following`/`ubrk_next`. Les mots déjà mis en forme sont gardés dans
   un cache de 5 000 entrées (le `LayoutCache` de minikin), qui remplace aussi les `LayoutPieces`
   que minikin garde dans chaque `MeasuredText`.

## Alternatives écartées

- **Analyse sfnt maison** (cmap, hmtx, kern) : sans l'indication de FreeType, chaque avance
  diffère jusqu'à un pixel de celle d'Android ; les mises en page divergeraient.
- **DirectWrite** : propre à Windows, autres arrondis et autres métriques ; les tests sous wine ne
  refléteraient pas Windows.
- **minikin tel quel** : demande ICU4C (plusieurs dizaines de Mo de données) et une couche Skia.

## Conséquences

- Deux composants tiers de plus (licences FTL et « Old MIT », citées dans le README). Les polices
  des applications sont des données non fiables : FreeType et HarfBuzz sont fuzzés en continu
  (OSS-Fuzz) ; les mises à jour suivent les versions d'AOSP.
- Le rendu des glyphes (J7) pourra s'appuyer sur les contours et le rastériseur de FreeType.
- Césure : les motifs (`/system/usr/hyphen-data/*.hyb`) ne sont chargés que par
  `Hyphenator.init()`, que le zygote appelle et que notre démarrage n'appelle pas encore ; seuls les
  traits d'union et les tirets conditionnels (U+00AD) coupent donc un mot. `TextView` n'active pas
  la césure par défaut depuis Android 13.
- Tests : un générateur de police TrueType minimale dans les tests unitaires vérifie métriques,
  avances, indication et mise en forme sans fichier extrait d'une image ; les mesures sur les
  polices de l'image se vérifient par les tests facultatifs sur image.
