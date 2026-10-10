# miniz

Compression deflate/inflate (flux brut ou zlib), CRC-32 et Adler-32 utilisés par les natives de
`java.util.zip` (`Inflater`, `Deflater`, `CRC32`, `Adler32`) dans
[`runtime/framework-bridge/libcore_zip.cpp`](../../framework-bridge/libcore_zip.cpp).

- **Origine** : <https://github.com/richgel999/miniz>, commit
  `2948eebafc5b9f1c69bebed80333e403a09bd62d` (`git describe` : `3.1.2-12-g2948eeb`, soit la
  version 3.1.2 plus 12 commits).
- **Licence** : MIT, texte d'origine dans [LICENSE](LICENSE) (aussi en tête de `miniz.c`).
- **Modifications** : aucune. Les fichiers repris sont identiques à ceux du commit ci-dessus.
  Seul `miniz_export.h` est propre au projet : en amont, CMake le génère
  (`GenerateExportHeader`) ; ici il définit `MINIZ_EXPORT` vide (bibliothèque statique).
- **Fichiers compilés** : `miniz.c`, `miniz_tdef.c` et `miniz_tinfl.c` (bibliothèque statique
  `aa_miniz`), avec `MINIZ_NO_ARCHIVE_APIS`, `MINIZ_NO_STDIO`, `MINIZ_NO_TIME` et
  `MINIZ_NO_ZLIB_COMPATIBLE_NAMES`. L'API d'archives ZIP n'est pas utilisée : `miniz_zip.c`
  n'est pas repris, et `miniz_zip.h` n'est conservé que parce que `miniz.h` l'inclut.

Pour mettre à jour : remplacer les fichiers par ceux d'un nouveau commit amont (sans
`miniz_zip.c`), mettre à jour ce fichier et `LICENSE`, puis relancer les tests unitaires et les
tests `libcore-*` facultatifs.
