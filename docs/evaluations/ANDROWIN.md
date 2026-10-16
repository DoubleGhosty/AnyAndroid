# Évaluation d'ANDROWIN (« ntdroid ») pour AnyAndroid

- **Statut** : évaluation, pas une décision. Aide à préparer un futur ADR sur la couche native (G4).
- **Date** : 2026-10-16
- **Source** : projet ANDROWIN du porteur (Google Drive, activité du 2026-08-06 au 2026-09-03).
  Analyse sur pièces : lecture intégrale de README, HANDOFF, ARCHITECTURE, PHASE1–6,
  DECISIONS.md (538 lignes), et du code mémoire/binder/fork/tests. **Rien n'a été exécuté**
  (Windows/MSVC requis) : on distingue partout « vérifié dans le code » de « affirmé par les docs ».

## 1. Ce qu'est ANDROWIN

Un runtime **Windows x64 (C++20/MSVC)** qui exécute des **binaires ELF Linux x86-64 non modifiés**
dans un processus Windows ordinaire — **sans VM, sans hyperviseur, sans WSL, sans émulation
d'instructions**. Le code invité s'exécute directement sur le CPU ; seuls ses **appels système**
sont interceptés et servis par une réimplémentation en espace utilisateur de l'ABI du noyau Linux.
C'est, dans l'esprit, un « WSL 1 sans pilote noyau ».

Mécanisme (vérifié dans le code et les docs) :
- chaque instruction `syscall` est réécrite en `ud2` (déroutement `#UD` capté par un gestionnaire
  d'exceptions vectoriel) ; quand le motif `mov eax, imm32; syscall` précède, le site est fusionné
  en un saut vers un trampoline direct (mesure docs : 14,3 M appels/s contre 450 k/s par le piège) ;
- le TLS `%fs` repose sur FSGSBASE et un réarmement sur faute ; `%gs` (registre de thread d'ART,
  qui désigne le TEB sous Windows) est réécrit site par site (~300 000 sites dans libart + les .oat,
  et le code JIT scanné à sa première exécution) ;
- mémoire : arène de 8 Gio, bitmap de pages (réservation Windows 64 Kio vs page Linux 4 Kio),
  mmap paresseux ; fork = **vrai second processus Windows** qui reçoit une copie de la mémoire ;
  binder = pilote en processus + broker inter-processus sur tubes nommés ; propriétés lues dans le
  `build.prop` de l'image ; sockets `AF_UNIX` sur tubes nommés ; horloge monotone unique.

**Point capital : ntdroid exécute le VRAI espace utilisateur d'Android** (linker64, bionic, libart,
app_process, zygote, servicemanager, démons). C'est **l'inverse d'AnyAndroid**, qui réimplémente
l'interpréteur Dalvik, la libcore et le framework en C++. Chez ntdroid, ART et bionic sont les vrais ;
seule la frontière avec le noyau est réécrite.

## 2. Où ça en est réellement

**Atteint et testé** (cas présents dans `scripts/test.ps1`) : binaires statiques (busybox),
binaires dynamiques sur la vraie bionic (toybox), threads, signaux, `dalvikvm -Xint` sur un dex,
trampoline direct, binder entre 3 processus (`service list`/`service call` via le vrai
servicemanager), `incidentd`/`storaged` non modifiés qui s'enregistrent, mort de participant
(`BR_DEAD_BINDER`), fork en second processus, socket `AF_UNIX` stream, horloge haute résolution,
et **le critère « essentiel » de la phase 4 (2026-08-20) : `app_process` exécute `RuntimeInit.main`
puis une classe `Hello` d'un dex et sort proprement**. Tests anti-intermittence (JIT 6/6, threads 10/10).

**Affirmé par les docs, mais SANS test associé** (à ne pas tenir pour acquis) : le zygote préchargeant
6 535 classes en ~1 s (8/8), la JIT forcée 16/16, le fork accéléré ×10 par mmap paresseux, le fork de
`system_server`.

**PAS atteint, de l'aveu même du projet** : aucune **Activity**, aucun **Context**, aucun **APK tiers**
lancé. `system_server` est forké puis sort en `exit_group(0)` **avant toute ligne de Java**. Le critère
de la phase 6 (« une appli du Play Store obtient un Context et exécute son `onCreate` ») n'est pas
atteint. Restent ouverts : `SCM_RIGHTS`, transactions binder imbriquées, récupération des tampons
binder, pool de threads binder.

**Conclusion de maturité** : ntdroid est **plus avancé qu'AnyAndroid sur un axe** (il fait tourner le
vrai ART avec JIT et du vrai code natif), mais **pas plus avancé sur le but commun** « lancer une
appli » : lui non plus n'a ni Context, ni Activity, ni rendu. Il n'a ni affichage, ni fenêtre, ni
saisie, ni audio, ni réseau, ni traduction ARM.

## 3. Limites structurelles (décisives)

1. **x86-64 uniquement, aucune traduction ARM.** La plupart des `.so` du Play Store et de GmsCore
   sont **arm64-v8a**. ntdroid met `ro.dalvik.vm.native.bridge=0` : il ne couvrirait que les
   applications livrant des `.so` x86_64. (Notre ADR-002 §6 avait déjà identifié cette borne.)
2. **Android 9 (API 28), pas Android 17.** Tout est validé sur l'image SDK API 28. Android 17 apporte
   l'ELF TLS, des zones de propriétés éclatées, et bionic/linker/ART en APEX. Les hypothèses expirent.
3. **Aucun bac à sable.** « The guest is not sandboxed » : l'invité partage l'espace d'adressage du
   runtime, un pointeur invité est un pointeur hôte, et un site `%gs` oublié écrit silencieusement
   dans le TEB Windows. **Cela contredit frontalement notre modèle de menaces** (objets natifs vus
   comme des identifiants bornés, jamais des pointeurs ; code Android non fiable).
4. **Windows + MSVC.** SEH `__try/__except`, émission de trampolines, `/permissive-`. Notre chaîne est
   **MinGW/GCC cross-compilé + wine** : le portage n'est pas trivial, et plusieurs mécanismes reposent
   sur un comportement de Windows **non contractuel** (remise à zéro de la base FS à la préemption).
5. **Preuves inégales.** Les résultats les plus spectaculaires (zygote, `system_server`, ×10) sont
   documentés sans test ; les docs ont des strates contradictoires ; aucune référence différentielle
   (aucun vrai Android ne démarre sur la machine de l'auteur, à cause de VBS/Credential Guard) ;
   mesures issues d'une seule machine.

## 4. Posture GApps / intégrité / licences

- ntdroid **n'aborde jamais** Play Integrity, SafetyNet ni l'attestation (recherche plein texte : 0).
  Son « honnêteté » concerne la véracité des résultats (ne rien simuler sans le consigner), **pas** la
  certification d'appareil. Les propriétés exposées sont celles de l'image, sans usurpation. →
  **Cohérent avec nos garde-fous** (ADR-002 §5) : une appli exigeant l'intégrité « appareil » reste
  refusée dans les deux projets.
- Redistribution : posture **compatible avec la nôtre** — aucun binaire tiers versionné (busybox
  épinglé en SHA, image API 28 téléchargée à la demande et archivée hors dépôt). **Réserve** : le
  script de récupération d'image ne présente aucune acceptation de licence du SDK Android (le nôtre,
  `tools/fetch-android-inputs.sh`, le fait pour la GSI — à conserver si on reprend un chemin d'entrées).

## 5. Ce qui est réutilisable (du plus sûr au plus lourd)

### Gains immédiats, faible risque, dans notre périmètre actuel
Ce sont de **vrais bugs latents chez nous**, mis en évidence par les mesures de ntdroid :

- **Attentes haute résolution.** Nos attentes temporisées passent toutes par
  `cv.wait_for(..., std::chrono::milliseconds)` (`runtime/dex-engine/threads.cpp:133`, utilisé par
  `sleep`, `park_current`, `join`, et `MessageQueue.nativePollOnce`). Sur Windows réel, risque
  d'arrondi au tick de ~15,6 ms. ntdroid a mesuré `poll(1 ms)` à **16,7 ms** : un Looper/animations
  piloté par délai tournerait ~16× trop lentement, **sans rien dans les journaux**. → Mesurer sous
  Windows (test façon `clockcheck` : demander 1 ms, mesurer), puis, si confirmé, une horloge
  monotone unique + attente par timer haute résolution. Concerne directement J5 (Looper).
- **Sémantique POSIX de fichiers sur NTFS.** `runtime/vfs/vfs.cpp` ouvre bien en `FILE_SHARE_DELETE`
  (l.527) mais supprime/renomme via `std::filesystem` (l.555, l.562), sans sémantique POSIX sur
  fichier ouvert. SQLite, `AtomicFile`, `SharedPreferences` font des `rename` par-dessus des fichiers
  ouverts. → Tests (unlink d'un fichier ouvert ; rename par-dessus une cible ouverte) et, s'ils
  échouent, `FILE_DISPOSITION_POSIX_SEMANTICS` / `FileRenameInfoEx`. Nécessaire pour `/data` dès J4.
- **Registre d'« honnêteté » à l'exécution.** Complète nos statuts statiques (`Supportée`/`Partielle`/
  `Stub` de `NativeRegistry`) par ce qui a réellement été substitué pendant une exécution, et un test
  qui exige l'aveu. Aligné avec nos principes et notre outil `gaps`.
- **Hygiène de tests** : sauts nommés et comptés, mécanismes sujets aux courses répétés N fois (×10,
  ×6) avec toutes les exécutions exigées. Applicable tel quel à notre ctest.
- **Recensement des collisions de casse à l'extraction** (ntdroid a perdu 5 fichiers sur 3 333 en
  silence). Notre lecteur d'images lit en place, mais notre bac à sable `/data` sur NTFS y est exposé.

### Brique lourde, décision stratégique : la couche native G4
ntdroid est une **preuve de faisabilité directe de notre G4** (« chargeur ELF x86_64 + couche
bionic → Windows », ADR-002 §6). Il **délègue au linker64 et à la bionic d'origine** `dlopen`, les
relocations, les IFUNC, le TLS — au lieu d'écrire une couche bionic→Windows. Réutilisables presque
tels quels : l'arène mémoire 4K/64K et `guest_mem` (avec ses tests), le décodeur de longueurs x86 et
le patcher (avec leurs tests), la conception du broker binder (pour G5 multiprocessus) et ses pièges
documentés, le générateur de zone de propriétés bionic, les outils de diagnostic.
**Coût élevé**, **risque élevé** : voir §3 (ARM, API 28→17, pas de bac à sable, MSVC→MinGW,
`%gs` oublié = corruption silencieuse de l'hôte).

### À documenter, NE PAS adopter maintenant : exécuter le vrai ART
Exécuter le vrai libart/app_process/zygote de l'image (réécriture `%gs` + scan JIT) **au lieu de**
notre interpréteur Dalvik donnerait une **JIT** — la performance dont GmsCore a besoin (ADR-002 §2.1).
Mais cela **remettrait en cause G1→G3** (interpréteur, libcore et framework d'Android 17 déjà branchés),
reste x86_64/API 28, sans bac à sable, et **n'a pas obtenu de Context non plus**. Très élevé / très élevé.

## 6. Comparaison synthétique

| Axe | AnyAndroid | ANDROWIN (ntdroid) |
|---|---|---|
| Principe | Réimplémente Dalvik + libcore + framework en C++ | Exécute le vrai ART/bionic ; réécrit la frontière noyau |
| Cible Android | 17 (GSI) | 9 (API 28) |
| ABI | indépendant (interprète le DEX) | **x86-64 seulement** ; pas d'ARM |
| Perf code managé | interpréteur, **pas de JIT** | **vraie JIT ART** |
| Code natif `.so` | G4 à faire (0 %) | **fonctionne** (x86_64) |
| Rendu / fenêtre / entrées | en cours (Win32, Skia visé) | **absent** |
| Bac à sable | oui (identifiants bornés) | **non** (pointeur invité = pointeur hôte) |
| Chaîne | MinGW/GCC + wine, CI Linux | MSVC, Windows seulement |
| Lancer une appli (Context/onCreate) | pas encore (J4, bloqué sur drawables) | **pas encore non plus** |
| Intégrité / honnêteté | garde-fous explicites | n'aborde pas l'intégrité ; honnêteté des résultats |

## 7. Recommandation

1. **Ne pas changer de cap maintenant.** AnyAndroid garde ses acquis (G1–G3 : runtime, vraie libcore
   et vrai framework d'Android 17, bac à sable, rendu en cours, CI Linux/wine). ntdroid ne lance pas
   d'appli plus que nous et ajouterait les contraintes x86_64/API 28/MSVC/pas-de-bac-à-sable.
2. **Récolter tout de suite les gains sûrs** (§5, bloc 1) : attentes haute résolution, sémantique
   POSIX du VFS, registre d'honnêteté, hygiène de tests, recensement de casse. Ce sont des
   corrections de bugs latents, pas une importation d'architecture.
3. **Retenir ntdroid comme l'implémentation de référence de G4** (couche native ELF). Quand on
   attaquera G4, partir de son approche (bionic d'origine + interception des syscalls) plutôt que
   d'écrire une couche bionic→Windows, en l'isolant **dans un processus hôte séparé** (p. ex.
   AppContainer) relié à notre interpréteur par JNI — ce qui lève l'objection « pas de bac à sable ».
   Portée réaliste : les applis à `.so` x86_64 ; l'ARM reste hors périmètre comme prévu.
4. **Garder en archive l'option « vrai ART »** (§5, bloc 3), sans l'ouvrir : elle n'est justifiée que
   si l'interpréteur sans JIT se révèle trop lent pour GmsCore, et elle sacrifierait G1–G3.

## 8. Expériences courtes qui trancheraient

- **Mesurer la résolution réelle de nos attentes sur un vrai Windows** (pas seulement wine) : confirme
  ou non le bug d'arrondi à 15,6 ms.
- **Vérifier `unlink`/`rename` sur fichier ouvert** via notre VFS sur Windows : confirme le besoin de
  la sémantique POSIX.
- **Mesurer la part des `.so` x86_64 vs arm64** dans GmsCore et dans un échantillon d'APK du Play
  Store (notre outil `gaps`) : chiffre la couverture atteignable d'une G4 sans traduction ARM.
- **Prototype d'isolation** : faire tourner un `hello` natif x86_64 dans un processus hôte séparé
  (AppContainer) piloté par notre runtime, pour valider le modèle « G4 isolée » avant tout portage.
