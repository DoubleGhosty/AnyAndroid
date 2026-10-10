# ADR-001 — Architecture du MVP : runtime Android-compatible (option B)

- **Statut** : accepté pour le MVP ; complété par l'ADR-002 pour l'objectif GApps
- **Date** : 2026-10-09

## Contexte

Le cadrage (§5.1, §11.2) compare trois options : (A) portage fidèle d'AOSP, (B) runtime
Android-compatible qui réimplémente un sous-ensemble d'API, (C) Android dans une machine virtuelle.
L'option C est exclue par définition : aucun hyperviseur, aucun noyau Linux invité. Le cadrage
recommande B pour la preuve de concept.

## Décision

Le MVP suit l'option B :

| Couche | Choix |
|---|---|
| Analyse d'APK | C++20, lecteur ZIP et décompresseur maison stricts, parseurs AXML et DEX bornés ; aucun code exécuté |
| Exécution | interpréteur Dalvik écrit pour le projet (pas d'ART) ; références sur 32 bits via une table de poignées ; vérification structurelle du bytecode avant exécution ; pas de ramasse-miettes |
| Framework | pont C++ déclaratif (`NativeRegistry`) : chaque membre porte un statut (Supportée, Partielle, Stub, Non supportée), qui sert à la fois à l'exécution, au rapport de compatibilité et à la matrice publiée |
| Cycle de vie | `AppHost` : pile d'activités, ordre des rappels conforme à Android, vérification des appels à `super`, plantage explicite |
| Affichage | arbre de vues neutre (`ui::Node`) ; backend Win32 (contrôles natifs, DPI par écran) et backend *headless* scriptable pour les tests |
| Outillage | `apkgen` : assembleur smali → DEX, compilateur manifeste → AXML, ZIP déterministe ; les APK de démonstration sont reconstruits depuis leurs sources à chaque build |

## Raisons

- Prototype progressif, testable de bout en bout sans SDK Android.
- Aucune dépendance au noyau Linux ni à du code tiers difficile à porter.
- Surface d'attaque maîtrisée (§12) : tous les parseurs sont bornés et testés par mutation.

## Conséquences

- La compatibilité est **partielle par construction** : toute API absente échoue explicitement
  (`NoSuchMethodError` préfixée `[AnyAndroid]`) et est signalée dans le journal et dans
  `anyandroid inspect`.
- Limites connues : pas de threads, pas de ramasse-miettes, pas de ressources compilées
  (`resources.arsc`), pas de code natif, rendu limité aux contrôles standard.
- Ce choix **ne passe pas à l'échelle des GApps** : voir l'ADR-002, qui propose d'exécuter le code
  Java d'AOSP sur la machine virtuelle en conservant l'outillage, l'interpréteur et les tests.
