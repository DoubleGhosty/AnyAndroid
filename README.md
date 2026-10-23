# AnyAndroid for Windows

![Avancement AnyAndroid](docs/badge-overall.svg)
![Pont Framework](docs/badge-bridge.svg)
![Natives Libcore](docs/badge-libcore.svg)

Couche de compatibilité qui exécute des applications Android (bytecode DEX) **comme des processus
Windows**, sans machine virtuelle, sans hyperviseur et sans noyau Linux invité.

> **Avancement : ≈ 50 % vers « Android natif sur Windows »**
> 📊 Détail des jalons et carte complète dans [docs/STATUS.md](docs/STATUS.md) · [Données brutes](docs/progress.json).
>
> **Statut : prototype de recherche.** La preuve de concept de l'annexe A du cadrage fonctionne :
> des APK de démonstration s'ouvrent dans une vraie fenêtre Windows. L'objectif prioritaire est
> désormais de faire fonctionner les **GApps** : voir [ADR-002](docs/adr/ADR-002-objectif-gapps.md).
> Ce n'est **pas** une alternative à Android ni à WSA : seul le sous-ensemble documenté dans la
> [matrice des API](docs/compatibility/api-matrix.md) est pris en charge.

## Ce qui fonctionne (vérifié par les tests)

- **Analyse statique d'APK** (`inspect`) sans exécution de code : ZIP durci (bombes, chevauchements,
  doublons, traversée de chemins), manifeste binaire, DEX 035 à 040, multi-DEX, bibliothèques
  natives ELF, signature v1/v2, APK éclatés et bundles `.apks`/`.xapk`, API de la plateforme
  utilisées avec leur statut, verdict de compatibilité.
- **Interpréteur Dalvik** : jeu d'instructions complet (sauf `invoke-custom`/`invoke-polymorphic`),
  exceptions, héritage et interfaces, initialisation des classes, vérification structurelle du
  bytecode, limites de pile, d'objets et d'instructions.
- **Runtime G1** ([ADR-003](docs/adr/ADR-003-runtime-threads-gc-jni.md)) :
  - vrais threads Windows (`Thread`, `synchronized`, `wait`/`notify`, `sleep`, `join`, `interrupt`)
    sous un verrou global ;
  - ramasse-miettes mark-sweep avec poignées générationnelles et `WeakReference` ;
  - réflexion `java.lang.reflect` (`InvocationTargetException` complète : cause, message, pile) ;
  - **JNI 1.6** en convention System V, prêt pour les bibliothèques Android x86_64 ;
  - profil d'exécution par méthode (`exec --profile`), sans coût quand il est désactivé.
- **Pont framework** (≈ 110 classes, ≈ 700 membres) : `java.lang`, `ArrayList`, `HashMap`,
  `Activity`, `Intent`, `Bundle`, `Log`, `Handler`, `View`, `LinearLayout`, `TextView`, `Button`,
  `EditText`, `Toast`… Toute API absente échoue explicitement et est consignée.
- **Ressources des applications** : `resources.arsc` de l'APK lu comme AssetManager2 (langue de
  Windows ou `--locale`), `getString`/`getText`/`getColor`/`getDimension…`, `setText(int)`,
  `Toast.makeText(…, int, …)`, titre `@string/` du manifeste, et **mises en page XML gonflées** par
  `LayoutInflater` (`setContentView(R.layout.x)`, `<include>`, `<merge>`, `android:onClick`).
- **Cycle de vie Android** : pile d'activités, ordre des rappels conforme, `startActivity` avec
  extras, `finish`, bouton Retour, plantage avec trace de pile.
- **Fenêtre Win32 native** (contrôles standard, DPI par écran, toasts, Échap/Alt+← = Retour) et
  **mode headless scriptable** pour les tests.
- **Analyse d'écart** (`gaps`) pour mesurer ce qui manque aux GApps sur de vrais APK.
- **G2 atteint** ([ADR-004](docs/adr/ADR-004-libcore-aosp.md)) : des programmes Java s'exécutent
  sur la **vraie libcore d'Android 17**, extraite d'une image système officielle (`image` : formats
  sparse, super, ext4, EROFS compressé, APEX, DEX 041). En mode libcore (`exec --bootclasspath`), le
  bytecode d'AOSP remplace le pont pour `java.*` ; ses méthodes natives sont implémentées pour
  Windows (`libcore natives` mesure la couverture), avec `invoke-polymorphic`/`VarHandle`, les
  chaînes compactes d'ART et les jeux de caractères standards. Collections, `Formatter`,
  `java.util.concurrent`, exceptions et **fichiers** (NIO.2 et `FileChannel` compris)
  fonctionnent : un système de fichiers virtuel
  ([ADR-005](docs/adr/ADR-005-systeme-de-fichiers-virtuel.md)) sert `/system` et `/apex` depuis
  l'image système en lecture seule, et `/data` depuis un bac à sable.
  `java.util.zip` repose sur miniz intégré ; `MessageDigest` (MD5, SHA-1, SHA-2) sur les natives de
  condensé de conscrypt, réécrites ; `Unsafe`, `libcore.io.Memory`, les `ByteBuffer` directs et
  `mmap` sur une mémoire native émulée (adresses étiquetées vérifiées à chaque accès, budget global).
  Côté `framework.jar`, `Resources.getSystem()` lit le vrai `framework-res.apk` de l'image.
- **Démarrage d'applications par le vrai framework** ([ADR-006](docs/adr/ADR-006-demarrage-applications.md),
  en cours) : vrais `PathClassLoader` sur l'APK, services système locaux (`package` par le vrai
  `PackageParser`, `activity`, `window`, `display`, `permissionmgr`, réglages persistés) et
  `java.util.regex` sur un moteur compatible ICU. Le vrai `ActivityThread` démarre l'application
  par ses propres gestionnaires (`bindApplication`, `LaunchActivityItem`, `ResumeActivityItem`) :
  `onCreate`, `onStart`, `onResume`, puis fenêtre logique (`addToDisplayAsUser`), vsync à 60 Hz,
  mesure et mise en page des vues, et **première image dessinée** par le vrai framework avec le
  Skia d'Android 17 ([ADR-008](docs/adr/ADR-008-moteur-2d.md)) : barre d'action, bouton Material,
  texte (`exec --frame-out` l'écrit en PNG, `image probe` en lit les pixels).

## Avancement récent

| Jalon (ADR-006) | État |
|---|---|
| J0-J2 — outillage Java/D8, `ServiceManager` local, vrais chargeurs de classes | fait |
| J3 — services système v1 | en cours (chaque appel manquant est tracé) |
| J4 — `Application` et activité jusqu'à `onResume`, en direct | fait |
| J5 — démarrage par les vrais gestionnaires d'`ActivityThread`, fenêtre logique | atteint : jusqu'à la première image publiée (test e2e `process-launch`) |
| J6 — texte (FreeType + HarfBuzz, logique de minikin) | fait pour l'exemple : mesure (`Paint`), `MeasuredText`, coupure (`LineBreaker`), dessin ; `TextRunShaper` et `PositionedGlyphs` ajoutés, pas encore exercés par une vraie application |
| J7 — moteur 2D logiciel (dessin des vues dans la `Surface`) | fait pour l'exemple (image vérifiée au pixel) : `SkiaCanvas` de HWUI sur le Skia d'AOSP, effets des `Paint` ; rotations 3D de `RenderNode`, `drawGlyphs` et AGSL ajoutés, pas encore exercés par une vraie application |
| J8 — fenêtre Win32 et entrées | fait : `exec --window` montre l'écran ; la souris et le clavier passent par un répartiteur d'entrées fidèle à InputDispatcher jusqu'au vrai framework (deux clics changent le libellé, BACK passe la tâche en arrière-plan comme Android 12+ : tests `process-input` et `process-back`) |
| J9 — navigation, cycle de vie complet, toasts et dialogues | en cours : **toasts faits** (service `notification` fidèle à NotificationManagerService, `ToastPresenter` du framework ; tests `process-toast` et `process-toast-end`) ; **navigation entre activités faite** (`startActivity`, `finish`, BACK, cycle de vie dans l'ordre d'Android ; tests `process-nav` et `process-nav-back`) ; fenêtres composées comme sur un téléphone (cadres par le `WindowLayout` du framework, une couche par fenêtre, arbre de couches façon SurfaceFlinger) ; dialogues encore à vérifier |
| J10 — une vraie application AOSP affichée et utilisable | à faire |

## Limites actuelles

Ressources limitées en mode pont (chaînes, couleurs, dimensions, entiers, booléens et mises en page :
pas d'images, de styles, de thèmes ni de tableaux), pas de chargeur de bibliothèques
natives ELF (JNI est prêt, le chargement des `.so` arrive en G4), conscrypt limité aux condensés
(ni chiffrement ni TLS), Binder local seulement (pas de transactions entre processus) et
services système encore partiels, annotations limitées aux annotations système (classes
internes), pas de parallélisme du code Java (verrou global), rendu limité aux contrôles standard
en mode pont, un seul processus. L'ADR-002 détaille le chemin vers les GApps.

## Construire

Prérequis : Windows 10/11 x64, CMake ≥ 3.21, Ninja et un compilateur C++20. Le projet est testé avec
MinGW-w64 GCC 15 (UCRT). MSVC n'a pas encore été testé.

```bash
cmake -S . -B build -G Ninja
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

Facultatif : avec un JDK ≥ 17 et le jar de R8 (`tools/fetch-android-inputs.sh <dossier> --r8`),
`-DAA_R8_JAR=<dossier>/tools/r8-9.5.23.jar` construit aussi le code d'hôte Java
(`runtime/android-host`, [ADR-006](docs/adr/ADR-006-demarrage-applications.md)).

Le build reconstruit les APK de démonstration depuis leurs sources texte (`samples/`, smali +
manifeste) avec l'outil `apkgen` : aucun SDK Android n'est nécessaire.

## Utiliser

```bash
build/apps/cli/anyandroid.exe inspect build/samples/basic-activity.apk
```

```bash
build/apps/cli/anyandroid.exe run build/samples/two-screens.apk
```

```bash
build/apps/cli/anyandroid.exe exec build/samples/hello-dex.apk com.anyandroid.samples.hello.Hello
```

```bash
build/apps/cli/anyandroid.exe exec build/samples/threads-demo.apk com.anyandroid.samples.threads.Main
```

```bash
build/apps/cli/anyandroid.exe gaps chemin/vers/vos-gapps.apks --markdown --output rapport-gapps.md
```

Libcore d'AOSP (G2) : extraire le chemin de classes d'amorçage d'une image système **hors du
dépôt** (les fichiers extraits ne doivent pas être redistribués), puis l'utiliser :

```bash
build/apps/cli/anyandroid.exe image bootclasspath chemin/vers/system.img --output C:/aosp/libcore
```

```bash
build/apps/cli/anyandroid.exe libcore natives --bootclasspath C:/aosp/libcore --libcore-only
```

```bash
build/apps/cli/anyandroid.exe exec --bootclasspath C:/aosp/libcore --libcore-only build/samples/hello-dex.apk com.anyandroid.samples.hello.Hello
```

Avec l'arborescence servie depuis l'image système et un bac à sable choisi :

```bash
build/apps/cli/anyandroid.exe exec --bootclasspath C:/aosp/libcore --libcore-only --system-image C:/aosp/system.img --data-dir C:/aosp/appdata build/samples/libcore-demo.apk com.anyandroid.samples.libcore.Main
```

`run` demande une confirmation avant d'exécuter le code d'un APK (`--yes` pour l'automatiser) et
refuse les APK jugés incompatibles (`--force` pour passer outre). `anyandroid help` liste toutes les
options.

| Code de sortie | Signification |
|---|---|
| 0 | succès |
| 1 | l'application a planté ou le programme a échoué |
| 2 | ligne de commande invalide ou fichier illisible |
| 3 | commande prévue mais pas encore implémentée |
| 4 | APK incompatible : lancement refusé |
| 5 | lancement non confirmé |

## Organisation du dépôt

```
apps/cli/                 CLI anyandroid (inspect, run, exec, compat, gaps, image, libcore)
runtime/common/           octets bornés, journaux JSONL, JSON, hachages, UTF-8/16
runtime/apk-inspector/    ZIP, inflate, AXML, manifeste, ELF, analyse du code
runtime/image/            images Android : sparse, super, ext4, EROFS/LZ4, APEX, chemin d'amorçage
runtime/vfs/              système de fichiers virtuel du processus Android (montages, bac à sable)
runtime/dex-engine/       format DEX, opcodes, vérification, runtime, interpréteur, classes de base
runtime/framework-bridge/ API Java/Android implémentées en natif (registre à statuts)
runtime/lifecycle/        hôte d'application et cycle de vie des activités
runtime/compat/           verdict de compatibilité, matrice des API, analyse d'écart
runtime/third_party/      code tiers repris sans modification (miniz, FreeType, HarfBuzz, Skia : « Composants tiers »)
backends/windows/         fenêtre Win32 ; backends/headless/ : backend scriptable
tools/apkgen/             assembleur smali, compilateur AXML et res/ (resources.arsc), écriture ZIP/DEX
samples/                  APK de démonstration (sources smali, code Java équivalent en commentaire)
tests/                    tests unitaires, de bout en bout (sorties de référence) et fuzzing
docs/                     ADR, matrice des API, modèle de menace
```

## Documentation

- [ADR-001 — Architecture du MVP](docs/adr/ADR-001-architecture-runtime.md)
- [ADR-002 — Objectif GApps](docs/adr/ADR-002-objectif-gapps.md)
- [ADR-003 — Threads, ramasse-miettes, réflexion et JNI](docs/adr/ADR-003-runtime-threads-gc-jni.md)
- [ADR-004 — La libcore d'AOSP sur la VM (G2)](docs/adr/ADR-004-libcore-aosp.md)
- [ADR-005 — Système de fichiers virtuel](docs/adr/ADR-005-systeme-de-fichiers-virtuel.md)
- [ADR-006 — Démarrage des applications par le vrai framework](docs/adr/ADR-006-demarrage-applications.md)
- [ADR-007 — Moteur de texte](docs/adr/ADR-007-moteur-de-texte.md)
- [ADR-008 — Moteur 2D](docs/adr/ADR-008-moteur-2d.md)
- [Matrice des API](docs/compatibility/api-matrix.md) (générée par `anyandroid compat --markdown`)
- [Modèle de menace](docs/security/threat-model.md) · [SECURITY.md](SECURITY.md) · [CONTRIBUTING.md](CONTRIBUTING.md)

## Composants tiers

- [miniz](https://github.com/richgel999/miniz) 3.1.2 (+ 12 commits, `2948eeb`), licence MIT :
  deflate/inflate, CRC-32 et Adler-32 des natives de `java.util.zip`. Fichiers repris sans
  modification dans [runtime/third_party/miniz](runtime/third_party/miniz/README.md), avec leur
  [licence](runtime/third_party/miniz/LICENSE).
- [FreeType](https://freetype.org/) 2.13.2, tel qu'Android le livre (miroir AOSP
  `external/freetype`, `ba86f9b`), licence FTL : métriques, avances et contours des glyphes.
  Modules utiles seulement, sans modification, dans
  [runtime/third_party/freetype](runtime/third_party/freetype/README.md) ([licence](runtime/third_party/freetype/docs/FTL.TXT)).
- [HarfBuzz](https://harfbuzz.github.io/) 10.2.0 (version d'AOSP `external/harfbuzz_ng`), licence
  « Old MIT » : mise en forme du texte. Fichiers de la compilation unique `harfbuzz.cc`, sans
  modification, dans [runtime/third_party/harfbuzz](runtime/third_party/harfbuzz/README.md)
  ([licence](runtime/third_party/harfbuzz/COPYING)).
- [Skia](https://skia.org/), la copie d'Android 17 (AOSP `external/skia`, étiquette
  `android-17.0.0_r1`), licence BSD : moteur 2D du dessin des vues. Rendu CPU seulement, sans
  modification, dans [runtime/third_party/skia](runtime/third_party/skia/README.md)
  ([licence](runtime/third_party/skia/LICENSE)).

## Licence et marques

Licence du projet **à décider** (ADR-002, décision D3 ; Apache-2.0 suggérée). Android est une
marque de Google LLC et Windows une marque de Microsoft. AnyAndroid n'est affilié à aucune de ces
sociétés ni certifié par elles. Le dépôt ne contient et ne distribue aucun binaire propriétaire.
