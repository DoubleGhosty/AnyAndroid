# Consignes pour les agents IA qui travaillent sur AnyAndroid

Ce fichier est chargé automatiquement par Claude Code. Il complète
[docs/HANDOFF.md](docs/HANDOFF.md) (prompt de passation complet : lis-le en entier avant ta
première modification) et [docs/STATUS.md](docs/STATUS.md). Les règles ci-dessous viennent
d'erreurs réellement commises dans ce dépôt (voir HANDOFF §13) : elles ne sont pas négociables.

## 1. Plusieurs sessions travaillent en même temps

- Avant de commencer et avant chaque push : `git fetch origin && git rebase origin/main`.
  Lis les commits arrivés depuis ta dernière synchronisation (`git log HEAD@{1}..origin/main`)
  et ne refais pas un travail qu'une autre session vient de pousser.
- Ne pousse que sur `main` et sur la branche que ton environnement t'a attribuée. Jamais de
  `--force` sur `main`, jamais de push sur la branche d'une autre session.
- Un sujet en cours dans une autre session (entrées, compositeur, GC…) ne se réécrit pas en
  parallèle : appuie-toi sur son code, corrige-le par petits commits expliqués.

## 2. Un environnement complet, sinon tu ne déclares rien « fait »

Le projet ne se vérifie qu'avec les **entrées Android** (hors dépôt) et **R8** :

```bash
AA_CLI=<chemin de anyandroid.exe> tools/fetch-android-inputs.sh <dossier> --r8
cmake ... -DAA_LIBCORE_DIR=<dossier>/libcore17 -DAA_SYSTEM_IMAGE=<dossier>/aosp17/system.img \
          -DAA_R8_JAR=<dossier>/tools/r8-9.5.23.jar      # + un JDK >= 17 (javac)
```

- `ctest` doit annoncer **37 suites ou plus** (dont `e2e-launch`, `e2e-process-launch`,
  `e2e-host-probe`, `e2e-host-services`, `e2e-framework-demo`). Avec **27 suites**, tu n'as que
  la configuration de la CI : le vrai framework n'a pas été exécuté et le code Java d'hôte
  (`runtime/android-host`) n'a même pas été compilé.
- Si tu ne peux pas obtenir ces entrées : ne modifie pas le code Java d'hôte, et écris
  « non vérifié sur le vrai framework » dans le commit et dans STATUS.md pour tout comportement
  du framework que tu touches.

## 3. Ce qui prouve qu'une fonctionnalité marche

- **Une preuve, c'est une exécution du vrai `framework.jar`** : `anyandroid exec … ProcessLauncher`
  sur un APK d'exemple, avec la trace (`--trace-services`), l'image écrite (`--frame-out`) et
  des sondes de pixels (`image probe`, option FRAME des tests e2e). Pour une fonctionnalité
  visible (clic, retour, toast, dialogue, navigation), ajoute un test e2e de ce genre.
- Un test unitaire qui appelle les natives depuis un mini-framework smali (`TestReceiver`…) teste
  **le pont**, pas le comportement d'Android. Il ne suffit pas pour annoncer un jalon. Exemple à
  ne pas reproduire : le jalon J10 annoncé « atteint » avec un seul test unitaire simulé.
- La **CI Windows** (`.github/workflows/windows.yml`) est relue après **chaque** push (outils
  GitHub `actions_list` puis `get_job_logs`). Si elle est rouge sur ton commit, tu la répares
  avant toute autre chose. wine n'est pas Windows : un test qui ne passe que sous wine est faux.

## 4. Fidélité à Android 17

- Avant d'écrire une native, une surcharge de service ou une déclaration dans `stubs/`, vérifie
  la signature sur le **vrai** jar :
  `python3 -I tools/dev/jarapi.py <libcore17>/system/framework/framework.jar methods|natives|fields <classe>`.
  Les signatures d'Android 17 diffèrent de celles d'Android 13/14 que tu connais
  (ex. `activityStopped(…, HandoffActivityData, …)`, `IWindowSession.remove(IBinder)`).
- `stubs/` : uniquement les déclarations utilisées, signature exacte, **aucun initialiseur
  constant**, pas de `Parcelable` ni de corps inventés (runtime/android-host/README.md).
- `e2e-host-services` doit afficher « surcharges : … toutes reconnues ». Une méthode publique
  d'un service qui ne surcharge rien n'est jamais appelée par le framework : rends-la statique
  ou non publique.
- Reproduis l'algorithme AOSP (HWUI, minikin, libinput, InputDispatcher, WMS, NotificationManagerService…)
  à partir de sa source à la balise `android-17.0.0_r1`, constantes comprises (par exemple
  `dimAmount` lu dans `LayoutParams`, pas une constante « 60 % »).
- Fils d'exécution : aucun accès au runtime (objets Java, `MessageQueue`, `post_queue_callback`)
  depuis un fil de l'hôte (fenêtre Win32, minuteur) sans le verrou global
  (`rt::Runtime::Scope`). Le ramasse-miettes est conservateur : un test qui suppose qu'un objet
  est déjà inaccessible doit le créer dans un cadre séparé et vider les résidus de pile.

## 5. STATUS.md, README.md et pourcentages

- Un pourcentage ne monte que si une **preuve** l'accompagne (test e2e, exécution réelle). Pas
  de commit qui ne fait que monter des chiffres. « 100 % » signifie qu'il ne reste rien
  de connu à faire dans l'étape.
- Un jalon est « fait » quand son critère de l'ADR est démontré par un test sur le vrai
  framework (J10 = une vraie application AOSP affichée et utilisable, pas un test unitaire).
- Écris aussi ce qui échoue ou n'est pas vérifié. STATUS.md dit la vérité.

## 6. Commits

- Français **avec accents**, UTF-8 **sans BOM**, titre `type(étape/jalon): résumé`, corps qui
  explique le pourquoi et la preuve (tests lancés, nombres exacts).
- Un commit = une avancée cohérente (pas « backstack + fenêtres + focus + compositeur + docs +
  progression » dans un seul commit).
- Termine chaque message par les lignes d'attribution demandées par ton environnement
  (`Co-Authored-By: …` et `Claude-Session: …`).
- Fichiers CRLF (`git ls-files --eol`) : édition binaire uniquement. Références e2e
  (`tests/e2e/expected/`) : régénérées en lançant la commande exacte, jamais à la main.
- Étiquettes de `HandleTable` : prends la suivante dans HANDOFF §8 et mets la liste à jour ;
  `check_handle_kinds` doit rester vert.

## 7. Avant de dire « fait »

1. Signature et algorithme vérifiés sur Android 17 (ou partiel déclaré avec une note honnête).
2. Build sans avertissement ; `ctest` complet (37 suites ou plus) vert ; pour une fonctionnalité
   visible, un e2e sur le vrai framework ; chaque e2e nouveau lancé trois fois.
3. Diff relu en adversaire : indices des arguments natifs, verrous et GIL, fins de ligne,
   includes standard explicites (la CI utilise GCC 15).
4. STATUS.md et HANDOFF.md mis à jour avec ce qui est réellement vérifié.
5. Rebase, push, puis **CI Windows verte**.
