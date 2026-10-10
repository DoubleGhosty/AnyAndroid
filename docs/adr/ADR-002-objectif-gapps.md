# ADR-002 — Objectif prioritaire : faire fonctionner les GApps

- **Statut** : accepté le 2026-10-09 — D1 : voie B puis A ; D3 : pivot AOSP accepté ; D2 (APK) en attente ; licence Apache-2.0 proposée, à confirmer
- **Date** : 2026-10-09
- **Remplace partiellement** : document de cadrage §4.2 (« Google Play Services … exclus du premier périmètre »)

## 1. Contexte

Le porteur du projet a fixé un nouvel objectif prioritaire : faire fonctionner les **GApps**
(Google Mobile Services) sur AnyAndroid, c'est-à-dire au minimum :

| Paquet | Rôle |
|---|---|
| `com.google.android.gsf` (Google Services Framework) | identifiant d'appareil (GSF ID), fournisseur `gservices`, enregistrement (*checkin*) |
| `com.google.android.gms` (Google Play Services, « GmsCore ») | API clientes de Google (comptes, notifications push FCM, localisation, Maps, Sign-In, SafetyNet/Play Integrity…) |
| `com.android.vending` (Play Store) | catalogue, installation et mises à jour |
| applications Google (Gmail, Maps, YouTube…) | clientes des services ci-dessus |

Le document de cadrage excluait explicitement ces paquets du premier périmètre (§4.2) et
interdisait d'intégrer des binaires propriétaires sans droits (§12.2). Cet ADR documente ce que
le nouvel objectif implique, ce qui ne change pas, et les décisions à prendre.

## 2. Ce que les GApps exigent de la plateforme

Les exigences ci-dessous viennent de l'architecture publique de GMS et d'Android. Elles seront
**mesurées précisément** sur les APK réels avec `anyandroid gaps` (§6, étape G0) :

1. **Volume de code** : GmsCore compte une dizaine de fichiers DEX et des centaines de milliers de
   méthodes. Il faut un runtime fidèle et performant (threads, ramasse-miettes, réflexion,
   `invoke-custom`/`MethodHandle`) ; un interpréteur sans JIT sera lent au démarrage.
2. **Code natif** : de nombreuses bibliothèques `.so` (Cronet, TensorFlow Lite, chiffrement,
   DroidGuard…). Des variantes **x86_64** existent (émulateurs, Chromebooks) et peuvent s'exécuter
   nativement sur Windows x64 *après* l'écriture d'un chargeur ELF et d'une couche bionic → Windows.
   Les bibliothèques ARM64 seules imposeraient une traduction binaire, hors périmètre à ce stade.
3. **Binder / AIDL partout** : toutes les API clientes de GMS passent par `bindService` et des
   interfaces AIDL. Il faut Binder (en processus puis inter-processus), `Parcel`, `ServiceManager`.
4. **Statut d'application système** : GmsCore et GSF partagent l'UID `com.google.uid.shared`, sont
   *persistent*, demandent des permissions `signature|privileged` (liste blanche
   `privapp-permissions`) et déclarent plusieurs processus. Il faut donc un gestionnaire de paquets
   avec signatures, UID partagés, installation « privilégiée » et un modèle multi-processus.
5. **Services du système** : AccountManager (authentificateur Google), ContentResolver et
   fournisseurs de contenu, SyncManager, JobScheduler, AlarmManager, notifications, Keystore,
   connectivité, WebView (flux de connexion au compte).
6. **Interface** : le Play Store et les applications Google utilisent des layouts compilés,
   AppCompat/Material et du dessin personnalisé. Le rendu par contrôles Win32 natifs du MVP ne suffit
   pas : il faut le vrai pipeline de vues Android (Canvas/Skia) dessiné dans une fenêtre Windows.
7. **Réseau et TLS** : sockets, HTTP/2, Conscrypt/BoringSSL, connexion persistante FCM.

## 3. Conséquence d'architecture : le pont écrit à la main ne passera pas à l'échelle

Le MVP (ADR-001) réimplémente en C++ un sous-ensemble d'API Android (environ 700 membres). Les
GApps en référencent **des dizaines de milliers**, avec une sémantique fine. Les réécrire une à une
n'est pas réaliste.

**Proposition** : faire évoluer AnyAndroid vers l'option « A hybride » du cadrage (§5.1) :

- exécuter **le code Java d'AOSP lui-même** (bibliothèques de base `core-oj`/`core-libart`, puis
  `framework.jar` et les services système écrits en Java) sur la machine virtuelle d'AnyAndroid ;
- n'écrire en C++ que la **couche native** située dessous (méthodes JNI de libcore et
  d'`android_runtime`, Binder, AssetManager/ressources, graphismes), traduite vers Windows ;
- réutiliser les bibliothèques C/C++ portables d'AOSP quand la licence le permet (libandroidfw pour
  les ressources, Skia pour le dessin, BoringSSL/Conscrypt, zlib, ICU).

Le pont C++ actuel reste utile pour amorcer le runtime, pour les tests et comme repli. L'inspecteur,
l'interpréteur, l'outillage et les tests sont conservés.

Un projet existant suit une démarche comparable sous Linux : *Android Translation Layer* (ART +
framework réimplémenté + traduction de bionic). Il est à **auditer** (code, licence GPL, maturité)
avant d'écrire la couche native, conformément au §11 du cadrage.

## 4. Deux voies pour « les GApps »

| | Voie A — GApps propriétaires | Voie B — microG d'abord |
|---|---|---|
| Ce qui tourne | les vrais GmsCore, GSF et Play Store fournis par l'utilisateur | microG (GmsCore libre, Apache-2.0) à la place de GmsCore/GSF |
| Couverture des API clientes | complète, à terme | partielle (selon microG) |
| Exigences plateforme | maximales (§2, points 1 à 7) | élevées mais moindres (moins de natif et de services privés) |
| Licences | binaires propriétaires : jamais redistribués par le projet | entièrement open source |
| Play Store | oui, si le compte et l'appareil sont acceptés par Google | non (alternatives : F-Droid…) |
| Point sensible | certification de l'appareil (§5) | *signature spoofing* : faire accepter microG comme `com.google.android.gms` — à n'activer que sur décision explicite, limité à microG |

Les deux voies partagent le même chemin critique (runtime, natif, Binder, framework). La voie B
permet de **valider ce socle plus tôt** sans dépendre de binaires propriétaires.

## 5. Ce qui ne change pas (garde-fous)

- **Aucun binaire Google dans le dépôt** ni dans les artefacts publiés. Les APK sont fournis par
  l'utilisateur, qui reste responsable de leur provenance et du respect des conditions de Google.
- **Pas de contournement des contrôles d'intégrité** : AnyAndroid ne falsifiera pas l'empreinte d'un
  appareil certifié et ne contournera ni Play Integrity, ni SafetyNet, ni DroidGuard. Les applications
  qui exigent l'intégrité « appareil » (banques, certains services de streaming) **resteront
  refusées**, ce qui est cohérent avec le §4.2 du cadrage. Pour un appareil non certifié, Google
  propose une procédure officielle d'enregistrement de l'identifiant GSF : cette décision appartient à
  l'utilisateur.
- **Pas de présentation trompeuse** : AnyAndroid n'est ni certifié ni affilié à Google.
- Les principes du cadrage s'appliquent toujours : refus explicite de ce qui n'est pas pris en
  charge, confirmation avant d'exécuter un APK, aucune élévation de privilèges Windows.

## 6. Plan par étapes (avec critères de sortie)

| Étape | Contenu | Critère de sortie |
|---|---|---|
| **G0** — Mesure | `anyandroid gaps` sur les APK réels de GSF, GmsCore (variante x86_64) et Play Store | rapport versionné dans `docs/compatibility/` ; priorités chiffrées |
| **G1** — Runtime fidèle ✅ | threads + moniteurs, ramasse-miettes, réflexion, interface JNI, DEX 040 (`invoke-custom` reporté : d8 le désucre) | **atteint** — voir [ADR-003](ADR-003-runtime-threads-gc-jni.md) |
| **G2** — libcore AOSP ✅ | amorcer `core-oj`/`core-libart` sur la VM, natifs libcore → Windows (fichiers, sockets, ICU, zlib) | « Hello World » Java exécuté *avec la vraie libcore* — **atteint le 2026-10-10** sur la libcore d'Android 17 ; fichiers, réseau et ICU restent à couvrir, voir [ADR-004](ADR-004-libcore-aosp.md) |
| **G3** — Framework AOSP | `framework.jar`, `ActivityThread`, Binder en processus, services système minimaux, ressources (libandroidfw), rendu Canvas/Skia | une application AOSP simple (ex. Calculatrice) affichée et utilisable |
| **G4** — Couche native | chargeur ELF x86_64, couche bionic → Windows, dlopen, TLS, pthreads | APK avec `.so` x86_64 (ex. démo NDK) fonctionnel |
| **G5** — GMS | installation privilégiée, UID partagé, multi-processus (Binder inter-processus), comptes, *checkin* ; voie B puis voie A | GSF + GmsCore démarrent, connexion d'un compte de test, notification FCM reçue |

Le cadrage estimait le portage fidèle d'AOSP « très élevé et incertain » (§15). Ce plan rend chaque
étape vérifiable séparément : G0 à G2 ont de la valeur même si G5 n'est jamais atteinte.

## 7. Décisions

> Réponse du porteur du projet (2026-10-09) : « B puis A, j'accepte le pivot AOSP, lance G1 ». Les garde-fous du §5 s'appliquent sans changement.
>
> Suite (2026-10-09) : « Extrais-la de l'image système AOSP, lance G2 ; pour les APK, va les chercher
> toi-même ». La libcore sera extraite d'une image système officielle (ADR-004). Les APK Google
> propriétaires ne sont pas téléchargés par l'assistant depuis des miroirs tiers (source non fiable,
> redistribution de binaires Google) : pour la voie B, les APK microG officiels (GitHub du projet
> microG) peuvent l'être après confirmation explicite ; pour la voie A, l'utilisateur fournit les APK.

- **D1** — Voie : A (GApps propriétaires), B (microG d'abord) ou B puis A (**recommandé**).
- **D2** — Fourniture des APK pour l'étape G0 : GSF, GmsCore (variante **x86_64**), Play Store,
  idéalement sous forme de bundle `.apks`. Leur provenance relève de l'utilisateur.
- **D3** — Pivot d'architecture du §3 et ses conséquences de licence : `framework.jar` est sous
  Apache-2.0, libcore en partie sous GPLv2 avec *Classpath Exception*. Cela conditionne le choix de
  la licence du projet (Apache-2.0 suggérée).
- **D4** — Validation des garde-fous du §5, en particulier : pas de contournement de Play Integrity
  ni de falsification de certification.
