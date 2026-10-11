# ADR-005 — Système de fichiers virtuel du processus Android

- **Statut** : accepté et livré le 2026-10-10 (suite de G2, ADR-004).
- **Contexte** : la libcore d'AOSP accède aux fichiers par les natives de `libcore.io.Linux`
  (`open`, `read`, `stat`…) et de `java.io.UnixFileSystem`. Une application et, plus tard, le
  framework (G3) lisent `/system`, `/apex/*` et écrivent dans `/data/data/<paquet>`. Le modèle de
  menace interdisait jusqu'ici tout accès aux fichiers (M7).

## Décision

Le processus émulé voit une **arborescence Android virtuelle** (`runtime/vfs`), jamais un chemin de
l'hôte. Elle est assemblée de montages, choisis par préfixe le plus long :

| Chemin Android | Source | Accès |
|---|---|---|
| `/` | image système (*system-as-root*) lue directement (ext4/EROFS), ou à défaut la libcore extraite | lecture seule |
| `/apex/<module>` | APEX de `/system/apex` ouverts à la demande (charge utile en mémoire) | lecture seule |
| `/data/data/<paquet>`, `/data/user/0/<paquet>` | `<données>/<paquet>/data` | lecture/écriture |
| `/data/user_de/0/<paquet>` | `<données>/<paquet>/data_de` | lecture/écriture |
| `/data/local/tmp` | `<données>/<paquet>/tmp` | lecture/écriture |
| `/sdcard`, `/storage/emulated/0` | `<données>/<paquet>/sdcard` | lecture/écriture |
| `/dev` | `null`, `zero`, `random`, `urandom` (générateur cryptographique du système) | émulé |

`<données>` est donné par `--data-dir`, sinon le dossier `appdata` voisin de la libcore extraite
(`D:\AnyAndroid\donnees\appdata` sur le poste de développement). `--system-image` sert la base
depuis l'image système ; sans lui, la libcore extraite (`image bootclasspath`) sert de racine.

Règles :

- **Chemins** normalisés lexicalement (`.`, `..`, `//`) avant toute résolution : `..` ne remonte
  jamais au-dessus de la racine virtuelle. Longueur et noms bornés (4 096, 255), NUL refusé.
- **Liens symboliques** résolus au niveau du VFS (40 niveaux au plus), donc ils peuvent traverser
  les montages (`/sdcard` de l'image vers le bac à sable). L'application ne peut pas en créer
  (`symlink`, `link` : `EPERM`).
- **Bac à sable hôte** : les noms Linux interdits sous Windows (`:`, `?`, `*`, noms réservés comme
  `CON` ou `NUL`, point ou espace final, `%`) sont encodés `%XX` de façon réversible ; tout point
  d'analyse de l'hôte (lien, jonction) est refusé (`EACCES`), jamais traversé. Les fichiers sont
  ouverts avec `FILE_FLAG_OPEN_REPARSE_POINT`.
- **Erreurs** : valeurs `errno` Linux, converties en `android.system.ErrnoException` ; un nom de
  l'hôte n'apparaît jamais dans un message.
- **Descripteurs** : 0 à 2 sont les flux standard de l'hôte ; les autres (à partir de 3, 1 024 au
  plus) sont des descripteurs du VFS. Le parent d'un point de montage existe virtuellement
  (`/data/local` pour `/data/local/tmp`).

Natives couvertes : `open`, `close`, `read`/`readNoThrow`/`pread`, `write`/`pwrite`, `lseek`,
`fstat`/`stat`/`lstat`, `access`, `mkdir`, `unlink`/`remove`/`rename`, `readlink`, `realpath`,
`fsync`/`fdatasync`, `ftruncate`, `posix_fallocate`, `statvfs`/`fstatvfs`, `fcntl` (lecture des
drapeaux), attributs étendus (aucun), droits et propriétaires (sans effet) ; `UnixFileSystem`
(`java.io.File`) ; `FileInputStream` (`available`, `skip`, `position`, `length`) ; `Console`.

## Limites

- `readv`/`writev` : pas encore pris en charge (échec explicite). `mmap` et `FileChannel.map`
  donnent une copie privée du fichier (`MAP_SHARED` non émulé : les écritures ne reviennent pas au
  fichier) ; les `ByteBuffer` directs reposent sur la mémoire native émulée.
- Pas de verrous de fichiers, pas de droits Unix réels, système de fichiers de l'hôte insensible à
  la casse (`A` et `a` désignent le même fichier dans le bac à sable).
- L'entrée standard est lue de façon bloquante sans relâcher le verrou global.
- Chaque application a son propre bac à sable ; le partage entre applications (UID partagé, G5)
  n'est pas traité.

## Tests

`tests/unit/test_image.cpp` : normalisation et encodage des noms, bac à sable (écriture, ajout,
lecture et écriture positionnelles, troncature, répertoires, renommage entre montages refusé,
`..` contenu), `/dev`, image système en lecture seule, liens internes et vers un montage
inscriptible, boucle de liens, module APEX ouvert à la demande, points de montage listés.
Bout en bout : `libcore-demo` écrit, relit, liste et supprime un fichier, et lit les données ICU du
module i18n, avec la libcore extraite comme racine et avec l'image système (`libcore-demo-image`).
