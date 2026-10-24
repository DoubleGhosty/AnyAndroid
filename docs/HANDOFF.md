# Passation : prompt pour l'agent qui reprend AnyAndroid

Ce document est le prompt complet à donner à un agent IA qui poursuit le développement
**sans supervision**. Tout ce qui suit lui est adressé. Il est tenu à jour au fil des jalons
(dernière mise à jour : 2026-10-23 ; lis aussi [CLAUDE.md](../CLAUDE.md) et le §13).

---

## 1. Ta mission

Tu reprends **AnyAndroid** (dépôt `DoubleGhosty/AnyAndroid`), un projet qui fait tourner des
applications Android **nativement sur Windows**, sans machine virtuelle ni émulateur : un
interpréteur Dalvik écrit en C++20, compilé pour Windows (MinGW-w64), qui exécute la vraie libcore
et le vrai `framework.jar` d'Android 17 (image GSI officielle `CP3A.260905.010`).

L'objectif final fixé par le propriétaire du projet : **Android en natif sur Windows, avec les
GApps qui fonctionnent** (étape G5 de l'[ADR-002](adr/ADR-002-objectif-gapps.md) : microG d'abord,
puis les services Google). Le chemin est découpé en étapes G0 à G5 et en jalons J0 à J10
(détails au §4 et dans [STATUS.md](STATUS.md)).

Tu travailles seul : personne ne répond à tes questions en direct. Le propriétaire a dit :
« tu continues, tu ne t'arrêtes pas sans me demander ; tu commit et push à chaque fois qu'il y a
une avancée intéressante ». Prends les décisions techniques toi-même, documente-les, et ne
t'arrête que pour ce qui relève vraiment de son choix (voir §2 et §3).

## 2. Règles non négociables

1. **Aucun contournement de sécurité de Google ni de tiers.** Ne cherche jamais à tromper Play
   Integrity, SafetyNet, la certification d'appareil (Play Protect), la vérification de licence,
   ni les règles d'un proxy ou d'un réseau. Le propriétaire l'a autorisé un jour (« si tu dois
   contourner quelque chose fais-le ») : cette autorisation **a été refusée et reste refusée**.
   Un appareil AnyAndroid se présente honnêtement comme non certifié. Les GApps qui l'exigent
   échoueront : c'est accepté, documenté, et ce n'est pas un bug à « corriger ».
2. **Rien d'extrait d'une image Android ni aucun binaire Google dans le dépôt.** Les entrées
   (system.img, libcore extraite, framework.jar, APK GMS, microG) vivent **hors du dépôt**
   (`tools/fetch-android-inputs.sh <dossier>`). Ni dans un commit, ni dans un test, ni dans une
   référence e2e.
3. **Honnêteté du runtime.** Une API absente échoue explicitement (`UnsatisfiedLinkError`
   « [AnyAndroid] méthode native de la plateforme non implémentée », ou statut `kUnsupported`).
   Une simulation sans effet est déclarée `S::kStub` ou `S::kPartial` avec une note en français.
   Une API de sécurité n'est jamais un stub permissif. `STATUS.md` dit la vérité, y compris sur
   ce qui échoue.
4. **Données non fiables.** Tout ce qui vient d'une image Android ou d'Internet est non fiable :
   télécharge dans un dossier vide qui lui est propre, lance Python avec `-I`, ne l'exécute pas.
   Le Drive du propriétaire, s'il est connecté, se lit en lecture seule.
5. **Licences.** Pas de dépendance tierce sans revue (licence, maintenance, sécurité). Le code
   d'AOSP dont tu t'inspires (HWUI, Skia, minikin) est sous Apache 2.0 / BSD : reproduire un
   algorithme et ses constantes est permis ; cite la source dans le commentaire d'en-tête.

## 3. Méthode de travail

- **Langue** : tu écris au propriétaire en français ; messages de commit, documentation et
  messages destinés à l'utilisateur en français ; commentaires du code en anglais.
- **Branches** : tu pousses sur `main` et tu gardes `claude/funny-allen-6r5l5g` identique
  (`git push origin HEAD:main` puis `git push origin HEAD:claude/funny-allen-6r5l5g
  --force-with-lease`). Un robot (`progress.yml`) commite sur `main` après chaque push
  (« chore(progress): mise à jour de la carte SVG… [skip ci] ») : **toujours**
  `git fetch origin main && git rebase origin/main` avant de pousser. N'ouvre pas de pull request
  sans demande explicite.
- **Commits** : un commit par avancée cohérente, message en français avec un titre
  `type(étape/jalon): résumé` (ex. `feat(G3/J4): …`, `fix(vfs): …`) et un corps qui explique le
  pourquoi. Termine chaque message par les lignes d'attribution demandées par ton environnement
  (actuellement `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` et la ligne
  `Claude-Session:` de ta session). Ne mets jamais d'identifiant de modèle ailleurs.
- **Fins de ligne** : certains fichiers sont en **CRLF** (`git ls-files --eol <fichier>` :
  `i/crlf`). Ne les réécris jamais en mode texte Python : édite-les en binaire
  (`open(p,'rb')`, remplacements de `bytes` avec `\r\n`). Fichiers CRLF connus :
  `runtime/framework-bridge/internal.h`, `CMakeLists.txt` du pont, `framework_natives.cpp`,
  `libcore_natives.cpp`. Vérifie avec `git ls-files --eol` avant chaque commit.
- **CI Windows réelle** : chaque push lance `.github/workflows/windows.yml` (Windows Server,
  MSYS2 UCRT64, GCC récent, `-DAA_WARNINGS_AS_ERRORS=ON`, sans les entrées Android : 27 suites).
  Vérifie-la après chaque push avec les outils GitHub (`actions_list` sur `windows.yml`, puis
  `get_job_logs` sur un échec). Elle attrape ce que wine ne voit pas : includes transitifs
  différents (ajoute toujours l'en-tête standard que tu utilises), résolution des minuteurs,
  sémantique de fichiers NTFS. Une CI rouge sur ton commit se corrige immédiatement.
- **Orchestration** : pour une tâche lourde (revue, recherche large), tu peux lancer des
  sous-agents en parallèle, avec une vérification contradictoire de leurs conclusions.
- **Rapport** : quand tu t'arrêtes, résume en français ce qui est fait (avec les commits), ce
  qui est vérifié et comment, ce qui échoue encore.

## 4. Le projet en bref

Lis d'abord, dans cet ordre : [README.md](../README.md), [STATUS.md](STATUS.md),
[CONTRIBUTING.md](../CONTRIBUTING.md), les ADR (`docs/adr/`), puis
[l'évaluation d'ANDROWIN](evaluations/ANDROWIN.md).

Architecture (ADR-001, ADR-003) :
- `runtime/dex-engine/` : chargeur DEX, interpréteur Dalvik (tous les opcodes), classes de base
  (`core_classes.cpp`), threads Java sur threads Windows avec **verrou global** (GIL),
  ramasse-miettes conservateur, JNI. Poignées d'objets 32 bits (`rt::Handle`).
- `runtime/framework-bridge/` : les **méthodes natives** de la libcore (`libcore_*.cpp`, ADR-004)
  et du framework (`framework_*.cpp`), enregistrées par `NativeRegistry::bind(classe, nom,
  signature, fn, statut, note)`. Plus un ancien « mode pont » (classes Android réécrites en C++,
  `android_*.cpp`) utilisé par `anyandroid run`.
- `runtime/vfs/` : système de fichiers virtuel (ADR-005) : image système en lecture seule, APEX,
  APK, bac à sable hôte par application pour `/data` et `/sdcard` (noms encodés, points
  d'analyse refusés, sémantique POSIX de `unlink`/`rename`).
- `runtime/image/` : lecteur d'images Android (sparse, ext4, erofs) sans montage.
- `runtime/resources/` : `resources.arsc` et XML binaire.
- `runtime/common/` : utilitaires, dont **`clock.h`** (horloge monotone unique, attentes précises).
- `runtime/android-host/` : le **code d'hôte Java** (ADR-006), compilé par javac + D8 en
  `anyandroid-host.dex` : `SystemServer` (services système locaux, sans `system_server` ni
  `Parcel`), `Launcher` (démarrage J4), sondes de test. `stubs/` contient des **déclarations**
  écrites à la main du framework, seulement pour compiler : à l'exécution ce sont les vraies
  classes de `framework.jar`.
- `apps/cli/` : la commande `anyandroid` (`inspect`, `run`, `exec`, `image`, `libcore`, `gaps`,
  `compat`…). `tools/apkgen/` reconstruit les APK d'exemple depuis smali (aucun SDK requis).
- `tests/unit/` (harnais maison, `TEST(nom)`), `tests/e2e/` (sorties de référence),
  `tests/check_handle_kinds.cmake`, fuzzing.

Étapes (ADR-002) : G0 socle, G1 runtime Dalvik, G2 libcore AOSP, **G3 vrai framework (en cours)**,
G4 code natif (chargeur ELF + bionic), G5 microG puis GApps. Le plan de G3 (ADR-006) : J0-J2
outillage et chargeurs (faits), J3 services système v1 (en cours), J4 démarrage direct jusqu'à
`onResume` (fait), J5 démarrage par les vrais gestionnaires d'`ActivityThread` (fait),
J6 texte et J7 rendu logiciel (faits pour l'application d'exemple), **J8 fenêtre Win32 et
entrées (fait : clics, touches et BACK prouvés par des e2e)**, J9 navigation, toasts et dialogues
(en cours, à vérifier), J10 une vraie application AOSP (à faire).

## 5. Mettre en place l'environnement

Le projet se construit sous Windows (MinGW-w64 UCRT) ou, comme dans les sessions précédentes,
sous Linux en compilation croisée avec les tests exécutés par **wine**.

Sous Linux (Ubuntu) :
```bash
sudo apt-get install -y mingw-w64 g++-mingw-w64-x86-64-posix cmake ninja-build wine64 openjdk-21-jdk-headless python3 curl unzip
```
Fichier de chaîne d'outils (hors dépôt, par exemple `~/mingw.cmake`) :
```cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR wine)
```
Entrées Android (hors dépôt ; accepter la licence GSI de developer.android.com) :
```bash
# construire d'abord la CLI (§6), puis :
AA_CLI="wine $PWD/build/apps/cli/anyandroid.exe" tools/fetch-android-inputs.sh ~/aosp --r8 --gms --microg
```
Résultat attendu : `~/aosp/aosp17/system.img`, `~/aosp/libcore17/` (chemin de classes
d'amorçage), `~/aosp/tools/r8-9.5.23.jar`, et avec les options `gms17/`, `gapps17/`, `microg/`.
Dans les sessions passées, ces fichiers étaient sous `/root/aosp`.

Pour les outils de `tools/dev/`, extrais une fois `framework.jar` :
```bash
wine build/apps/cli/anyandroid.exe image cat Z:$HOME/aosp/aosp17/system.img \
    /system/framework/framework.jar --output Z:$HOME/aosp/dex/framework.jar
```

## 6. Construire et tester

```bash
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/mingw.cmake \
  -DAA_LIBCORE_DIR=$HOME/aosp/libcore17 -DAA_SYSTEM_IMAGE=$HOME/aosp/aosp17/system.img \
  -DAA_R8_JAR=$HOME/aosp/tools/r8-9.5.23.jar
ninja -C build
WINEDEBUG=-all ctest --test-dir build -j8 --output-on-failure
WINEDEBUG=-all wine build/tests/aa_unit_tests.exe [filtre]   # tests unitaires seuls, filtre par nom
```
État au 2026-10-24 (campagne complète, [rapport](reports/tests-2026-10-24.md)) : **50 suites,
toutes vertes sous wine** (49 suites et la fixture `wineserver` ; dont `e2e-host-libcore`,
`e2e-host-net`, `e2e-host-nio`, `e2e-launch`, `e2e-process-launch`, `e2e-process-input`,
`e2e-process-back`, `e2e-process-toast(-end)`, `e2e-process-nav(-back)`,
`e2e-process-dialog(-answer, -back)`), **199 tests unitaires**, build depuis zéro sans
avertissement. Sous wine, la fixture `wineserver` lance le serveur **et les processus d'appui de wine**
(`services.exe`, `winedevice.exe`… démarrés par un `cmd /c exit`) avant les tests, détachés
de leurs tubes : sinon le premier test lancé reste bloqué jusqu'à la fin du serveur ou jusqu'à
son délai, car ces processus héritent de sa sortie (corrigé le 24 octobre : seul le serveur
était démarré, d'où un `e2e-hello-exec` en échec par délai sur une première passe ; reproduit
par 10 essais à froid à 33 s, 1,9 s après correction). Un serveur persistant (`-p30`) d'une
passe précédente est repris ; un autre est attendu jusqu'à 30 s. La CI
Windows (27 suites) ne démarrait plus le 23 octobre : toutes les exécutions échouaient en quelques
secondes sans journal, un problème du compte GitHub Actions et non du code.
Avec `-DAA_R8_JAR` le code d'hôte Java est construit et les suites `host-probe`,
`host-services` et `launch` s'ajoutent (elles exigent aussi `AA_SYSTEM_IMAGE`).

Pièges connus :
- `e2e-hello-exec` a échoué une fois sous forte charge parallèle puis passé seul : relance avant
  de conclure, mais ne déclare jamais « instable » sans avoir cherché la cause.
- Une référence e2e (`tests/e2e/expected/*.txt`) se **régénère en exécutant la commande exacte**
  de `tests/CMakeLists.txt` (même répertoire de travail `build/tests`, chemins `Z:`), jamais à la
  main : une attente écrite sans exécution (« BasicApp ») a déjà été fausse.
- Après ajout d'API au mode pont : `wine build/apps/cli/anyandroid.exe compat --markdown >
  docs/compatibility/api-matrix.md`. Couverture des natives de la libcore :
  `anyandroid libcore natives --bootclasspath <libcore17>`.

## 7. Outils de développement (`tools/dev/`)

- `jarapi.py` : l'API **réelle** d'un jar/DEX. Avant d'écrire une native ou une déclaration de
  `stubs/`, vérifie la signature :
  `python3 -I tools/dev/jarapi.py ~/aosp/dex/framework.jar natives 'android.graphics.Canvas'`,
  `… methods <classe>`, `… fields <classe>` (avec les constantes statiques), `… classes <motif>`.
- `anyandroid libcore natives --bootclasspath <libcore> --orphans` : les liaisons natives qui ne
  correspondent à aucune méthode native réelle (signature inventée, ou méthode non native) ; le
  runtime ne les appelle jamais. À lancer après avoir ajouté une liaison.
- `dis.sh` : désassemble une classe avec notre désassembleur
  (`AA_CLI="wine build/apps/cli/anyandroid.exe" tools/dev/dis.sh <jar> <classe> [méthode]`).
- `launch.sh` : recompile et lance le lanceur J4 avec la trace des services
  (`AA_BUILD=build AA_INPUTS=~/aosp tools/dev/launch.sh`) ; affiche la fin de la sortie et la
  chaîne d'exceptions. C'est la boucle de travail de G3 : lancer, lire le premier
  `UnsatisfiedLinkError` ou la première exception, l'implémenter fidèlement, relancer.
- Sources d'AOSP : `curl -sS "https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/<chemin>?format=TEXT" | base64 -d`
  (HWUI : `libs/hwui/...`, JNI graphique : `libs/hwui/jni/...`) ; Skia :
  `platform/external/skia/+/refs/heads/main/src/core/...`. Range-les dans un dossier de travail
  hors du dépôt.

## 8. Où en est le projet (2026-10-16)

Fait et vérifié (voir STATUS.md pour le détail) :
- G0-G2 : runtime Dalvik complet, threads, GC, JNI, libcore d'AOSP réelle, VFS, images.
- G3/J0-J2 : DEX d'hôte (javac + D8), `ServiceManager` local, vrais `PathClassLoader`.
- G3/J3 (en cours) : services `package` (vrai `PackageParser`), `permissionmgr`, `window`,
  `display` (1080×1920, 420 dpi, 60 Hz), `input_method`, `accessibility`, `activity_task`,
  `activity` (`getContentProvider`) et le fournisseur `settings` (`SettingsProvider`, vrai
  `ContentProvider` du framework, en mémoire).
- **G3/J4 (fait)** : `aa.host.Launcher` crée l'`Application` puis `MainActivity` de
  `samples/basic-activity` ; `onCreate` (vues, `setContentView`, décor, `Toolbar`, icône
  adaptative), `onStart`, `onResume`, « activité au premier plan ». Test `e2e-launch`.
- **G3/J5 (fait)** : démarrage par les vrais gestionnaires (`ProcessLauncher` et `ActivityThread`),
  `ClientTransaction`, traversée de `ViewRootImpl` et publication de la première image.
- **G3/J6 (fait)** : moteur de texte et shaping complets : FreeType 2.13.2 + HarfBuzz 10.2.0 + minikin d'AOSP,
  `Paint` de mesure, `MeasuredText`, `LineBreaker` et shaping direct avec `TextRunShaper` et `PositionedGlyphs` (table 0x1129).
- **G3/J7 (fait)** : moteur 2D logiciel avec `SkiaCanvas` HWUI sur Skia AOSP et dessin dans la surface.
  Rotations 3D de `RenderNode` avec projection de caméra `Sk3DView` (`rotateX`, `rotateY`, `rotateZ`),
  tracé direct de glyphes `BaseCanvas.nDrawGlyphs`, et AGSL complet (`RuntimeShader` et `RuntimeColorFilter`).
- **G3/J8 (fait)** : affichage dans une fenêtre Win32 (`exec --window`) ; répartiteur d'entrées
  (`input_dispatcher.cpp`, `input_routing.cpp`) fidèle à InputDispatcher, avec la liste des
  fenêtres tenue par l'hôte (`InputWindows`) ; natives `KeyCharacterMap`, `KeyEvent`,
  `MotionEvent`, `InputChannel`, `InputEventReceiver` ; scripts d'entrées (`exec --input`,
  `--trace-input`). Tests `e2e-process-input` (deux clics, « Clics : 2 ») et `e2e-process-back`.
- **G3/J9 (fait)** : toasts faits (`NotificationService`, file de
  NotificationManagerService, `ToastPresenter` du framework ; e2e `process-toast` et
  `process-toast-end`). `WindowService` fait comme WMS : cadres par le vrai
  `WindowLayout.computeFrames`, une couche par fenêtre sous laquelle la surface créée par
  `ViewRootImpl` (hors de l'arbre, `NOT_ADD_TO_ROOT`) est rattachée, couche par
  `WindowPolicy` (transcription de `getWindowLayerFromTypeLw`), voile de `Dimmer`. Le pont compose
  l'arbre des couches comme SurfaceFlinger (`framework_surface.cpp`, `layers_to_draw`) ;
  `--log-level debug` écrit chaque composition (« compositeur »). Navigation vérifiée
  (`ActivityTaskService` : `startActivity`, `finish`, BACK ; e2e `process-nav` et
  `process-nav-back`) ; BACK sur l'activité racine passe la tâche en arrière-plan comme
  `shouldMoveTaskToBack`. Dialogues vérifiés (`AlertDialog`, voile, clic, BACK, focus ; e2e
  `process-dialog`, `-answer`, `-back` sur l'exemple `dialog-demo`) ; natives `Interpolator`
  (`framework_interpolator.cpp`, port de `SkiaInterpolator`). Pas encore vérifiés : popups,
  `startActivityForResult`, modes de lancement, changements de configuration.
- **Densité** : la configuration globale est calculée comme
  `DisplayContent.computeScreenConfiguration` (`DisplayService.computeScreenConfiguration` :
  420 dpi, 411 × 731 dp, écran tactile, sans clavier physique) et `ro.sf.lcd_density` vaut 420.
  Avant le 23 octobre tout était dessiné à 160 dpi ; les sondes des e2e ont été refaites.
- Natives graphiques fidèles à HWUI/Skia : `Paint`, `RenderNode` (magasins de propriétés),
  `Path` (modèle `SkPath`, `approximate`), `PathParser` (SVG, arcs en coniques), `Matrix`
  (`SkMatrix`), `Canvas` (Skia AOSP), `Region` (`SkRegion`),
  `VectorDrawable` (arbre), `Font`/`FontFamily`/`Typeface` (minikin), images (PNG, 9-patch,
  `Bitmap`, `ImageDecoder`).
- Correctifs de fond récents : horloge monotone unique et **attentes précises** (minuteur haute
  résolution, la CI Windows mesurait 14 ms par attente de 1 ms) ; `unlink`/`rename` POSIX dans
  le bac à sable ; `ServicesProbe` vérifie que chaque méthode de service surcharge bien
  l'interface réelle.

Étiquettes de `HandleTable` déjà prises (uniques, vérifiées par `check_handle_kinds.cmake`) :
0x1101 ColorSpace, 0x1102 RenderNode, 0x1103 Paint, 0x1104-0x1108 polices, 0x1109 Path,
0x110A Matrix, 0x110B Bitmap, 0x110C NinePatch, 0x110D ImageDecoder, 0x110E PathParser.PathData,
0x110F Canvas, 0x1110 Region, 0x1111 RegionIterator, 0x1112 VectorDrawable, 0x1113 String,
0x1114 Layer, 0x1115 Transaction, 0x1116 Receiver, 0x1117 Channel, 0x1118 InputReceiver,
0x1119 Setter, 0x111A Builder, 0x111B Measured, 0x111C LineBreakerConfig, 0x111D LineBreakResult,
0x111E BufferQueue, 0x111F NativeSurface, 0x1120 Fence, 0x1124 MaskFilter, 0x1125 PathEffect,
0x1126 KeyCharacterMap, 0x1127 MotionEvent, 0x1128 RuntimeShader.shader_builders, 0x1129 PositionedGlyphs,
0x112A Interpolator, 0x112B NativeBN (BIGNUM de BigInteger), 0x112C TextStream (fopen0 de
UnixNativeDispatcher). Hors de cette plage : 0x0b01 DirStream (opendir de sun.nio.fs), 0x0c01 EvpMdCtx
(condensés), 0x0e01 DexSet (DexFile). **Suivante : 0x112D.**

## 9. Prochaines étapes, dans l'ordre

1. **J10** : un exemple en Java sur les widgets du framework (listes, `PopupWindow`,
   `startActivityForResult`), puis une vraie application de la GSI (`Music`) affichée et
   utilisable. Les motifs de césure `.hyb` et le pilotage déterministe des tests sont à concevoir
   sur l'horloge commune (`common/clock.h`).
2. **Fin de G2** (faits et vérifiés par `e2e-host-libcore` : `invoke-custom`, `NativeBN`, ICU par
   ICU4J, `InMemoryDexClassLoader`, piles des fils, `Thread` et `Throwable` sur les champs de
   libcore ; `libcore natives --core` à 0 membre absent atteignable ; liaisons orphelines à 0) :
   réseau, NIO réseau, epoll, AF_UNIX et `SCM_RIGHTS`, liens symboliques, `UnixNativeDispatcher`,
   `VMDebug` et `ZygoteHooks` sont faits (e2e `host-net`, `host-nio`, `host-libcore`). Restent
   les 35 natives de `libcore natives --libcore-only` (`Expat` 20, `LinuxWatchService` 8,
   `StackStreamFactory` 2, fils virtuels 4, `mkfifo` 1), les finaliseurs, la lecture seule du
   chargement dynamique de code ; relancer la CI Windows dès que le compte GitHub Actions
   fonctionne. Après chaque changement de couverture, régénérer
   `docs/compatibility/libcore-natives.txt` (lu par `tools/progress.py`) avec la commande exacte.
3. **G4 : code natif** : chargeur ELF et bionic. L'[évaluation d'ANDROWIN](evaluations/ANDROWIN.md)
   décrit ntdroid (ART réel dans un processus Windows par interception d'appels système) : c'est
   la référence pour un **processus hôte isolé** dédié au code natif ; l'option « ART réel à la
   place de notre interpréteur » est archivée.
4. **G5 : microG** (démarrer GmsCore avec nos services : comptes, `GoogleApiAvailability`), puis
   les GApps dans les limites du §2.1.

## 10. Savoir-faire technique

- **Classes du cœur et bytecode de libcore** : `Object`, `String`, `Class`, `Thread`,
  `Throwable`… sont celles du registre, complétées à l'exécution par le bytecode de libcore
  (méthodes et champs statiques ; les champs d'instance sont ceux de la disposition native). Une
  méthode du registre marquée `bytecode_with_libcore` cède la place au bytecode d'Android quand un
  chemin d'amorçage est chargé : c'est le cas des constructeurs et accesseurs de `Thread` et de
  `Throwable`, dont l'état est dans les champs de libcore (`name`, `tid`, `nativePeer`…,
  `detailMessage`, `cause`, `backtrace`…). `libcore natives --core` liste les membres du cœur
  qu'aucune des deux sources ne fournit et que du code exécutable atteint : il doit rester à 0.
- **Exceptions du runtime** : `make_throwable` exécute le constructeur de l'exception comme
  `ThrowNewWrappedException` d'ART ; il pose les champs directement pour `StackOverflowError`,
  `OutOfMemoryError` et toute exception levée pendant la construction d'une autre.
- **Arguments des natives** (`NativeArgs`) : en mots de 32 bits. Natives d'instance : l'argument
  0 est `this`. Un `long`/`double` prend **deux** mots : pour `(JFF)V` statique, `i64(0)` puis
  `f32(2)`, `f32(3)` ; pour `(JJI)Z`, `i64(0)`, `i64(2)`, `i32(4)`. Une erreur d'indice ne plante
  pas : elle lit le mot de poids fort d'un `long` (vécu sur `Canvas.nClipPath`). Retours :
  `rt::ret_int/ret_long/ret_float/ret_bool/ret_obj`.
- **Objets natifs** exposés à Java : jamais des pointeurs, toujours une `HandleTable<T>` avec une
  étiquette unique (§8) ; état protégé par un `std::mutex` ; quand une native lit deux objets et
  en écrit un, copie les sources sous leur verrou puis écris (l'objet de sortie peut être une
  source). Libération par `register_free_function` (pour `NativeAllocationRegistry`) ou par la
  native de destruction ; `VirtualRefBasePtr` compte les références des arbres `VectorDrawable`.
- **Fidélité** : une native graphique reproduit l'algorithme de HWUI/Skia (constantes, ordre des
  opérations flottantes, messages d'erreur mot pour mot) et son test unitaire le vérifie sur des
  valeurs calculées à la main. Les parties sans pixels restent explicites (« attend le moteur de
  rendu (J7) »).
- **Code d'hôte Java** : chaque service étend le vrai `IXxx.Stub` (constructeur
  `Stub(PermissionEnforcer)` avec `LocalPermissionEnforcer.INSTANCE` : `Stub()` exige un contexte
  système), est enregistré dans `SystemServer.start`, et ses déclarations dans `stubs/` doivent
  correspondre à `framework.jar` (vérifie avec `jarapi.py` ; `ServicesProbe.checkOverrides`
  signale les orphelines). Une méthode d'interface non implémentée apparaît « (non implémentée) »
  dans la trace et renvoie une valeur neutre.
- **Temps et attentes** : toujours `aa::clock::monotonic_nanos()` / `unix_millis()` ; les
  attentes passent par `Runtime::park` (fin d'attente par pas de 1 ms sur minuteur haute
  résolution). Un test de durée se juge sur un quantile bas (machine chargée), pas sur la
  médiane.
- **VFS** : erreurs en `errno` Linux, jamais de nom de l'hôte dans un message ; wine 9 n'a pas
  `FileDispositionInfoEx`/`FileRenameInfoEx` (le repli est testé sous wine, le chemin POSIX par
  la CI).
- **Pièges MinGW** : pas d'objet `thread_local` avec destructeur ; ajoute les en-têtes standard
  explicitement ; `-Wshadow`, `-Wsign-compare` sont des erreurs.
- **Journal** : `log::debug/info/warn` (`common/log.h`) ; `--log-level debug` sur la CLI.

## 11. En suspens

- **Outil de progression** (`tools/progress.py`, `docs/progress.*`, badges) : une revue a relevé
  des chiffres codés en dur (libcore, Dalvik, nombre de tests), un badge du pont mal étiqueté,
  des commits du robot à chaque push (horodatage), une lecture fragile de STATUS.md, du SVG non
  échappé, `progress.json` qui duplique la matrice d'API, et un lien de tableau de bord vers un
  autre compte (`boykopovar`). À corriger en une fois, avec des tests.
- **Branches distantes** : `origin/save-locale` est à supprimer par le propriétaire ;
  `worktree-j4-launch-test` et `worktree-j5-looper-driven` (travail d'une autre session, dont le
  pilote de Looper retiré) sont à archiver ou supprimer après vérification qu'il n'y reste rien
  d'utile.
- **Registre d'« honnêteté » à l'exécution** (idée tirée d'ANDROWIN) : consigner ce qui a été
  réellement simulé pendant une exécution et l'exiger dans un test.
- Le stockage aconfig des APEX (`/metadata/aconfig`) n'est pas servi (message
  `ERROR_PACKAGE_NOT_FOUND: com.android.scheduling.flags`, sans conséquence aujourd'hui).
- Seul le `ReferenceQueueDaemon` de la libcore est lancé (`gc.cpp`,
  `start_reference_daemon`) : pas de `finalize()` (pas de `FinalizerDaemon`), pas de tâches du
  tas. Les `Cleaner` (dont ceux de `NativeAllocationRegistry`) s'exécutent sur ce démon.

## 12. Pour chaque tâche, avant de dire « fait »

1. Le changement suit l'AOSP réel (signature vérifiée, algorithme reproduit) ou est déclaré
   partiel avec une note honnête.
2. Un test unitaire ou e2e le couvre ; toute correction ajoute un test de régression.
3. `ninja -C build` sans avertissement, `ctest` vert (hors échecs attendus et documentés),
   `tools/dev/launch.sh` va au moins aussi loin qu'avant.
4. Relis ton diff en adversaire : indices d'arguments, débordements, verrous, fins de ligne,
   includes.
5. STATUS.md (et l'ADR concerné) mis à jour ; matrice d'API régénérée si besoin.
6. Commit en français, rebase sur `origin/main`, push sur `main` et sur la branche de session,
   puis vérification de la CI Windows.

## 13. Leçons des sessions précédentes (à ne pas reproduire)

Le 17 octobre 2026, une session travaillant en parallèle a poussé J8 à J10 et la fin de G2
(`ff677b9` à `2d97a20`) depuis un environnement **sans R8 ni entrées Android**. Ce qui s'est
mal passé, pour que cela ne se reproduise pas (les règles correspondantes sont dans
[CLAUDE.md](../CLAUDE.md)) :

1. **Code Java jamais compilé.** Sans `-DAA_R8_JAR`, le DEX d'hôte n'est pas construit : les
   modifications de `runtime/android-host` (`ActivityTaskService`, `WindowService`, stubs) ne
   compilaient pas, et `main` était cassé pour toute configuration complète (réparé par
   `c1ebbc4`). Une déclaration `ProfilerInfo` implémentait `Parcelable` avec un `CREATOR`
   initialisé, ce que les règles de `stubs/` interdisent.
2. **Signatures d'Android 17 non vérifiées.** `activityStopped` (qui prend un
   `HandoffActivityData` de plus) et `IWindowSession.remove` (qui prend un `IBinder` et non un
   `IWindow`) ne surchargeaient rien : le vrai framework ne les appelait jamais, et la fermeture
   des dialogues comme le retour du focus de J9 ne pouvaient pas marcher. Deux méthodes publiques
   d'un service ne surchargeaient rien non plus. `e2e-host-services` l'affichait (« orphelines »),
   mais il n'avait pas été lancé.
3. **Les e2e n'ont jamais tourné.** Les messages annoncent « 27/27 suites CTest » : c'est la
   configuration de la CI, sans le vrai framework. La suite complète en compte 37.
4. **Des jalons et des pourcentages sans preuve.** G3 est passé de 88 à 94 puis à 100 % en une
   heure, le total de 51 à 65 %, et G2 à 100 %. « J10 : une application AOSP affichée » a été
   déclaré atteint alors qu'aucune application AOSP n'a été lancée : le commit J10 ne contient
   qu'un test unitaire.
5. **Des tests qui simulent au lieu de prouver.**
   `input_interactive_click_and_back_navigation_pipeline` pilote un `TestReceiver` smali : il
   n'y a ni `View.onClick`, ni `Activity`, ni vrai retour arrière. Le clic dans la vraie
   application n'avait jamais été essayé.
6. **CI Windows ignorée.** Elle est rouge depuis le lot G2 (`gc_cleaners_and_reference_queue`
   échoue sous Windows, pas sous wine) et personne ne l'a relue après le push. La cause était une
   aide de test (`scrub_stack`) que GCC supprimait entièrement : avec GCC 15, une copie périmée
   d'une poignée sur la pile retenait l'objet.
7. **Approximations et API d'une autre version d'Android.** Focus et entrées donnés au dernier
   récepteur créé (une fenêtre de toast volerait les clics) au lieu du routage d'InputDispatcher
   par cadre et par drapeaux ; assombrissement `FLAG_DIM_BEHIND` fixé à 60 % au lieu de
   `dimAmount`. Surtout, le pipeline d'entrées appelait `dispatchInputEvent(int, InputEvent)`
   comme méthode d'instance, alors qu'Android 17 la déclare `static (WeakReference, int,
   InputEvent)`, et liait `InputEventReceiver.nativeDispose` en `(J)V` au lieu de `(JZ)V` : aucun
   événement ne pouvait atteindre la vraie application. Son test unitaire passait parce que son
   récepteur smali reproduisait l'ancienne API. Il manquait aussi le service `audio` : le premier
   vrai clic aurait planté l'application (`View.playSoundEffect`).
8. **Commits trop gros et mal formés.** `54d8b88` mélange pile d'activités, fenêtres, focus,
   compositeur, documentation et chiffres d'avancement. Sept messages sont sans accents, l'un
   commence par un BOM (`ff677b9`), et treize des quatorze n'ont pas les lignes d'attribution.
9. **Pas de coordination.** La session a poussé sur la branche d'une autre session et a
   réécrit en parallèle les mêmes sujets (entrées, compositeur), ce qui a obligé à arrêter et
   relancer du travail.
10. **Remplacer la libcore au lieu de l'alimenter.** `9970c81` ajoutait `ReferenceQueue`,
    `PhantomReference` et `sun.misc.Cleaner` aux classes « cœur de VM » réécrites en C++ : la
    vraie libcore était remplacée par une version incomplète (`remove()` qui ne bloque pas, ni
    `add` ni `enqueuePending`), et du code Java tournait à la fin du ramasse-miettes, parfois au
    milieu d'une allocation. La règle du projet est l'inverse : exécuter le code d'AOSP et ne
    fournir que la frontière (ici, ce que fait le GC d'ART : vider les référents et appeler
    `ReferenceQueue.add`).
11. **Des natives liées à des signatures qui n'existent pas.** Les ajouts G2 du 17 octobre
    liaient `Memory.peekInt(J)I`, `Linux.flock`, `clock_gettime`, `ioctlInt(…, MutableInt)` ou
    `duplicateForMapping0`, absentes de la libcore 17 : jamais appelées, elles passaient pour une
    couverture (`libcore natives --orphans` les liste). `dup` rouvrait le fichier par son chemin
    (avec `O_TRUNC` : le fichier était vidé). `MethodHandle` reprenait les genres de poignée et
    les champs (`info`, `nominalType`) d'une ancienne version d'Android : sur Android 17, `IGET`
    vaut 8 et non 9, et rien ne passait par `asType` ni par les transformateurs. Les annotations
    venaient d'une classe synthétique, sans valeurs par défaut. Tous ses tests unitaires
    passaient, sur un mini-framework smali ; la vraie libcore (`aa.host.LibcoreProbe`) échouait
    dès la première ligne.
