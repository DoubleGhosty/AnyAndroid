# Analyse d'écart AnyAndroid

> **Contexte (G0).** Rapport produit par `anyandroid gaps` le 10 octobre 2026 sur des APK
> officiels extraits de la GSI GMS x86_64 d'Android 17 (CP3A.260905.010) : Google Play Services, Google Services Framework et Play Store, téléchargés hors du dépôt par `tools/fetch-android-inputs.sh` (aucun binaire n'est
> versionné). « Supportée » se mesure par rapport au **pont** (mode `run`) : avec le vrai
> `framework.jar` d'AOSP (mode `--bootclasspath`), les classes `android.*` viennent du bytecode
> d'AOSP et seules leurs méthodes natives restent à fournir. Ce rapport sert surtout à mesurer les
> **besoins système** (processus, UID partagé, permissions, code natif, Binder) des GApps.

> Analyse statique : aucun code des paquets n'a été exécuté.

## Paquets analysés

| Paquet | Version | Split | Taille | DEX | Méthodes | Bytecode | ABI natives |
|---|---|---|---|---|---|---|---|
| com.google.android.gms | 26.22.36 (260800-937567277) (262236038) | base | 223.4 Mio | 19 | 767 535 | 55.8 Mio | x86, x86_64 |
| com.google.android.gsf | 17 (37) | base | 1.1 Mio | 1 | 2 972 | 121.7 Kio | — |
| com.android.vending | 51.8.09-31 [0] [PR] 927580062 (85180930) | base | 102.4 Mio | 7 | 257 074 | 29.1 Mio | x86_64 |

## Couverture des API de la plateforme

| Statut | API distinctes | Appels dans le code |
|---|---|---|
| Supportée | 970 | 1 139 414 (67.9 %) |
| Partielle | 101 | 18 857 (1.1 %) |
| Stub | 74 | 7 388 (0.4 %) |
| Non supportée | 23 570 | 513 458 (30.6 %) |

  Total : 24 715 API distinctes, 1 679 117 appels.

## Besoins de la plateforme (manifestes)

- com.google.android.gms : minSdk 35, targetSdk 37, com.google.android.gms.version=@0x7f0c005b
- com.google.android.gsf : minSdk 37, targetSdk 37, processus principal com.google.process.gapps, com.google.android.gms.version=@0x7f080000
- com.android.vending : minSdk 31, targetSdk 37, com.google.android.gms.version=@0x7f0c0065
- UID partagé (android:sharedUserId) : com.google.uid.shared (com.google.android.gms)
- UID partagé (android:sharedUserId) : com.google.uid.shared (com.google.android.gsf)
- plusieurs processus — com.google.android.gms : 14 processus (com.google.android.gms, @0x7f1509e2, @0x7f1509a4, com.google.android.gms:identitycredentials, @0x7f1508fa, @0x7f150520, com.google.android.gms.learning, @0x7f1509e6, com.google.android.gms.feedback, com.google.android.gms:car, @0x7f1509ae, @0x7f1509cb, com.google.android.gms.room, com.google.android.gms.remapping1)
- plusieurs processus — com.google.android.gsf : 2 processus (com.google.process.gapps, com.google.process.gservices)
- plusieurs processus — com.android.vending : 6 processus (com.android.vending, com.android.vending:instant_app_installer, com.android.vending:quick_launch, com.android.vending:recovery_mode, com.android.vending:background, com.android.vending:isolated_service)
- permission réservée aux applications système/privilégiées : android.permission.ACCESS_CACHE_FILESYSTEM
- permission réservée aux applications système/privilégiées : android.permission.BACKUP
- permission réservée aux applications système/privilégiées : android.permission.CAPTURE_AUDIO_OUTPUT
- permission réservée aux applications système/privilégiées : android.permission.CHANGE_COMPONENT_ENABLED_STATE
- permission réservée aux applications système/privilégiées : android.permission.DELETE_PACKAGES
- permission réservée aux applications système/privilégiées : android.permission.GET_ACCOUNTS_PRIVILEGED
- permission réservée aux applications système/privilégiées : android.permission.GRANT_RUNTIME_PERMISSIONS
- permission réservée aux applications système/privilégiées : android.permission.INSTALL_PACKAGES
- permission réservée aux applications système/privilégiées : android.permission.INTERACT_ACROSS_USERS
- permission réservée aux applications système/privilégiées : android.permission.LOCAL_MAC_ADDRESS
- permission réservée aux applications système/privilégiées : android.permission.MANAGE_APP_OPS_MODES
- permission réservée aux applications système/privilégiées : android.permission.MANAGE_DEVICE_ADMINS
- permission réservée aux applications système/privilégiées : android.permission.MANAGE_USERS
- permission réservée aux applications système/privilégiées : android.permission.MASTER_CLEAR
- permission réservée aux applications système/privilégiées : android.permission.MODIFY_PHONE_STATE
- permission réservée aux applications système/privilégiées : android.permission.OBSERVE_GRANT_REVOKE_PERMISSIONS
- permission réservée aux applications système/privilégiées : android.permission.PACKAGE_USAGE_STATS
- permission réservée aux applications système/privilégiées : android.permission.READ_LOGS
- permission réservée aux applications système/privilégiées : android.permission.READ_PRIVILEGED_PHONE_STATE
- permission réservée aux applications système/privilégiées : android.permission.REBOOT
- permission réservée aux applications système/privilégiées : android.permission.START_ACTIVITIES_FROM_BACKGROUND
- permission réservée aux applications système/privilégiées : android.permission.SUBSTITUTE_NOTIFICATION_APP_NAME
- permission réservée aux applications système/privilégiées : android.permission.UPDATE_APP_OPS_STATS
- permission réservée aux applications système/privilégiées : android.permission.UPDATE_DEVICE_STATS
- permission réservée aux applications système/privilégiées : android.permission.WRITE_GSERVICES
- permission réservée aux applications système/privilégiées : android.permission.WRITE_SECURE_SETTINGS
- 156 permission(s) de niveau signature déclarée(s) (ex. android.server.checkin.CHECKIN.permission.C2D_MESSAGE [signature])
- 754 composant(s) service/récepteur/fournisseur, dont 60 fournisseur(s) de contenu
- dépendance détectée (chaîne constante) : com.android.vending
- dépendance détectée (chaîne constante) : com.google.android.c2dm.intent.RECEIVE
- dépendance détectée (chaîne constante) : com.google.android.c2dm.permission.SEND
- dépendance détectée (chaîne constante) : com.google.android.gms
- dépendance détectée (chaîne constante) : com.google.android.gms.persistent
- dépendance détectée (chaîne constante) : com.google.android.gsf
- dépendance détectée (chaîne constante) : com.google.android.gsf.login
- dépendance détectée (chaîne constante) : content://com.google.android.gsf.gservices

## Profil des capacités requises

| Capacité | Ce que cela demande | API (manquantes) | Appels | Exemples |
|---|---|---|---|---|
| Code natif (JNI) | chargeur ELF, couche bionic/POSIX, JNI | 2 (2) | 76 | `Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V`<br>`Ljava/lang/System;->load(Ljava/lang/String;)V` |
| Binder / AIDL | IPC entre composants et entre paquets (bindService, Parcel) | 134 (134) | 35 660 | `Landroid/os/Parcel;->readInt()I`<br>`Landroid/os/Parcel;->dataPosition()I`<br>`Landroid/os/IBinder;->queryLocalInterface(Ljava/lang/String;)Landroid/os/IInterface;` |
| Threads et concurrence | threads Java réels, verrous, exécuteurs | 571 (537) | 30 157 | `Ljava/util/concurrent/TimeUnit;->MILLISECONDS:Ljava/util/concurrent/TimeUnit;`<br>`Ljava/util/concurrent/TimeUnit;->SECONDS:Ljava/util/concurrent/TimeUnit;`<br>`Ljava/util/concurrent/atomic/AtomicReference;->get()Ljava/lang/Object;` |
| Réflexion | introspection et appels dynamiques | 109 (48) | 4 235 | `Ljava/lang/Class;->getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;`<br>`Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;`<br>`Ljava/lang/Class;->forName(Ljava/lang/String;)Ljava/lang/Class;` |
| API internes ou cachées | API hors SDK public, sans garantie de stabilité | 71 (71) | 1 123 | `Lsun/misc/Unsafe;->getObject(Ljava/lang/Object;J)Ljava/lang/Object;`<br>`Landroid/os/SystemProperties;->get(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;`<br>`Lsun/misc/Unsafe;->putObject(Ljava/lang/Object;JLjava/lang/Object;)V` |
| Chargement dynamique de code | DexClassLoader, modules téléchargés à l'exécution | 19 (15) | 174 | `Ljava/lang/ClassLoader;->loadClass(Ljava/lang/String;)Ljava/lang/Class;`<br>`Ljava/lang/ClassLoader;->getSystemClassLoader()Ljava/lang/ClassLoader;`<br>`Ljava/lang/ClassLoader;->getResource(Ljava/lang/String;)Ljava/net/URL;` |
| Réseau et TLS | sockets, HTTP, TLS | 1 591 (1 591) | 16 500 | `Landroid/net/Uri;->parse(Ljava/lang/String;)Landroid/net/Uri;`<br>`Landroid/net/Uri$Builder;->appendQueryParameter(Ljava/lang/String;Ljava/lang/String;)Landroid/net/Uri$Builder;`<br>`Landroid/net/Uri;->toString()Ljava/lang/String;` |
| Stockage et bases de données | fichiers privés, SharedPreferences, SQLite | 900 (900) | 41 035 | `Landroid/database/Cursor;->close()V`<br>`Landroid/database/sqlite/SQLiteDatabase;->execSQL(Ljava/lang/String;)V`<br>`Ljava/nio/charset/StandardCharsets;->UTF_8:Ljava/nio/charset/Charset;` |
| Comptes et synchronisation | AccountManager, ContentResolver, fournisseurs de contenu | 144 (144) | 6 202 | `Landroid/accounts/Account;->name:Ljava/lang/String;`<br>`Landroid/accounts/Account;-><init>(Ljava/lang/String;Ljava/lang/String;)V`<br>`Landroid/accounts/Account;->CREATOR:Landroid/os/Parcelable$Creator;` |
| Cryptographie et Keystore | JCA, Android Keystore | 898 (898) | 13 557 | `Ljava/security/GeneralSecurityException;-><init>(Ljava/lang/String;)V`<br>`Ljava/security/InvalidKeyException;-><init>(Ljava/lang/String;)V`<br>`Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;` |
| Graphismes avancés et WebView | Canvas/Bitmap, OpenGL ES, WebView | 1 518 (1 518) | 27 884 | `Landroid/graphics/Rect;->left:I`<br>`Landroid/graphics/Rect;->top:I`<br>`Landroid/graphics/Rect;-><init>()V` |
| Services système | getSystemService, JobScheduler, alarmes, notifications, PackageManager | 604 (603) | 10 362 | `Landroid/content/Context;->getSystemService(Ljava/lang/String;)Ljava/lang/Object;`<br>`Landroid/content/pm/PackageManager;->getPackageInfo(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;`<br>`Landroid/content/Context;->getSystemService(Ljava/lang/Class;)Ljava/lang/Object;` |
| Ressources compilées | resources.arsc, layouts XML, chaînes localisées | 168 (140) | 28 811 | `Landroid/content/Context;->getString(I)Ljava/lang/String;`<br>`Landroid/content/Context;->getResources()Landroid/content/res/Resources;`<br>`Landroid/content/res/Resources;->getString(I)Ljava/lang/String;` |
| Méthodes JNI déclarées | méthodes Java implémentées en code natif | 1 709 (1 709) | 1 709 | `Landroidx/datastore/core/NativeSharedCounter;->nativeCreateSharedCounter(I)J`<br>`Landroidx/datastore/core/NativeSharedCounter;->nativeGetCounterValue(J)I`<br>`Landroidx/datastore/core/NativeSharedCounter;->nativeIncrementAndGetCounterValue(J)I`<br>`Landroidx/datastore/core/NativeSharedCounter;->nativeTruncateFile(I)I`<br>`Lcom/google/android/chre/ap/ContextHubAPNative;->destroy()V`<br>`Lcom/google/android/chre/ap/ContextHubAPNative;->init()I`<br>`Lcom/google/android/chre/ap/ContextHubAPNative;->isInitialized()Z`<br>`Lcom/google/android/chre/ap/ContextHubAPNative;->listNanoapps()[Lcom/google/android/chre/ap/NanoAppState;` |

## Paquets Java manquants les plus utilisés

| Nom | API manquantes | Appels |
|---|---|---|
| `java.util` | 1 161 | 83 561 |
| `android.os` | 806 | 60 894 |
| `java.lang` | 1 898 | 37 200 |
| `android.view` | 1 397 | 35 478 |
| `android.content` | 626 | 30 039 |
| `android.widget` | 2 709 | 29 936 |
| `java.io` | 483 | 25 103 |
| `android.graphics` | 735 | 20 327 |
| `java.util.concurrent` | 397 | 14 451 |
| `android.util` | 285 | 13 876 |
| `android.text` | 316 | 12 308 |
| `java.nio` | 151 | 11 806 |
| `android.content.res` | 133 | 10 432 |
| `android.net` | 245 | 10 013 |
| `java.util.concurrent.atomic` | 84 | 9 467 |
| `org.json` | 90 | 8 161 |
| `android.app` | 1 205 | 7 954 |
| `android.content.pm` | 472 | 7 653 |
| `android.database` | 126 | 7 027 |
| `java.security` | 263 | 6 925 |
| `java.math` | 109 | 5 633 |
| `android.database.sqlite` | 148 | 5 354 |
| `android.accounts` | 68 | 5 343 |
| `android.graphics.drawable` | 310 | 4 753 |
| `android.animation` | 165 | 3 671 |
  … et 235 autres

## Classes manquantes les plus utilisées

| Nom | API manquantes | Appels |
|---|---|---|
| `Landroid/os/Parcel;` | 94 | 30 128 |
| `Landroid/view/View;` | 337 | 17 102 |
| `Ljava/util/Set;` | 16 | 16 389 |
| `Ljava/lang/Object;` | 1 470 | 14 821 |
| `Ljava/util/Arrays;` | 89 | 11 376 |
| `Ljava/nio/ByteBuffer;` | 66 | 10 385 |
| `Landroid/text/TextUtils;` | 25 | 8 545 |
| `Landroid/content/Context;` | 105 | 8 230 |
| `Landroid/os/Bundle;` | 76 | 7 904 |
| `Ljava/util/Map$Entry;` | 5 | 7 785 |
| `Ljava/lang/Float;` | 8 | 7 148 |
| `Landroid/content/Intent;` | 87 | 7 145 |
| `Ljava/io/File;` | 55 | 6 934 |
| `Lorg/json/JSONObject;` | 42 | 6 661 |
| `Landroid/database/Cursor;` | 42 | 6 603 |
| `Ljava/util/concurrent/TimeUnit;` | 25 | 6 270 |
| `Landroid/content/res/TypedArray;` | 27 | 5 942 |
| `Landroid/view/ViewGroup;` | 304 | 5 616 |
| `Ljava/util/Map;` | 5 | 5 478 |
| `Ljava/lang/String;` | 32 | 5 446 |
| `Ljava/util/AbstractCollection;` | 12 | 5 378 |
| `Landroid/net/Uri;` | 40 | 5 291 |
| `Landroid/accounts/Account;` | 8 | 5 047 |
| `Landroid/graphics/Rect;` | 33 | 4 947 |
| `Ljava/io/PrintWriter;` | 33 | 4 905 |
  … et 3 324 autres

## API manquantes les plus appelées

| API | Statut | Appels | Note |
|---|---|---|---|
| `Landroid/text/TextUtils;->isEmpty(Ljava/lang/CharSequence;)Z` | Non supportée | 7 199 | classe non implémentée |
| `Landroid/view/View;->findViewById(I)Landroid/view/View;` | Non supportée | 6 739 | méthode non implémentée |
| `Landroid/os/Parcel;->readInt()I` | Non supportée | 6 136 | classe non implémentée |
| `Ljava/util/Set;->iterator()Ljava/util/Iterator;` | Non supportée | 5 361 | classe non implémentée |
| `Ljava/util/Set;->add(Ljava/lang/Object;)Z` | Non supportée | 4 185 | classe non implémentée |
| `Landroid/os/Parcel;->dataPosition()I` | Non supportée | 4 045 | classe non implémentée |
| `Ljava/util/Map$Entry;->getValue()Ljava/lang/Object;` | Non supportée | 4 005 | classe non implémentée |
| `Landroid/os/IBinder;->queryLocalInterface(Ljava/lang/String;)Landroid/os/IInterface;` | Non supportée | 3 783 | classe non implémentée |
| `Ljava/util/Map$Entry;->getKey()Ljava/lang/Object;` | Non supportée | 3 738 | classe non implémentée |
| `Ljava/lang/Object;->size()I` | Non supportée | 3 637 | méthode non implémentée |
| `Landroid/accounts/Account;->name:Ljava/lang/String;` | Non supportée | 3 565 | classe non implémentée |
| `Ljava/lang/Throwable;->addSuppressed(Ljava/lang/Throwable;)V` | Stub | 3 432 | exceptions supprimées ignorées |
| `Landroid/os/SystemClock;->elapsedRealtime()J` | Non supportée | 3 132 | classe non implémentée |
| `Landroid/os/Parcel;->writeInt(I)V` | Non supportée | 2 992 | classe non implémentée |
| `Landroid/os/Parcel;->readStrongBinder()Landroid/os/IBinder;` | Non supportée | 2 847 | classe non implémentée |
| `Ljava/util/Set;->contains(Ljava/lang/Object;)Z` | Non supportée | 2 843 | classe non implémentée |
| `Ljava/util/AbstractCollection;->isEmpty()Z` | Non supportée | 2 762 | classe non implémentée |
| `Ljava/lang/Float;->intBitsToFloat(I)F` | Non supportée | 2 721 | méthode non implémentée |
| `Landroid/os/Parcel;->readString()Ljava/lang/String;` | Non supportée | 2 712 | classe non implémentée |
| `Landroid/os/Parcel;->writeNoException()V` | Non supportée | 2 453 | classe non implémentée |
| `Ljava/util/Map;->entrySet()Ljava/util/Set;` | Non supportée | 2 440 | méthode non implémentée |
| `Ljava/util/concurrent/TimeUnit;->MILLISECONDS:Ljava/util/concurrent/TimeUnit;` | Non supportée | 2 380 | classe non implémentée |
| `Ljava/lang/String;->format(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;` | Non supportée | 2 293 | méthode non implémentée |
| `Ljava/lang/Float;->floatToRawIntBits(F)I` | Non supportée | 2 274 | méthode non implémentée |
| `Ljava/util/Arrays;->hashCode([Ljava/lang/Object;)I` | Non supportée | 2 137 | classe non implémentée |
  … et 23 619 autres

## Prérequis à construire, dans l'ordre suggéré

  1. Couche native : chargeur ELF, traduction bionic → Windows (libc, pthread, mmap, dlopen) et JNI (bibliothèques x86_64 présentes : exécution native possible sans traduction)
  2. Concurrence : java.util.concurrent (exécuteurs, verrous, atomiques) au-dessus des threads du runtime
  3. IPC : Binder/AIDL en processus (Parcel, IBinder, bindService) et gestionnaire de services
  4. Runtime : réflexion java.lang.reflect complète
  5. Ressources : resources.arsc, layouts et chaînes compilés
  6. Plateforme : installation « système privilégié », UID partagé et modèle multi-processus
  7. Services : AccountManager, ContentResolver/ContentProvider, synchronisation
  8. Framework : 23 570 API référencées non implémentées (voir le classement) — plaide pour l'exécution du framework AOSP lui-même plutôt qu'une réécriture API par API
