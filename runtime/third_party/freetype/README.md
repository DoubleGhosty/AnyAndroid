# FreeType

Lecture des polices sfnt (TrueType, CFF, bitmaps intégrés), métriques, avances et contours des
glyphes, et rastérisation anticrénelée, pour le moteur de texte
([ADR-007](../../../docs/adr/ADR-007-moteur-de-texte.md)). Android mesure le texte avec Skia sur
FreeType : reprendre la même version, avec la même configuration, donne les mêmes avances
(indication TrueType comprise).

- **Origine** : la copie de FreeType d'Android, <https://android.googlesource.com/platform/external/freetype>,
  commit `ba86f9bbb8901de353e0d07814974cf3044c1cab` (branche `main`), soit FreeType **2.13.2**
  (fichier `METADATA` d'AOSP). L'archive du commit a pour SHA-256
  `70322f53e2b6cf939fcf12ecee64f1b0a509491431cc5594f5ca24e00caa7c0c`.
- **Licence** : FreeType License (FTL), choisie parmi les deux licences de FreeType (FTL ou GPLv2) ;
  textes dans [LICENSE.TXT](LICENSE.TXT) et [docs/FTL.TXT](docs/FTL.TXT). La FTL demande de citer
  FreeType dans la documentation : c'est fait dans le README du projet (« Composants tiers »).
- **Modifications** : aucune. Les fichiers repris sont identiques à ceux du commit ci-dessus, y
  compris `include/freetype/config/ftoption.h`, qui porte la configuration d'Android
  (interpréteur TrueType v35, sous-pixel désactivé, polices variables, bitmaps intégrés).
  `aa-config/aa_ftmodule.h` est propre au projet : il remplace la liste des modules
  (`FT_CONFIG_MODULES_H`) par ceux qui sont compilés.
- **Fichiers repris** : `include/` en entier, et dans `src/` les modules `base`, `sfnt`,
  `truetype`, `cff`, `psaux`, `pshinter`, `psnames` et `smooth`, sans les fichiers de
  construction (`module.mk`, `rules.mk`, `Jamfile`, `*.rc`) ni, dans `src/base`, les fichiers des
  formats non compilés (`ftbdf.c`, `ftcid.c`, `ftgxval.c`, `ftotval.c`, `ftpfr.c`, `ftwinfnt.c`,
  `ftpatent.c`). `fttype1.c` est repris pour Skia (`FT_Get_PS_Font_Info`, appelé par son port
  FreeType). L'auto-hinter (`autofit`) n'est pas repris : HWUI charge les glyphes avec
  `FT_LOAD_NO_AUTOHINT`.
- **Fichiers compilés** : voir [CMakeLists.txt](CMakeLists.txt) (bibliothèque statique
  `aa_freetype`).

Pour mettre à jour : reprendre les mêmes dossiers d'un nouveau commit d'AOSP (la version qu'Android
livre, pour garder ses avances), relire `ftoption.h`, mettre à jour ce fichier, puis relancer les
tests unitaires du moteur de texte.
