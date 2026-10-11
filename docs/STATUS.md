# État du projet AnyAndroid for Windows

*Mis à jour le 11 octobre 2026.*

## Le but final

**Faire tourner Android nativement sur Windows.** Une application Android (APK) doit s'ouvrir comme
un programme Windows ordinaire, dans sa propre fenêtre, sans machine virtuelle, sans émulateur et
sans noyau Linux invité, Google Play Services compris.

Pour y arriver, AnyAndroid rejoue ce que fait un téléphone :

| Couche d'Android             | Ce qu'en fait AnyAndroid                                                          |
|------------------------------|-----------------------------------------------------------------------------------|
| ART (machine virtuelle Java) | Un interpréteur Dalvik écrit en C++ (`runtime/dex-engine`)                         |
| libcore (`java.*`)           | Le **vrai** code d'AOSP, extrait d'une image système. Ses méthodes natives sont réécrites pour Windows |
| framework (`android.*`)      | Le **vrai** `framework.jar` d'AOSP. Ses méthodes natives (libandroid_runtime) sont réécrites pour Windows |
| Bibliothèques natives (.so)  | À venir (G4) : chargeur ELF, bionic sur Windows                                    |
| Services système (Binder)    | Émulés dans le processus (`Parcel` et `Binder` locaux pour commencer)               |
| Rendu, fenêtres, saisie      | Win32. Le rendu du vrai framework (Skia/HWUI) est à venir                           |
| Google Play Services         | microG d'abord, puis les GApps fournies par l'utilisateur (G5)                      |

Le principe est le même qu'avec Wine : on ne réécrit pas Android, on exécute son propre code et on
remplace uniquement la frontière avec le noyau et le matériel.

## Avancement

Les pourcentages sont des estimations de l'effort réalisé dans chaque étape. Ils ne comptent pas
le nombre de fichiers.

| Étape | Contenu                                                    | Avancement | État |
|-------|------------------------------------------------------------|-----------:|------|
| G0    | Mesure des écarts (`gaps`) sur de vrais APK                 | 100 %      | Terminé : [GApps d'Android 17](compatibility/gaps-gapps-android17.md) et [microG](compatibility/gaps-microg.md) mesurés |
| G1    | Runtime : interpréteur, threads, GC, réflexion, JNI         | 100 %      | Terminé ([ADR-003](adr/ADR-003-runtime-threads-gc-jni.md)) |
| G2    | Vraie libcore d'AOSP (Android 17)                           | 92 %       | NIO.2 et `FileChannel` sur VFS, `java.util.zip`, condensés conscrypt, mémoire native émulée ([ADR-004](adr/ADR-004-libcore-aosp.md)) |
| G3    | Vrai `framework.jar` et une application AOSP affichée       | 42 %       | **En cours** (ressources des applications en mode pont, `Resources.getSystem()` sur le vrai `framework-res.apk`) |
| G4    | Couche native ELF (bibliothèques `.so` des applications)    | 0 %        | À faire |
| G5    | Services Google : microG, puis GApps                        | 0 %        | À faire |
| **Total** | **Android natif sur Windows**                          | **≈ 38 %** | Pondéré par l'effort : G3 et G4 sont les plus lourdes |

```
G0 ██████████ 100 %
G1 ██████████ 100 %
G2 █████████░  92 %
G3 ████░░░░░░  42 %
G4 ░░░░░░░░░░   0 %
G5 ░░░░░░░░░░   0 %
   ───────────────
   ███▊░░░░░░ ≈ 38 % vers « Android natif sur Windows »
```

## Ce qui marche aujourd'hui (vérifié par 33 suites de tests et 109 tests unitaires)

- **Analyse d'APK sans rien exécuter** (`anyandroid inspect`). L'analyse est durcie contre les
  archives piégées et produit un verdict de compatibilité.
- **Applications de démonstration** affichées dans une vraie fenêtre Windows, avec cycle de vie
  Android et bouton Retour.
- **Runtime complet** : vrais threads Windows, ramasse-miettes, réflexion et JNI.
  `InvocationTargetException` garde sa cause, son message et sa pile, même construite par le
  bytecode de la libcore. `exec --profile` liste les méthodes les plus coûteuses (instructions,
  appels) ; sans l'option, les compteurs ne sont pas touchés.
- **Lecture des images système Android** (`anyandroid image`) : formats sparse, super, ext4 et
  EROFS compressé, APEX.
- **Vraie libcore d'Android 17** : collections, `Formatter`, concurrence, fichiers, jeux de
  caractères, `VarHandle`, et **NIO.2 (`sun.nio.fs`, `FileDispatcherImpl`, `FileChannel`) sur VFS**.
  - **`java.util.zip`** (`Deflater`, `Inflater`, flux GZIP, `CRC32`, `Adler32`) sur
    [miniz](../runtime/third_party/miniz/README.md) intégré, avec le comportement de zlib pour les
    vidages répétés et un nombre plafonné de flux vivants (exemple `zip-demo`).
  - **`MessageDigest`** (MD5, SHA-1, SHA-224, SHA-256, SHA-384, SHA-512) par le fournisseur
    `AndroidOpenSSL` de conscrypt, dont les natives de condensé sont réécrites (contextes en
    identifiants bornés). Pas de chiffrement ni de TLS.
  - **Mémoire native émulée** : `Unsafe.allocateMemory` et les accès par adresse,
    `libcore.io.Memory`, `ByteBuffer` directs, `Os.mmap` (anonyme, ou copie privée d'un fichier)
    et `FileChannel.map`. Les adresses sont des valeurs étiquetées vérifiées à chaque accès, jamais
    des pointeurs de l'hôte, dans un budget global de 1 Gio.
- **Système de fichiers virtuel** ([ADR-005](adr/ADR-005-systeme-de-fichiers-virtuel.md)).
  `/system` et `/apex` sont servis depuis l'image, et `/data` et `/sdcard` sont isolés dans un bac
  à sable. Le processus émulé ne voit ni les fichiers ni le nom d'utilisateur de l'hôte.
- **Vrai `framework.jar`** : `Log`, `TextUtils`, `Bundle`, `ArrayMap`, `Uri`, `Base64`, `JSONObject`,
  `Build` (identité honnête « AnyAndroid »), `Looper`/`Handler`, `SystemClock`, `Parcel` (aller-retour
  en octets d'un `Bundle` et d'un `Intent`), `Binder` local (`transact`), `SystemProperties.digestOf`
  (fournisseurs JCA) et `Resources.getSystem()` sur le vrai `framework-res.apk` (thème Material,
  `TypedArray`). L'exemple `framework-demo` les vérifie.
- **Ressources des applications en mode pont** (`anyandroid run`) : le `resources.arsc` de l'APK est
  chargé avec la configuration de l'appareil (langue de Windows ou `--locale`). `Resources`
  (`getString`, `getText`, `getColor`, `getDimension…`, `getInteger`, `getBoolean`, `getIdentifier`,
  `getResourceName`), `Context.getString(id, args…)`, `TextView.setText(int)`, `Activity.setTitle(int)`,
  `Toast.makeText(…, int, …)` et le titre `@string/` du manifeste.
- **Mises en page XML** : `LayoutInflater` gonfle `res/layout` dans les vues du pont
  (`setContentView(R.layout.x)`, `inflate`, `<include>`, `<merge>`, `android:onClick`, attributs
  courants : id, texte, indice, orientation, tailles, couleurs, poids, visibilité, activation).
  L'APK `resources-demo` le vérifie en français et en anglais.
- **Contexte** : `Context.getSystemService()` (seul `layout_inflater` en mode pont).
  `getFilesDir()`, `getCacheDir()` et `getDataDir()` restent non pris en charge : le pont ne
  définit pas `java.io.File` (ils viendront de framework.jar, ADR-002).

## Ce qui est en cours (G3)

**Les ressources Android** (`runtime/resources`). C'est l'équivalent d'AssetManager2 de
libandroidfw : sans lui, aucune application ne peut lire ses textes, ses couleurs, ses thèmes ni
ses mises en page.

- Fait :
  - lecture de `resources.arsc` : pools de chaînes UTF-8/UTF-16 et styles, entrées compactes, types
    creux, bibliothèques partagées ;
  - choix de la meilleure variante selon la configuration : langue avec script, densité, taille
    d'écran, mode nuit… avec les règles exactes d'AOSP ;
  - styles avec héritage, thèmes et résolution d'attributs de `TypedArray` ;
  - XML compilé (mises en page) ;
  - tests unitaires complets en mémoire pour `runtime/resources` (`test_resources.cpp`, 10 tests) ;
  - `apkgen build --res` compile un dossier `res/` (valeurs, mises en page, langues) en
    `resources.arsc` et XML binaire, sans SDK Android ; `--r-txt` donne les identifiants `R` ;
  - implémentation de `sun.nio.fs` sur le VFS (`libcore_nio.cpp`) débloquant les dépendances NIO.
- Vérifié sur le vrai `framework-res.apk` d'Android 17 avec la commande
  `anyandroid resources <apk> --get string/cancel --locale fr-FR` (résultat : « Annuler »), et
  par le vrai framework : la section `resources` de `framework-demo` passe par
  `Resources.getSystem()`, un thème et un `TypedArray`.
- Les natives Java correspondantes sont écrites : `AssetManager`, `ApkAssets`, `StringBlock` et
  `XmlBlock`, plus de 90 méthodes.
- En mode pont, `Context`, `Activity`, `TextView`, `Toast` et `LayoutInflater` sont branchés sur
  les ressources de l'APK (`android_res.cpp`).

## Prochaines étapes, dans l'ordre

1. `ActivityThread` : démarrer une application comme le fait Android (`LoadedApk`,
   `ContextImpl`, `Instrumentation`), avec la hiérarchie complète des services système.
2. Rendu du vrai framework : `View`, `Canvas`, `Paint`, texte. Moteur de dessin 2D
   sur Windows, à la place de Skia/HWUI.
3. **Sortie de G3** : une application AOSP de l'image (par exemple Calculatrice) affichée dans
   une fenêtre Windows.
4. G4 : chargeur ELF et bionic, pour les applications avec du code natif.
5. G5 : microG, puis les GApps fournies par l'utilisateur.

## Règles que le projet s'impose

Ces règles viennent d'[ADR-002](adr/ADR-002-objectif-gapps.md) et du
[modèle de menaces](security/threat-model.md).

- **Aucun binaire Google ni fichier extrait d'une image Android dans ce dépôt.** L'image système
  AOSP et la libcore extraite restent sur la machine de l'utilisateur.
- Pas de contournement de Play Integrity, SafetyNet ou DroidGuard. Pas d'usurpation d'empreinte
  d'appareil certifié : l'appareil se présente honnêtement comme « AnyAndroid ».
- Les GApps propriétaires sont fournies par l'utilisateur, jamais téléchargées depuis des miroirs.
- Le code Android exécuté est traité comme non fiable. Les objets natifs lui sont présentés comme
  des identifiants bornés, jamais comme des pointeurs, et ses fichiers restent dans un bac à sable.

## Entrées Android (hors dépôt)

`tools/fetch-android-inputs.sh <dossier> [--gms] [--microg]` télécharge et vérifie (SHA-256
publiés par Google, empreintes de l'index F-Droid de microG) la GSI x86_64 officielle d'Android 17
(AOSP et, avec `--gms`, la variante GMS dont il extrait les GApps), extrait la libcore avec
`anyandroid image bootclasspath`, et récupère microG. Rien de tout cela n'entre dans le dépôt.

Mesures G0 sur ces APK réels :

- [GApps d'Android 17](compatibility/gaps-gapps-android17.md) (Play Services 26.22, GSF, Play
  Store 51.8) : 24 715 API distinctes, 1,68 million d'appels ; 14 processus pour GmsCore, UID
  partagé `com.google.uid.shared`, des dizaines de permissions privilégiées, code natif x86_64 ;
- [microG 0.3.17](compatibility/gaps-microg.md) : 14 262 API distinctes, 0,4 million d'appels.

## Compiler et tester

Il faut CMake, Ninja et MinGW-w64 (GCC 15, UCRT). Le SDK Android n'est pas nécessaire : les APK de
démonstration sont générés par `tools/apkgen`.

```bash
cmake -S . -B build -G Ninja -DAA_WARNINGS_AS_ERRORS=ON
cmake --build build
cd build && ctest
```

Les tests G2 et G3 sur la vraie libcore sont facultatifs. Il faut d'abord extraire la libcore d'une
image système AOSP (GSI), hors du dépôt :

```bash
anyandroid image bootclasspath system.img --output <dossier>
cmake -S . -B build -DAA_LIBCORE_DIR=<dossier> -DAA_SYSTEM_IMAGE=<chemin>/system.img
```
