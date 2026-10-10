# ADR-004 — G2 : la libcore d'AOSP sur la VM AnyAndroid

- **Statut** : **critère de sortie atteint le 2026-10-10**. Les programmes `hello-dex` et
  `threads-demo` s'exécutent sur la libcore réelle d'Android 17 (GSI AOSP x86_64
  `DP11.260918.006`, SHA-256 vérifié) avec une sortie identique à la référence ; tests de bout en
  bout `hello-libcore` et `threads-libcore` (facultatifs, `-DAA_LIBCORE_DIR=<dossier extrait>`).
  Voir §5 pour le chemin parcouru et §4 pour la suite.
- **Contexte** : le pont natif (`runtime/framework-bridge`) réimplémente environ 110 classes Java et
  Android. Les GApps en utilisent des milliers. Le pivot accepté (ADR-002, D3) consiste à exécuter le
  code Java d'AOSP lui-même sur notre VM, en commençant par la bibliothèque de classes `libcore`
  (`core-oj.jar`, `core-libart.jar`, `okhttp.jar`, `bouncycastle.jar`, `apache-xml.jar`).

## 1. Provenance : extraction d'une image système officielle

**Décision.** La libcore est extraite d'une image système Android officielle (GSI AOSP x86_64) par
`anyandroid image bootclasspath <system.img> --output <dossier>`. Rien n'est compilé depuis les
sources AOSP et **aucun fichier extrait n'entre dans le dépôt** : le dossier de sortie est choisi
par l'utilisateur, hors du dépôt, et `bootclasspath.txt` le rappelle en tête.

Chaîne de lecture (`runtime/image/`), sans code tiers, bornée et testée sur des images synthétiques :

| Couche | Formats pris en charge | Refusé explicitement |
|---|---|---|
| Conteneur | image brute, image *sparse* (libsparse), image *super* (partitions dynamiques liblp, sommes SHA-256 vérifiées) | ZIP de l'image (décompresser avec `tar -xf`), partitions multi-périphériques |
| Système de fichiers | **ext4** (extents, *block map*, descripteurs 32/64 bits, flex_bg, meta_bg, répertoires hachés lus linéairement, blocs partagés) ; **EROFS** (plat, en ligne, découpé, compressé **LZ4** avec index complets ou compacts 2B/4B, grands pclusters, *zero padding*, queue en ligne, pclusters entrelacés) | inline_data ext4, chiffrement, EROFS LZMA/DEFLATE/ZSTD, fragments EROFS |
| Paquets | APEX et APEX compressés (`.capex`), APEX aplatis | APEX antérieurs à Android 10 |
| Configuration | `classpaths/bootclasspath.pb` (système et modules), `build.prop` | — |

L'ordre du chemin de classes suit `derive_classpath` : module ART, puis `/system`, puis les autres
modules. Quand un module existe en version de production et de débogage, la production l'emporte.
Les données lues par la libcore à l'exécution (ICU `etc/icu`, fuseaux horaires `etc/tz`) sont
extraites avec les archives. Les chemins issus de l'image sont validés (pas de `..`, pas de chemin
absolu), la taille par fichier et le total sont plafonnés.

**Image retenue** : la GSI AOSP x86_64 publiée par Google (developer.android.com). Son
téléchargement suppose l'acceptation, par l'utilisateur, de la licence de la page de
téléchargement ; il n'est jamais fait sans confirmation explicite.

## 2. Mode libcore : chemin de classes d'amorçage hybride

`Runtime::add_boot_dex()` déclare les DEX du chemin d'amorçage. La résolution d'une classe suit
alors la délégation parent d'abord d'Android :

1. **Cœur de la VM** (`is_vm_core_class`) : `Object`, `String`, `Class`, `Throwable`, `Thread`,
   `Reference`, `WeakReference`, `SoftReference`, et dans `java.lang.reflect` : `AccessibleObject`,
   `Method`, `Constructor`, `Field`, `Array`, `InvocationTargetException`. Ces classes portent un état
   natif dont dépendent le modèle objet, les moniteurs, les threads, les exceptions et la réflexion.
   **Fusion** : leur implémentation native est complétée par le bytecode de la même classe dans le
   chemin d'amorçage (méthodes sans équivalent natif, champs statiques, `<clinit>`, interfaces
   supplémentaires). Le natif l'emporte toujours ; les champs d'instance restent ceux de la
   disposition native, si bien qu'un accès à un autre champ échoue explicitement ;
2. **chemin d'amorçage** (bytecode AOSP) : tout le reste de `java.*`, `javax.*`, `libcore.*`,
   `dalvik.*`, `android.system.*`… ;
3. **pont** : classes absentes du chemin d'amorçage (`android.*` du pont, jusqu'à G3) ;
4. **application** : une application ne peut pas remplacer une classe d'amorçage.

Conséquences d'interopérabilité traitées : les classes d'amorçage peuvent étendre une classe du
cœur (exceptions de la libcore sous notre `Throwable`) ; les interfaces (`CharSequence`,
`Comparable`…) viennent de la libcore, avec leurs méthodes par défaut ; les types encapsulés
(`Integer`…) passent par `valueOf()` et le champ `value` ; `Thread` expose `threadLocals` et
`inheritableThreadLocals` (lus directement par `ThreadLocal`) ; `Reference` expose `queue`,
`queueNext` et `pendingNext` (manipulés par `ReferenceQueue`), le référent restant dans l'état natif
pour que le ramasse-miettes le traite comme faible ; `Class.getClassLoader()` renvoie
`BootClassLoader.getInstance()` pour les classes d'amorçage, comme Android (et non `null` comme la
JVM de référence).

Reproductions d'ART nécessaires à la libcore :

- **`String`** porte les champs d'ART `count` (`(longueur << 1) | codeur`, compression des chaînes
  ASCII 1–0x7F) et `hash`, tenus à jour par le runtime : la libcore lit `count` directement
  (`length()`, `coder()`, `StringBuilder` à chaînes compactes). Ses constructeurs ne s'exécutent
  jamais : comme dans ART, chacun est redirigé vers la méthode `StringFactory` de mêmes paramètres.
- **`invoke-polymorphic`** est exécuté par l'interpréteur avec le prototype du site d'appel. Les
  `VarHandle` de champ, de champ statique et d'élément de tableau (`FieldVarHandle`,
  `StaticFieldVarHandle`, `ArrayElementVarHandle`) sont implémentés pour tous les modes d'accès
  (lecture, écriture, CAS, échange, `getAndAdd`, opérations bit à bit), avec conversions
  d'encapsulation et contrôles de type, de bornes et de champ final. `Field.getArtField()` renvoie un
  identifiant borné du runtime, jamais un pointeur. `MethodHandle.invoke` reste à faire.
- **DEX 041** : le format conteneur d'Android 15+ (plusieurs DEX dans un fichier, décalages relatifs
  au conteneur, table de chaînes partagée) est pris en charge ; `framework.jar` d'Android 17 en
  compte 6 sections.
- **`LockSupport.park/unpark`** sont reliés aux threads de la VM (un jeton par thread, attente
  sans le verrou global, réveil sur interruption).

## 3. Méthodes natives de la libcore

Dans ART, elles vivent dans `libjavacore.so`, `libopenjdk.so` et `libart.so`. AnyAndroid les
implémente en C++ pour Windows et les **lie par signature exacte**
(`NativeRegistry::bind`, fichier `runtime/framework-bridge/libcore_natives.cpp`) ; un intrinsèque
peut aussi remplacer un corps de méthode en bytecode (`NativeRegistry::intrinsic`). Une méthode
native non liée lève `UnsatisfiedLinkError` « méthode native de la plateforme non implémentée » et
est consignée. `anyandroid libcore natives --bootclasspath <dossier>` mesure la couverture.

Liées à ce jour : `System` (temps, `arraycopy` et variantes typées, journal, `setOut0`…),
`Runtime`, `Math` et `StrictMath`, `Float`/`Double`, `StringFactory`, `String` (`fillBytes*`…),
`Character` (partiel), `VMRuntime`, `VMStack`, `OsConstants`/`OsConstantsHolder`,
`libcore.io.Linux` (sorties standard, identité, environnement, `uname`, `sysconf`, `strerror`,
fdsan), `FileDescriptor`, `libcore.icu.ICU` (versions, locale fixe `en-US`), `sun.misc.Version`,
`jdk.internal.misc.VM`, `sun.misc.Unsafe` et `jdk.internal.misc.Unsafe` (accès aux champs et
tableaux par décalages opaques, CAS, `compareAndExchange`, `park`/`unpark` ; la mémoire brute
`allocateMemory`/`getInt(long)` n'est **pas** exposée), natives ART de `Class` (réflexion,
`getPrimitiveClass`, `classForName`…), de `Reference` et d'`Array`.

**Convertisseurs de caractères** (`libcore_charsets.cpp`) : `NativeConverter` (ICU4C sur Android)
est réimplémenté sans ICU pour UTF-8, US-ASCII, ISO-8859-1, UTF-16(BE/LE) et UTF-32(BE/LE), avec la
sémantique exacte attendue par `CharsetEncoderICU`/`CharsetDecoderICU` (décalages, unités
invalides, codes d'erreur ICU, actions signaler/ignorer/remplacer, marques d'ordre des octets).
Les autres jeux de caractères sont déclarés non pris en charge, comme ICU le fait pour un nom
inconnu. Les poignées de convertisseur sont des index bornés.

**Annotations** : les natives qui lisent les annotations DEX (`getSignatureAnnotation`,
`isAnonymousClass`, `getDeclaredAnnotations`…) répondent « aucune annotation » et sont signalées
comme simulées (journal) : la lecture des annotations DEX reste à faire.

**Isolation** : le code exécuté ne voit que les sorties standard, un environnement Android fixe
(`ANDROID_ROOT`, `ANDROID_ART_ROOT`…), une identité d'application fixe (UID 10000) et un `uname`
générique. Aucun fichier, variable d'environnement ni nom d'utilisateur de l'hôte n'est exposé.
L'accès aux fichiers passera par un système de fichiers virtuel (racine Android émulée) décrit
dans un ADR ultérieur.

## 4. Suite et risques

- **`MethodHandle.invoke`/`invokeExact`** (transformations, `EmulatedStackFrame`) : non pris en
  charge ; échec explicite.
- **Annotations DEX** : nécessaires à la réflexion générique et aux classes internes.
- **Fichiers** : livré, voir [ADR-005](ADR-005-systeme-de-fichiers-virtuel.md) ; restent `mmap`,
  `ByteBuffer` directs et `readv`/`writev`.
- **ICU** : `java.util.Formatter` fonctionne déjà (`%d`, `%s`, `%.2f`) ; les locales, formats de
  dates et `android.icu` (données `icudt78l.dat`, lues par `mmap`) restent à faire.
- **Démarrage** : comme ART, `bridge::start_libcore()` initialise `System` avant le code de
  l'application ; sinon un programme qui commence par `Collections.sort` provoque un cycle
  d'initialisation (`VMRuntime` → `AtomicInteger` → `System` → `VMRuntime`).
- **Finaliseurs et files de références** : jamais déclenchés par le ramasse-miettes.
- **Couverture** : `anyandroid libcore natives --bootclasspath <dossier> --libcore-only` liste les
  natives manquantes (environ 690 sur 1 034 pour la libcore d'Android 17, surtout réseau, fichiers,
  NIO et ICU) ; `--core` liste les membres du cœur encore absents.

## 5. Chemin parcouru sur la vraie libcore

Itérations successives sur `hello-dex` avec l'image réelle : `Unsafe.addressSize`, puis les
`VarHandle` d'`AtomicInteger` (`invoke-polymorphic`), `Class.getPrimitiveClass` et la fusion des
classes du cœur, les natives `Linux` renommées d'Android 17, les versions ICU, `FileDescriptor`, les
jeux de caractères standards (y compris UTF-32, nouveau dans `StandardCharsets`),
`NativeAllocationRegistry` (qui exige le `BootClassLoader`), `Reference` et `ReferenceQueue`, les
chaînes compactes de `StringBuilder`, la redirection `StringFactory`. `System.<clinit>` s'exécute
alors entièrement (environ 35 000 instructions) et le programme complet en environ 204 000.

## 6. Tests

Images synthétiques construites par les tests (`tests/unit/test_image.cpp`) selon les formats
mke2fs, mkfs.erofs, libsparse et liblp : toutes les dispositions de blocs ext4, EROFS plat, en
ligne, découpé et compressé (index complets et compacts, grands pclusters, queue en ligne, avec et
sans *zero padding*), images *sparse* et *super*, APEX et `.capex`, extraction complète d'un
chemin d'amorçage et refus d'une traversée de chemin. Mode libcore (`tests/unit/test_libcore.cpp`) :
priorité du chemin d'amorçage, cœur natif préservé, fusion du bytecode et redirection
`StringFactory`, champ `count` de `String`, liaison des natives, `OsConstants`, `ErrnoException`,
types encapsulés, `Unsafe`, `VarHandle` (champ, statique, tableau, types encapsulés, champ final,
identifiant forgé), convertisseurs de caractères (encodage, débordement, non représentable,
malformé).

Validation sur l'image réelle : les 53 archives du chemin d'amorçage (dont les 6 sections DEX 041 de
`framework.jar`) se relisent sans erreur de CRC, ce qui valide les lecteurs ext4, APEX compressés
et DEX 041 sur des données réelles.
