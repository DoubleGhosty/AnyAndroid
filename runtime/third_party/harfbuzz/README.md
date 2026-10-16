# HarfBuzz

Mise en forme du texte (OpenType `GSUB`/`GPOS`, écritures complexes) pour le moteur de texte
([ADR-007](../../../docs/adr/ADR-007-moteur-de-texte.md)), comme minikin dans Android.

- **Origine** : <https://github.com/harfbuzz/harfbuzz>, version **10.2.0**, celle d'Android
  (`external/harfbuzz_ng`, fichier `METADATA`). Archive de publication
  `harfbuzz-10.2.0.tar.xz`, SHA-256
  `620e3468faec2ea8685d32c46a58469b850ef63040b3565cde05959825b48227`.
- **Licence** : « Old MIT », texte dans [COPYING](COPYING).
- **Modifications** : aucune. Seuls les fichiers de `src/` qu'inclut la compilation unique
  `src/harfbuzz.cc` sont repris (liste obtenue par `g++ -MM src/harfbuzz.cc`) : pas de
  sous-ensemble (`hb-subset`), d'outils, de tests ni d'intégrations (Cairo, GLib, ICU…).
- **Construction** : `src/harfbuzz.cc` seul (bibliothèque statique `aa_harfbuzz`), avec les
  fonctions de police OpenType et les données Unicode intégrées de HarfBuzz (`hb-ot`, `hb-ucd`),
  sans FreeType : comme minikin, le moteur fournit lui-même les avances des glyphes.

Pour mettre à jour : prendre la version d'AOSP, recopier les fichiers donnés par
`g++ -std=c++17 -MM src/harfbuzz.cc` (chemins relatifs), mettre à jour ce fichier, puis relancer
les tests unitaires du moteur de texte.
