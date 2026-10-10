# Contribuer

## Règles (cadrage §13.3)

- Chaque composant a un contrat et des tests ; toute correction ajoute un test de régression.
- Toute API Android ajoutée au pont déclare un **statut** (`Supportée`, `Partielle`, `Stub`,
  `Non supportée`) et une note si nécessaire. Régénérez ensuite la matrice :
  `anyandroid compat --markdown > docs/compatibility/api-matrix.md`.
- Une API absente doit **échouer explicitement** (`ClassBuilder::unsupported`) et non simuler un
  succès. Une API de sécurité n'est jamais un stub permissif.
- Toute décision structurante est consignée dans un ADR (`docs/adr/`).
- Les dépendances tierces ne sont ajoutées qu'après revue de licence, de maintenance et de sécurité.
  Aucun binaire propriétaire (Google ou autre) dans le dépôt.

## Ajouter une API au pont

1. Trouvez la classe dans `runtime/framework-bridge/` (ou créez-la avec `NativeRegistry::define`).
2. Ajoutez la méthode avec sa signature DEX exacte, par exemple
   `b.method("setText", "(Ljava/lang/CharSequence;)V", fn)`.
3. Écrivez un test : test unitaire (smali dans `tests/unit/test_runtime.cpp`) ou scénario de bout en
   bout (APK de `samples/` + script *headless* + sortie de référence dans `tests/e2e/expected/`).

## Règles du runtime (ADR-003)

- Tout appel au runtime depuis du code C++ se fait dans un `rt::Runtime::Scope`.
- Un état natif (`NativePeer`) qui garde une poignée Java l'expose dans `trace()` ; une tâche
  différée capture ses objets dans une `rt::GlobalRef`.
- Une fonction `JNI_ABI` (System V) ne contient aucune gestion d'exceptions C++ : passez par un
  utilitaire Windows `noexcept` (voir `guarded()` dans `jni.cpp`).
- Pas d'objet `thread_local` avec destructeur (corruption du tas avec MinGW lié statiquement).

## Méthodes natives de la libcore (ADR-004)

- Une native AOSP s'ajoute dans `runtime/framework-bridge/libcore_natives.cpp` avec
  `NativeRegistry::bind(classe, nom, signature, fn)`, signature DEX exacte. Vérifiez la couverture
  avec `anyandroid libcore natives --bootclasspath <dossier>`.
- Rien de l'hôte ne doit fuiter vers le code émulé : ni fichier, ni variable d'environnement, ni nom
  d'utilisateur (voir M14 du modèle de menace).
- Les fichiers extraits d'une image système (libcore, framework) ne vont **jamais** dans le dépôt.

## Style

C++20, `clang-format` (style Google, 120 colonnes), commentaires en anglais, messages destinés à
l'utilisateur en français. Les avertissements du compilateur sont traités comme des erreurs en CI
(`-DAA_WARNINGS_AS_ERRORS=ON`).

## Types d'issues

bug · API manquante · demande de compatibilité (joindre la sortie de `anyandroid inspect --json`) ·
sécurité (voir `SECURITY.md`) · proposition d'architecture.
