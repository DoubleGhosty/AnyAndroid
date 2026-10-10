# ADR-003 — Runtime G1 : threads, ramasse-miettes, réflexion et JNI

- **Statut** : accepté (étape G1 de l'ADR-002, livrée le 2026-10-09)
- **Contexte** : exécuter du code Java réel, puis le code d'AOSP (G2/G3) et les bibliothèques natives
  x86_64 (G4), exige un runtime multi-thread, à mémoire gérée, réflexif et doté de JNI.

## 1. Threads : verrou global de l'interpréteur (GIL)

**Décision.** Chaque thread Java est un vrai thread Windows (`std::thread`). Un verrou global
(`Runtime::gil_`) sérialise l'exécution du code Java, l'accès au tas et à l'éditeur de liens.

- Le verrou est relâché aux **points bloquants** : `Thread.sleep`, `Object.wait`, `Thread.join`,
  moniteur contesté, initialisation de classe en cours dans un autre thread, et pendant
  l'exécution du **code natif JNI**.
- Aux **points de sûreté** (entrée de méthode et branchements arrière), un thread cède le verrou
  si d'autres l'attendent (toutes les 1 000 vérifications), déclenche le ramasse-miettes si
  nécessaire et s'arrête si le runtime s'éteint.
- Les moniteurs Java (`synchronized`, `monitor-enter/exit`, `wait/notify/notifyAll`) utilisent
  des variables de condition associées au verrou global ; l'interruption (`Thread.interrupt`)
  réveille `sleep`, `wait` et `join`.
- Toute entrée dans le runtime passe par `Runtime::Scope` : attachement du thread appelant et prise
  du verrou. L'hôte d'application en ouvre une par événement, et la boucle de messages Windows
  tourne sans le verrou.
- Arrêt : `~Runtime` positionne un drapeau ; les threads démarrés sont déroulés par une exception
  C++ interne (`ThreadKill`, jamais visible du code Java), puis joints. `exec` attend d'abord les
  threads non démons, comme la JVM.

**Pourquoi.** C'est le modèle le plus sûr pour un runtime dont le tas et les caches ne sont pas
conçus pour la concurrence : la sémantique Java (visibilité, ordre) est respectée trivialement,
sans risque de corruption mémoire de l'hôte. **Limite assumée** : pas de parallélisme pour le code
Java (le code natif, lui, tourne en parallèle). Un modèle à verrous fins est une évolution
possible, à mesurer d'abord avec `anyandroid gaps` et des profils réels.

**Règles Android reproduites** : modifier une vue depuis un autre thread lève
`CalledFromWrongThreadException` ; `Toast` et `new Handler()` exigent le thread UI ;
`runOnUiThread`, `View.post` et `Handler(Looper.getMainLooper()).post` font passer le travail sur
le thread UI. Une exception non rattrapée dans un thread arrête l'application, comme sur Android ;
en mode `exec`, elle est affichée comme par la JVM.

## 2. Mémoire : poignées générationnelles et ramasse-miettes mark-sweep

- Une référence est une poignée de 32 bits : **8 bits de génération + 24 bits d'index**. Un
  emplacement libéré change de génération, ce qui fait détecter les références périmées
  (`VerifyError`) au lieu de lire un autre objet.
- **Ramasse-miettes stop-the-world mark-sweep**, sans déplacement d'objets, exécuté par le thread
  qui détient le verrou. Il se déclenche quand le tas double (planchers configurables) ou en
  dernier recours avant `OutOfMemoryError`.
- **Racines précises** : miroirs de classes et champs statiques de type référence, chaînes
  résolues et internées, caches natifs, références globales (`GlobalRef`, JNI), fournisseurs de
  racines de l'hôte (activités, intents en attente), objets `Thread`, références locales JNI.
- **Racines conservatives** : registres de l'interpréteur, arguments des méthodes natives, piles
  natives des threads présents dans la VM (registres sauvegardés par `setjmp` au moment de céder le
  verrou). Toute valeur de 32 bits qui est une poignée valide, et tout pointeur de 64 bits vers un
  objet, maintient l'objet en vie. Les générations initiales sont dispersées pour limiter les
  fausses racines.
- **Traçage du tas précis** : champs de type référence (`Class::ref_slots`), tableaux d'objets,
  état natif (`NativePeer::trace`). `WeakReference` est vidée quand son référent meurt ;
  `SoftReference` est conservée (pas de notion de pression mémoire) ; les finaliseurs ne sont
  jamais appelés.
- Règle pour le code natif du pont : tout état natif qui garde une poignée la déclare dans
  `trace()`, et toute tâche différée la capture dans une `GlobalRef`.

## 3. Réflexion

`Class.forName`, `getDeclared*` et les autres accesseurs, `Method.invoke` (répartition virtuelle,
conversions élargissantes des types encapsulés, `InvocationTargetException`),
`Constructor.newInstance`, `Field.get`/`set`, `Array`, `Modifier`, `ClassLoader` minimal,
`Integer.TYPE` et les autres classes primitives. Aucun contrôle d'accès n'est appliqué
(`setAccessible` est toujours accepté). Les **annotations** sont refusées explicitement : la
lecture des annotations DEX arrive en G2.

## 4. JNI

- Table JNI 1.6 complète (233 entrées, disposition vérifiée par `static_assert`) et JavaVM
  (`AttachCurrentThread`, `GetEnv`…).
- **Convention System V AMD64** pour toutes les fonctions JNI et les méthodes natives, puisque
  c'est celle des bibliothèques Android x86_64 (G4).
- Appel d'une méthode native de signature quelconque : 6 entiers et 8 flottants en registres,
  jusqu'à 16 mots sur la pile (au-delà, refus explicite).
- Références locales (racines, libérées au retour), globales et globales faibles distinguées par
  une étiquette dans le pointeur `jobject`.
- Une exception Java ne traverse jamais le code natif : elle devient l'« exception en attente »
  du `JNIEnv` et est relancée au retour dans la VM.
- Le verrou global est relâché pendant le code natif et repris par chaque fonction JNI.
- `RegisterNatives` est disponible. La résolution par nom de symbole (`Java_pkg_Classe_methode`)
  et `JNI_OnLoad` arriveront avec le chargeur ELF (G4).

## 5. Contraintes de la chaîne de compilation (MinGW-w64 / GCC 15)

- Une fonction `sysv_abi` ne doit contenir **aucune gestion d'exceptions C++** : GCC n'émet pas de
  données de déroulement SEH valides pour elle, et l'assembleur échoue. Les points d'entrée JNI
  sont donc de minces trampolines vers des fonctions Windows `noexcept`.
- `setjmp` et un appel `sysv_abi` dans la même fonction provoquent une erreur interne de GCC : ils
  sont isolés dans des fonctions distinctes.
- **Pas d'objet `thread_local` avec destructeur** : avec un runtime MinGW lié statiquement, cela
  corrompait le tas à la fin des threads (corrigé et couvert par un test de bout en bout).

## Conséquences

Critère de sortie G1 atteint. Les tests couvrent :
- un compteur partagé et un producteur-consommateur ;
- les interruptions et un gestionnaire d'exceptions non rattrapées ;
- l'arrêt d'un thread en boucle infinie ;
- 200 000 allocations sous un plafond de 50 000 objets ;
- les références faibles et les poignées périmées ;
- la réflexion depuis du bytecode ;
- JNI : arguments sur la pile, appels variadiques, chaînes MUTF-8, champs, exceptions dans les deux
  sens et références.

La démo `threads-demo` (4 threads) a réussi 20 exécutions sur 20. Étape suivante : **G2**, faire
tourner la vraie libcore d'AOSP sur ce runtime.
