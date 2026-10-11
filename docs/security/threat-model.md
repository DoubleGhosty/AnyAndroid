# Modèle de menace (prototype)

Ce document reprend le §12 du cadrage et indique l'état de chaque mesure. **AnyAndroid ne fournit
pas de bac à sable fort** : l'isolation n'a fait l'objet d'aucun audit indépendant.

## Menaces

| # | Menace | Mesures en place | État |
|---|---|---|---|
| M1 | APK malveillant exploitant un bug de parsing | lecteurs ZIP/AXML/DEX bornés (`ByteReader`), limites de taille, d'entrées et de profondeur ; fuzzing par mutation en CI (`aa_fuzz`) | en place |
| M2 | Bombe ZIP, entrées chevauchantes ou dupliquées | taille annoncée vérifiée avant décompression, taux de compression plafonné, chevauchements et doublons refusés | en place |
| M3 | Traversée de chemins (`../`) | noms dangereux signalés, jamais extraits | en place (aucune extraction dans le MVP) |
| M4 | Bytecode malformé visant la mémoire de l'hôte | vérification structurelle avant exécution (registres, index, cibles de branchement) ; contrôles dynamiques des types, des bornes et des poignées | en place ; pas de vérificateur de types complet |
| M5 | Épuisement mémoire ou CPU | plafond d'objets, de taille des tableaux, de profondeur de pile, budget d'instructions par événement (`--max-instructions`), budget global de la mémoire native émulée (1 Gio : `Unsafe.allocateMemory`, `mmap`) | en place |
| M6 | Bibliothèque native qui exécute du code arbitraire | aucune bibliothèque `.so` n'est chargée ; les APK concernés sont refusés | en place |
| M7 | Accès aux fichiers hors de l'espace privé | système de fichiers virtuel (ADR-005) : arborescence Android assemblée de montages ; image système et APEX en lecture seule ; écriture limitée au bac à sable de l'application ; `..` borné à la racine virtuelle ; noms encodés ; liens et jonctions de l'hôte jamais traversés ; aucune création de lien par l'application ; aucun chemin de l'hôte dans les messages | en place |
| M8 | Données personnelles dans les journaux | journaux locaux uniquement (stderr et JSONL) ; pas de télémétrie | en place |
| M9 | Exécution non voulue d'un APK | confirmation interactive obligatoire (`--yes` pour l'automatiser) ; refus en entrée non interactive | en place |
| M11 | Code natif JNI (G1) | l'interface JNI existe mais **aucune bibliothèque `.so` n'est chargée** tant que le chargeur ELF (G4) et l'isolation (M10) ne sont pas en place ; les exceptions Java ne traversent jamais le code natif | en place |
| M12 | Concurrence (G1) | verrou global : le tas et l'éditeur de liens ne sont jamais modifiés en parallèle ; poignées générationnelles contre les références périmées | en place |
| M13 | Image système malveillante ou corrompue (G2) | lecteurs sparse, super, ext4, EROFS, LZ4 et APEX bornés (`FormatError`) ; sommes SHA-256 liblp vérifiées ; profondeur de liens et d'extents limitée ; chemins issus de l'image validés avant écriture (`..`, absolus refusés) ; tailles par fichier et totale plafonnées ; aucune exécution pendant l'extraction | en place ; pas encore de fuzzing dédié |
| M14 | Code de la libcore AOSP exécuté (G2) | environnement Android fixe, identité fixe, locale fixe, `uname` générique : rien de l'hôte n'est exposé ; fichiers uniquement à travers le système de fichiers virtuel (M7) ; natives non implémentées refusées explicitement ; identifiants opaques et bornés (champs des `VarHandle`, convertisseurs de caractères, décalages `Unsafe`) à la place de pointeurs, si bien qu'un bytecode qui forge une valeur obtient une exception ; mémoire brute d'`Unsafe` (`allocateMemory`, `getInt(long)`) non exposée | en place |
| M10 | Processus compromis par l'APK | code de l'APK interprété dans le processus `anyandroid`, avec les droits de l'utilisateur et sans élévation (manifeste `asInvoker`) | **à faire** : processus de travail dédié avec jeton restreint (cadrage §6.3) |

## Conséquences de l'objectif GApps (ADR-002)

L'exécution de code natif (couche ELF) et de services privilégiés élargit fortement la surface
d'attaque. Avant l'étape G4, il faudra : un processus de travail par application avec un jeton
Windows restreint (AppContainer ou *low integrity*), une IPC minimale vers le gestionnaire, et une
revue de sécurité dédiée.
