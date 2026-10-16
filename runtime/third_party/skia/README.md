# Skia

Moteur 2D de HWUI : AnyAndroid dessine les vues avec le même code qu'Android
([ADR-008](../../../docs/adr/ADR-008-moteur-2d.md)).

- **Origine** : la copie de Skia d'Android, <https://android.googlesource.com/platform/external/skia>,
  étiquette **`android-17.0.0_r1`** (fichier `METADATA` d'AOSP : Skia
  `eeadbfde3671f69a4565cc655f82f041a9bc68b5`, mise à jour du 29 mars 2026).
- **Licence** : BSD (texte dans [LICENSE](LICENSE)).
- **Modifications** : aucune. Les fichiers repris sont identiques à ceux de l'étiquette, y compris
  `include/config/SkUserConfigManual.h`, la configuration d'Android
  (`SK_BUILD_FOR_ANDROID_FRAMEWORK`, faux gras FreeType au diviseur 34, comportements hérités de
  HWUI). `aa-config/` est propre au projet : `SkUserConfig.h` (charge la configuration d'Android
  et donne `SK_ALWAYS_INLINE` à GCC sous Windows), les deux en-têtes d'Android que cette
  configuration appelle (`android/log.h` pour `SK_ABORT`, `cutils/trace.h` pour la trace système,
  jamais active) et l'implémentation de `__android_log_assert`.
- **Sous-ensemble** : le rendu CPU seulement. Unités de traduction : les listes `gn/*.gni` de Skia
  pour le cœur (`skia_core_sources`), les effets, filtres de couleur et d'image, SkSL (effets
  d'exécution), les opérations sur chemins, les utilitaires, le port FreeType
  (`SkFontHost_FreeType`, `SkFontMgr_custom` et sa variante vide, celle de HWUI), la mémoire, le
  journal et les fichiers de Windows, skcms, et la chaîne de rendu Haswell (`SkOpts_hsw.cpp`).
  Pas de GPU (Ganesh, Graphite), de codecs, d'encodeurs, de PDF, de SVG ni d'XPS. En-têtes : la
  fermeture de ces unités (`g++ -MM`), rien d'autre. Liste compilée : [sources.cmake](sources.cmake).
- **Construction** : bibliothèque statique `aa_skia` (C++20), avec le FreeType du projet.
  `SkOpts_hsw.cpp` est compilé pour Haswell et choisi à l'exécution ; GCC n'alignant pas la pile à
  32 octets sous Windows (bogue 54412), ses déplacements AVX alignés sont assemblés en déplacements
  non alignés (`-Wa,-muse-unaligned-vector-move`). Comme dans Android, skcms garde ses
  transformations de base (`SKCMS_DISABLE_HSW`, `SKCMS_DISABLE_SKX`) et la variante AVX-512 de
  Skia n'est pas compilée.

Pour mettre à jour : prendre l'étiquette d'AOSP de la nouvelle version d'Android, extraire
`include`, `src`, `gn` et `modules/skcms`, recalculer la liste des unités
(`tools/dev/gni_lists.py`) et leur fermeture d'en-têtes (`g++ -std=c++20 -MM`, avec
`aa-config` en tête des chemins d'inclusion), recopier avec `tools/dev/vendor_skia.py`, mettre à jour
ce fichier, puis relancer les tests unitaires (`skia_*`, `text_*`).
