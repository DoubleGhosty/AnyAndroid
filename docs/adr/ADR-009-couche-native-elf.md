# ADR-009 — Couche native ELF et chargeur x86_64 (G4)

- **Statut** : accepté le 2026-10-17 (étape G4, suite de l'ADR-002 §6 et de l'ADR-003 §5).
- **Contexte** : AnyAndroid a achevé l'étape G3 (J0 à J10) avec un runtime complet exécutant le
  `framework.jar` d'AOSP, la mesure/mise en page et le rendu logiciel Skia, ainsi que la gestion
  interactive des fenêtres et des entrées. La plupart des applications Android du monde réel
  embarquent des bibliothèques natives compilées via le NDK (`.so`). L'étape G4 vise à charger
  et exécuter ces bibliothèques natives x86_64 directement sur le CPU Windows sans émulation
  d'instructions ni machine virtuelle, en garantissant l'étanchéité JNI et le respect du modèle
  de sécurité.

## Décision

1. **Cible x86_64 native stricte** :
   AnyAndroid exécute les bibliothèques `.so` d'architecture x86_64 (ABI `x86_64` du NDK).
   Les architectures ARM/ARM64 restent hors périmètre direct (exigeraient un traducteur binaire
   dynamique lourd type FEX/Box64/libndk_translation).

2. **Chargeur ELF-64 AnyAndroid dédié** (`runtime/elf/`) :
   Un chargeur ELF dynamique x86_64 complet et modulaire est introduit :
   - validation structurelle des en-têtes ELF-64 (`Elf64_Ehdr`, `Elf64_Phdr`, `Elf64_Dyn`) ;
   - projection mémoire via `VirtualAlloc` avec alignement strict sur des pages de 4 Kio ;
   - application des protections mémoire conformes W^X (`PAGE_EXECUTE_READ` pour le code,
     `PAGE_READONLY` pour `.rodata` et `PT_GNU_RELRO`, `PAGE_READWRITE` pour les données) ;
   - moteur de réallocations dynamiques x86_64 (`R_X86_64_RELATIVE`, `R_X86_64_64`,
     `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`, GNU IFUNC `R_X86_64_IRELATIVE`) ;
   - résolution rapide de symboles via `DT_GNU_HASH` (filtre de Bloom + chaînes) et repli `DT_HASH` ;
   - exécution des initialiseurs `DT_INIT` et `DT_INIT_ARRAY` en convention System V AMD64.

3. **Approche Bionic pragmatique par shims System V** :
   Au lieu de charger la bionic binaire d'AOSP et d'intercepter des centaines d'appels système
   Linux non documentés (approche ANDROWIN/ntdroid), AnyAndroid fournit une couche d'émulation
   légère exportant l'ABI standard du NDK en convention `__attribute__((sysv_abi))` :
   - `liblog.so` : `__android_log_print`, `__android_log_write`, `__android_log_vprint` redirigés
     vers le journal structuré d'AnyAndroid (`common/log.h`) ;
   - `libm.so` : fonctions mathématiques directes de la libc (`sin`, `cos`, `pow`, `sqrt`…) ;
   - `libdl.so` : `dlopen`, `dlsym`, `dlclose`, `dlerror` reliés directement au chargeur ELF ;
   - `libc.so` : gestionnaire de mémoire (`malloc`, `free`, `realloc`, `calloc`) dans une arène
     dédiée bornée, opérations mémoire/chaînes (`memcpy`, `strlen`, `strcmp`…), primitives de
     synchronisation pthreads minimales (`pthread_mutex_*`, `pthread_once`), et entrées/sorties
     de fichiers (`open`, `read`, `write`, `close`) branchées sur le VFS (ADR-005).

4. **Intégration JNI complète et étanchéité du runtime** :
   - Connexion de `System.loadLibrary` et `Runtime.nativeLoad` à `ElfLinker` ;
   - Invocation de `JNI_OnLoad(JavaVM*, void*)` en convention System V ABI avec passage de `JavaVM` ;
   - Prise en charge des enregistrements dynamiques via `RegisterNatives` (déjà en place en G1) ;
   - Résolution dynamique des symboles `Java_<paquet>_<classe>_<methode>` pour les méthodes
     non explicitement pré-enregistrées ;
   - Libération du verrou global du runtime (`GIL`) pendant l'exécution du code natif invité,
     avec réacquisition automatique lors des rentrées JNI (`JniAccess`).

5. **Isolation et durcissement (Menaces M6, M10, M11)** :
   - Mémoire native bornée par quota dans une arène virtuelle dédiée ;
   - Gestionnaire d'exceptions vectorielles (VEH) interceptant les violations d'accès (`#AV`/`SIGSEGV`)
     pour remonter une exception Java propre (`InternalError`) au lieu de corrompre l'hôte ;
   - Évolution prévue vers un processus ouvrier (worker) doté d'un jeton restreint (AppContainer /
     Low Integrity) pour l'exécution non fiable.

## Plan de découpage incrémental (G4-J1 à G4-J8)

- **G4-J1** : Parseur ELF-64 et validation de structure (`elf_types.h`, `elf_reader.h`/`.cpp`).
- **G4-J2** : Projection mémoire et gestion des segments `PT_LOAD` (`elf_image.h`/`.cpp`).
- **G4-J3** : Moteur de réallocations dynamiques x86_64 (`elf_reloc.h`/`.cpp`).
- **G4-J4** : Résolution de symboles (`DT_GNU_HASH`/`DT_HASH`), `DT_NEEDED` et initialiseurs.
- **G4-J5** : Couche de shims Bionic System V (`liblog`, `libm`, `libdl`, `libc`).
- **G4-J6** : Intégration JNI (`Runtime.nativeLoad`, `JNI_OnLoad`, résolution dynamique).
- **G4-J7** : Extraction et chargement depuis l'APK (`lib/x86_64/*.so`) et `PathClassLoader`.
- **G4-J8** : Durcissement, gestionnaire d'exceptions VEH et validation sur un APK NDK réel.

## Alternatives écartées

- **Délégation totale au linker64/bionic d'AOSP avec déroutement de syscalls (`syscall` → `ud2` / trampoline)** :
  Approche explorée par ANDROWIN. Écartée pour AnyAndroid car elle brise la portabilité MinGW/GCC
  (nécessite des trampolines assembleur spécifiques MSVC), expose l'hôte à l'écrasement silencieux
  du TEB Windows via `%gs` (l'invité partage tout l'espace d'adressage), et contourne le VFS
  d'AnyAndroid.
- **Émulation CPU (QEMU / Box64 / FEX)** :
  Inutile et contre-productif pour les binaires x86_64 sur hôte x86_64 : introduirait une perte de
  performances de 5× à 10× et une complexité de dépendances disproportionnée.
- **Chargement par `LoadLibrary` Win32** :
  Impossible : les bibliothèques Android sont au format ELF-64 et ciblent la convention d'appel
  System V AMD64 et la bionic libc, alors que Windows n'accepte que le format PE/COFF et la Win64 ABI.

## Conséquences

- Aucune régression sur le code Dalvik, libcore et framework existant (G1, G2, G3).
- Architecture modulaire testable en mémoire avec des tests unitaires rapides et autonomes.
- Préparation de l'écosystème pour l'exécution d'applications NDK réelles et, à terme, de microG / GApps.
