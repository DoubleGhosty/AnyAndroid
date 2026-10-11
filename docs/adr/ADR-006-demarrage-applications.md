# ADR-006 — Démarrer les applications par le vrai `ActivityThread`

- **Statut** : accepté le 2026-10-11 (étape G3 de l'ADR-002)
- **Contexte** : le vrai `framework.jar` d'Android 17 s'exécute déjà (exemple `framework-demo` :
  `Bundle`, `Parcel`, `Binder` local, `MessageDigest`, `Resources.getSystem()`…), mais aucune
  application ne démarre encore comme sur un téléphone. Les applications ne tournent que dans le
  « mode pont » (`anyandroid run`), qui réimplémente en C++ un sous-ensemble de widgets.
- **Sources** : lecture de `ActivityThread`, `LoadedApk`, `ContextImpl`, `Instrumentation`,
  `WindowManagerGlobal`, `ViewRootImpl`, `ServiceManager` (AOSP `main` et tag `android-17.0.0_r1`)
  et du bytecode du `framework.jar` de la GSI CP3A.260905.010 ; approche de Robolectric
  (`AndroidTestEnvironment`, `ActivityController`), qui exécute lui aussi le vrai framework hors
  d'Android.

## 1. Décision : pas de `system_server`, des services locaux

Sur un téléphone, le processus de l'application (`ActivityThread.main`) s'enregistre auprès de
`system_server` par Binder, qui lui renvoie `bindApplication` puis les transactions de lancement
d'activité. AnyAndroid **joue lui-même le rôle de `system_server` dans le processus** :

- le code Java d'AOSP n'est **ni modifié ni intercepté** : on ne remplace que la frontière native
  (`libandroid_runtime`, `libhwui`, `libgui`) et on fixe des réglages officiels
  (`ThreadedRenderer.sRendererEnabled`, propriété `debug.choreographer.vsync`) ;
- les **services système** (`package`, `activity`, `activity_task`, `window`, `display`,
  `input_method`, `accessibility`, `permissionmgr`, `notification`…) sont des objets Java qui
  étendent les classes `IFoo.Stub` du `framework.jar`. `ServiceManager.getService` les obtient par
  les natives `BinderInternal.getContextObject` / `ServiceManagerProxy.getNativeServiceManager`, qui
  renvoient un `LocalServiceManager` ; `IFoo.Stub.asInterface` trouve alors l'objet par
  `queryLocalInterface` et les appels sont **directs, sans `Parcel`** ;
- l'hôte construit `ActivityThread`, reproduit `attach(false)` et le prélude de
  `bindApplication` (`ApplicationSharedMemory`, `SystemFeaturesCache`, réglages cœur,
  `AppBindData`), puis appelle **les vrais gestionnaires** : `handleBindApplication`,
  `handleLaunchActivity`, `handleStartActivity`, `handleResumeActivity`. Un mode « direct » à la
  Robolectric (`makeApplicationInner`, `Activity.attach`, `performCreate`…) sert d'amorçage et de
  repli pour les tests.

Alternatives écartées :

| Option | Pour | Contre |
|---|---|---|
| **Services locaux + gestionnaires d'`ActivityThread`** (retenue) | vrai code de démarrage, testable pas à pas, rien à émuler côté noyau | sémantique Binder locale (alias, appels `oneway` synchrones) à encadrer |
| `system_server` émulé au niveau des transactions Binder (`onTransact`, codes `TRANSACTION_*`) | fidélité maximale du transport | marshalling complet de dizaines d'interfaces avant le premier écran ; aucun gain avant le multi-processus (G5) |
| Proxys dynamiques (`java.lang.reflect.Proxy`) des interfaces AIDL | peu de code par méthode | exige `Proxy.generateProxy` dans la VM d'abord ; types de retour complexes construits par réflexion, fragiles |

Les services étendent `IFoo.Stub` (et non `IFoo.Default`) : la même implémentation pourra recevoir
de vraies transactions quand le multi-processus arrivera (G5, GmsCore compte 14 processus).

Règles de la sémantique Binder locale :

1. les paramètres `in` sont copiés défensivement (`LayoutParams.copyFrom`…), les paramètres `out`
   (`InputChannel`, `WindowRelayoutResult`, `SurfaceControl`) remplis dans l'objet de l'appelant ;
2. les appels `oneway` et les rappels vers le client (`IWindow.resized`…) sont postés sur un
   `HandlerThread` « aa.system », jamais exécutés de façon synchrone et réentrante ;
3. une méthode de service non implémentée est **journalisée** et renvoie une valeur neutre ;
   `--trace-services` écrit la liste des appels (golden des tests) ;
4. les services absents qu'AOSP tolère (`connectivity`, `autofill`, `content_capture`,
   `voiceinteraction`) renvoient `null` ; ceux qu'il exige (`input_method`) existent.

## 2. Outillage : code d'hôte en Java, compilé par javac et D8

Les services manipulent des dizaines de types du framework (`ApplicationInfo`, `DisplayInfo`,
`InsetsState`, `WindowRelayoutResult`…) dont les signatures varient d'une version à l'autre :
les écrire en C++ (objets Java construits champ par champ) ou en smali serait fragile.

- le code d'hôte vit dans `runtime/android-host/` (Java), compilé en `anyandroid-host.dex` par
  `javac --release 17` puis **D8** (R8, Apache-2.0, téléchargé et vérifié par
  `tools/fetch-android-inputs.sh --r8`, jamais versionné) ; il est chargé dans l'espace
  d'amorçage, après le `framework.jar` : une application ne peut pas remplacer ses classes ;
- la compilation se fait contre des **déclarations minimales écrites à la main** des classes du
  framework utilisées (`runtime/android-host/stubs/`, jamais incluses dans le DEX). Une méthode de
  `IFoo.Stub` que l'hôte n'implémente pas déclenche, au lieu d'`AbstractMethodError`, un appel
  journalisé qui renvoie la valeur neutre du type (règle 3) : la VM le fait pour les classes du DEX
  d'hôte. Une surcharge dont la signature ne correspond plus au jar apparaît donc comme « non
  implémentée » dans la trace, au lieu de casser silencieusement ;
- l'option CMake `AA_ANDROID_HOST` n'est active que si un JDK ≥ 17 et le jar R8 sont présents,
  comme les tests sur image (`AA_LIBCORE_DIR`) : le build de base reste sans Java ni SDK.

## 3. Rendu : chemin logiciel d'AOSP d'abord

Avec `ThreadedRenderer.sRendererEnabled = false`, `ViewRootImpl` dessine par `drawSoftware` :
`Surface.lockCanvas` → `View.draw(Canvas)` → `unlockCanvasAndPost`. AnyAndroid implémente les natives
de `Canvas`, `Paint`, `Path`, `Matrix`, `Bitmap`, `Typeface` et de la mesure du texte sur un moteur
2D logiciel, et présente la surface dans une fenêtre Win32 (ou en PNG en mode headless). `RenderNode`
ne sert que de magasin de propriétés ; HWUI et l'accélération matérielle sont hors G3. Le choix du
moteur 2D et du moteur de texte (HarfBuzz/FreeType ou équivalent, composants tiers) fera l'objet de
l'ADR-007, avant le jalon J7.

## 4. Jalons

Chaque jalon a un test automatique (APK d'exemple et sortie de référence, ou test unitaire) et donne
lieu à un commit.

| Jalon | Livrable | Test |
|---|---|---|
| **J0** — Outillage | `AA_ANDROID_HOST` (javac + D8), DEX d'hôte chargé à l'amorçage, méthodes abstraites non implémentées journalisées, `--trace-services` | exemple Java minimal compilé et exécuté sur la vraie libcore |
| **J1** — ServiceManager local | natives `BinderInternal`/`ServiceManagerProxy`, `LocalServiceManager`, registre des services | `ServiceManager.getService("package")` renvoie le service local, `asInterface` aussi |
| **J2** — Chargeurs de classes | APK servi par le VFS (`/data/app/...`), `DexFile`/`PathClassLoader` réels, classe → chargeur de définition | une classe de l'APK chargée par son `PathClassLoader` |
| **J3** — Services v1 | `package` (via `PackageParser`), `activity`, `activity_task`, `IActivityClientController`, `display`, `window`/`IWindowSession`, `input_method`, `accessibility`, `permissionmgr`, `notification` | trace des appels de service de référence |
| **J4** — Démarrage direct | `Application` puis `Activity` jusqu'à `onResume`, sans fenêtre | journal du cycle de vie d'un APK d'exemple |
| **J5** — Démarrage par les gestionnaires | `handleBindApplication` … `handleResumeActivity`, fenêtre logique (`addToDisplayAsUser`, `relayout` calculé par `WindowLayout`), Looper piloté par l'hôte, horloge virtuelle pour les tests | même journal, par le vrai chemin |
| **J6** — Texte | polices de l'image, mesure du texte cohérente avec le dessin | mesures de référence |
| **J7** — Rendu logiciel headless | natives graphiques, `Surface`/`BLASTBufferQueue` vers un compositeur | image PNG de référence (empreinte) |
| **J8** — Fenêtre Win32 et entrées | présentation, souris/clavier → `InputChannel`, vsync | scénario headless + essai manuel |
| **J9** — Navigation | plusieurs activités, retour, cycle de vie complet | scénario de navigation |
| **J10 — Sortie de G3** | une application réelle affichée et utilisable : d'abord un exemple AnyAndroid en Java sur les widgets du framework, puis une application de la GSI (`Music`, puis `DeskClock` ou `Calendar` ; la Calculatrice AOSP n'existe pas dans les images d'Android 17) | scénario headless sur l'application |
| **G5.1** | démarrage du processus principal de microG GmsCore avec les mêmes services | journal de démarrage |

## 5. Risques

- **Le jar n'est pas la source** : le `framework.jar` de la GSI diffère du tag publié (exemple :
  ordre des paramètres d'`IWindowSession.relayout`). Les surcharges se vérifient contre le jar réel
  (trace des méthodes non implémentées) et les constructeurs longs (`ActivityClientRecord`) se
  résolvent par réflexion.
- **Performances de l'interpréteur** : `SystemServiceRegistry.<clinit>`, la carte des polices et
  l'inflation coûtent des millions d'instructions ; `exec --profile` guide les optimisations.
- **Périmètre des services** : chaque application en touche de nouveaux ; la trace et les valeurs
  neutres rendent ce glissement visible et non bloquant.
- **Sécurité** : le DEX d'hôte partage la VM avec du code non fiable, sans contrôle d'accès. Ses
  services ne détiennent aucune capacité au-delà du bac à sable de l'application, et les natives
  d'hôte restent sûres même appelées par l'application (identifiants bornés, quotas, aucun chemin de
  l'hôte). L'identité reste honnête (« AnyAndroid »), sans usurpation de signature ni contournement
  d'intégrité (ADR-002).
- **Déterminisme des tests** : threads d'arrière-plan et horloges ; horloge virtuelle et Looper
  piloté par l'hôte en mode test.
