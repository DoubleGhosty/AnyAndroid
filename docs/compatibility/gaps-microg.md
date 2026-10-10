# Analyse d'écart AnyAndroid

> **Contexte (G0).** Rapport produit par `anyandroid gaps` le 10 octobre 2026 sur des APK
> officiels de microG 0.3.17 (dépôt F-Droid microg.org) : GmsCore, GsfProxy et le compagnon Play Store, téléchargés hors du dépôt par `tools/fetch-android-inputs.sh` (aucun binaire n'est
> versionné). « Supportée » se mesure par rapport au **pont** (mode `run`) : avec le vrai
> `framework.jar` d'AOSP (mode `--bootclasspath`), les classes `android.*` viennent du bytecode
> d'AOSP et seules leurs méthodes natives restent à fournir. Ce rapport sert surtout à mesurer les
> **besoins système** (processus, UID partagé, permissions, code natif, Binder) des GApps.

> Analyse statique : aucun code des paquets n'a été exécuté.

## Paquets analysés

| Paquet | Version | Split | Taille | DEX | Méthodes | Bytecode | ABI natives |
|---|---|---|---|---|---|---|---|
| com.google.android.gms | 0.3.17.252432 (252432034) | base | 103.1 Mio | 5 | 254 210 | 12.3 Mio | arm64-v8a, armeabi-v7a, x86, x86_64 |
| com.google.android.gsf | v0.1.0 (8) | base | 21.4 Kio | 1 | 7 | 0.1 Kio | — |
| com.android.vending | 0.3.17.40226 (84022634) | base | 4.0 Mio | 1 | 29 178 | 2.6 Mio | — |

## Couverture des API de la plateforme

| Statut | API distinctes | Appels dans le code |
|---|---|---|
| Supportée | 822 | 243 729 (61.3 %) |
| Partielle | 85 | 5 258 (1.3 %) |
| Stub | 59 | 1 550 (0.4 %) |
| Non supportée | 13 296 | 146 797 (36.9 %) |

  Total : 14 262 API distinctes, 397 334 appels.

## Besoins de la plateforme (manifestes)

- com.google.android.gms : minSdk 19, targetSdk 29, com.google.android.gms.version=@0x7f0a0009
- com.google.android.gms requiert la bibliothèque partagée com.android.location.provider
- com.google.android.gsf : minSdk 10, targetSdk 23
- com.android.vending : minSdk 19, targetSdk 29, com.google.android.gms.version=@0x7f0a0009
- plusieurs processus — com.google.android.gms : 4 processus (com.google.android.gms, com.google.android.gms:ui, com.google.android.gms:persistent, com.google.android.gms.unstable)
- plusieurs processus — com.android.vending : 3 processus (com.android.vending, com.android.vending:ui, com.android.vending:background)
- permission réservée aux applications système/privilégiées : android.permission.DELETE_PACKAGES
- permission réservée aux applications système/privilégiées : android.permission.INSTALL_LOCATION_PROVIDER
- permission réservée aux applications système/privilégiées : android.permission.INSTALL_PACKAGES
- permission réservée aux applications système/privilégiées : android.permission.INTERACT_ACROSS_USERS
- permission réservée aux applications système/privilégiées : android.permission.MODIFY_PHONE_STATE
- permission réservée aux applications système/privilégiées : android.permission.START_ACTIVITIES_FROM_BACKGROUND
- permission réservée aux applications système/privilégiées : android.permission.UPDATE_APP_OPS_STATS
- permission réservée aux applications système/privilégiées : android.permission.UPDATE_DEVICE_STATS
- 12 permission(s) de niveau signature déclarée(s) (ex. com.android.vending.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION [signature])
- 179 composant(s) service/récepteur/fournisseur, dont 15 fournisseur(s) de contenu
- dépendance détectée (chaîne constante) : com.android.vending
- dépendance détectée (chaîne constante) : com.google.android.c2dm.intent.RECEIVE
- dépendance détectée (chaîne constante) : com.google.android.c2dm.permission.SEND
- dépendance détectée (chaîne constante) : com.google.android.gms
- dépendance détectée (chaîne constante) : com.google.android.gsf
- dépendance détectée (chaîne constante) : content://com.google.android.gsf.gservices

## Profil des capacités requises

| Capacité | Ce que cela demande | API (manquantes) | Appels | Exemples |
|---|---|---|---|---|
| Code natif (JNI) | chargeur ELF, couche bionic/POSIX, JNI | 2 (2) | 10 | `Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V`<br>`Ljava/lang/System;->load(Ljava/lang/String;)V` |
| Binder / AIDL | IPC entre composants et entre paquets (bindService, Parcel) | 108 (108) | 33 758 | `Landroid/os/Parcel;->recycle()V`<br>`Landroid/os/Parcel;->obtain()Landroid/os/Parcel;`<br>`Landroid/os/Parcel;->writeNoException()V` |
| Threads et concurrence | threads Java réels, verrous, exécuteurs | 461 (431) | 6 802 | `Ljava/util/concurrent/atomic/AtomicReferenceFieldUpdater;->get(Ljava/lang/Object;)Ljava/lang/Object;`<br>`Ljava/util/concurrent/Executor;->execute(Ljava/lang/Runnable;)V`<br>`Ljava/lang/Thread;->currentThread()Ljava/lang/Thread;` |
| Réflexion | introspection et appels dynamiques | 137 (64) | 3 346 | `Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;`<br>`Ljava/lang/Class;->getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;`<br>`Ljava/lang/reflect/Field;->get(Ljava/lang/Object;)Ljava/lang/Object;` |
| API internes ou cachées | API hors SDK public, sans garantie de stabilité | 31 (31) | 432 | `Lsun/misc/Unsafe;->getObject(Ljava/lang/Object;J)Ljava/lang/Object;`<br>`Lsun/misc/Unsafe;->putObject(Ljava/lang/Object;JLjava/lang/Object;)V`<br>`Lsun/misc/Unsafe;->putInt(Ljava/lang/Object;JI)V` |
| Chargement dynamique de code | DexClassLoader, modules téléchargés à l'exécution | 16 (12) | 68 | `Ljava/lang/ClassLoader;->loadClass(Ljava/lang/String;)Ljava/lang/Class;`<br>`Ljava/lang/ClassLoader;->getResources(Ljava/lang/String;)Ljava/util/Enumeration;`<br>`Ljava/lang/ClassLoader;->getResource(Ljava/lang/String;)Ljava/net/URL;` |
| Réseau et TLS | sockets, HTTP, TLS | 568 (568) | 2 197 | `Ljava/net/ProtocolException;-><init>(Ljava/lang/String;)V`<br>`Landroid/net/Uri;->parse(Ljava/lang/String;)Landroid/net/Uri;`<br>`Landroid/net/Uri;->toString()Ljava/lang/String;` |
| Stockage et bases de données | fichiers privés, SharedPreferences, SQLite | 590 (590) | 7 187 | `Landroid/database/Cursor;->close()V`<br>`Ljava/nio/ByteBuffer;->remaining()I`<br>`Landroid/database/Cursor;->getString(I)Ljava/lang/String;` |
| Comptes et synchronisation | AccountManager, ContentResolver, fournisseurs de contenu | 84 (84) | 733 | `Landroid/accounts/Account;->name:Ljava/lang/String;`<br>`Landroid/accounts/AccountManager;->get(Landroid/content/Context;)Landroid/accounts/AccountManager;`<br>`Landroid/accounts/AccountManager;->getAccountsByType(Ljava/lang/String;)[Landroid/accounts/Account;` |
| Cryptographie et Keystore | JCA, Android Keystore | 419 (419) | 4 206 | `Ljava/security/GeneralSecurityException;-><init>(Ljava/lang/String;)V`<br>`Ljava/security/Provider;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;`<br>`Ljava/security/InvalidAlgorithmParameterException;-><init>(Ljava/lang/String;)V` |
| Graphismes avancés et WebView | Canvas/Bitmap, OpenGL ES, WebView | 1 246 (1 246) | 13 143 | `Landroid/graphics/Rect;->left:I`<br>`Landroid/graphics/Rect;->top:I`<br>`Landroid/graphics/Rect;->right:I` |
| Services système | getSystemService, JobScheduler, alarmes, notifications, PackageManager | 372 (371) | 1 386 | `Landroid/content/Context;->getSystemService(Ljava/lang/String;)Ljava/lang/Object;`<br>`Landroid/content/pm/PackageManager;->getPackageInfo(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;`<br>`Landroid/content/pm/ResolveInfo;->activityInfo:Landroid/content/pm/ActivityInfo;` |
| Ressources compilées | resources.arsc, layouts XML, chaînes localisées | 170 (143) | 6 829 | `Landroid/content/Context;->getResources()Landroid/content/res/Resources;`<br>`Landroid/content/res/TypedArray;->recycle()V`<br>`Landroid/content/res/TypedArray;->getInt(II)I` |
| Méthodes JNI déclarées | méthodes Java implémentées en code natif | 3 818 (3 818) | 3 818 | `LJ/N;->M$oMD214(Ljava/lang/String;JIIII)J`<br>`LJ/N;->M0s8NeYn(JLjava/lang/Object;ILjava/lang/String;)V`<br>`LJ/N;->M135Cu0D(J)J`<br>`LJ/N;->M1cMYXGO(Ljava/lang/String;)V`<br>`LJ/N;->M1gJHszj(Ljava/lang/String;IJ)I`<br>`LJ/N;->M4mrObfZ(Ljava/lang/String;J)I`<br>`LJ/N;->M4znfYdB(JLjava/lang/Object;Z)V`<br>`LJ/N;->M51RPBJe(JLjava/lang/Object;Ljava/lang/String;)Z` |

## Paquets Java manquants les plus utilisés

| Nom | API manquantes | Appels |
|---|---|---|
| `android.os` | 432 | 41 246 |
| `java.util` | 827 | 16 875 |
| `android.view` | 1 374 | 13 511 |
| `android.graphics` | 768 | 9 922 |
| `android.widget` | 1 678 | 6 362 |
| `java.lang` | 406 | 5 358 |
| `android.content.res` | 135 | 4 806 |
| `java.io` | 318 | 4 498 |
| `android.content` | 301 | 3 685 |
| `android.util` | 192 | 3 277 |
| `java.security` | 164 | 2 683 |
| `java.nio` | 111 | 2 640 |
| `java.util.concurrent.atomic` | 91 | 2 470 |
| `android.graphics.drawable` | 189 | 2 451 |
| `java.util.concurrent` | 267 | 2 402 |
| `android.app` | 681 | 2 110 |
| `android.text` | 255 | 1 818 |
| `android.database` | 78 | 1 573 |
| `java.math` | 91 | 1 539 |
| `android.animation` | 149 | 1 317 |
| `org.json` | 53 | 1 172 |
| `android.content.pm` | 295 | 1 029 |
| `java.util.concurrent.locks` | 57 | 946 |
| `java.net` | 207 | 864 |
| `android.view.accessibility` | 338 | 852 |
  … et 146 autres

## Classes manquantes les plus utilisées

| Nom | API manquantes | Appels |
|---|---|---|
| `Landroid/os/Parcel;` | 87 | 29 522 |
| `Landroid/view/View;` | 387 | 6 068 |
| `Landroid/content/res/TypedArray;` | 30 | 3 200 |
| `Landroid/os/Build$VERSION;` | 6 | 2 950 |
| `Landroid/os/IBinder;` | 7 | 2 768 |
| `Landroid/graphics/Rect;` | 31 | 2 657 |
| `Ljava/util/Set;` | 15 | 2 332 |
| `Ljava/util/Arrays;` | 106 | 2 311 |
| `Ljava/nio/ByteBuffer;` | 60 | 2 296 |
| `Landroid/view/ViewGroup;` | 225 | 1 889 |
| `Landroid/graphics/drawable/Drawable;` | 66 | 1 867 |
| `Landroid/os/Bundle;` | 59 | 1 714 |
| `Ljava/util/Map$Entry;` | 5 | 1 645 |
| `Ljava/security/GeneralSecurityException;` | 6 | 1 505 |
| `Landroid/os/Binder;` | 10 | 1 433 |
| `Landroid/database/Cursor;` | 27 | 1 407 |
| `Landroid/graphics/RectF;` | 30 | 1 395 |
| `Ljava/util/Collections;` | 36 | 1 325 |
| `Landroid/content/Context;` | 69 | 1 290 |
| `Ljava/math/BigInteger;` | 44 | 1 267 |
| `Ljava/lang/Float;` | 6 | 1 135 |
| `Landroid/graphics/Canvas;` | 111 | 1 102 |
| `Landroid/view/MotionEvent;` | 48 | 1 102 |
| `Lorg/json/JSONObject;` | 35 | 1 027 |
| `Ljava/util/LinkedHashMap;` | 18 | 956 |
  … et 1 965 autres

## API manquantes les plus appelées

| API | Statut | Appels | Note |
|---|---|---|---|
| `Landroid/os/Parcel;->recycle()V` | Non supportée | 8 508 | classe non implémentée |
| `Landroid/os/Parcel;->obtain()Landroid/os/Parcel;` | Non supportée | 4 250 | classe non implémentée |
| `Landroid/os/Build$VERSION;->SDK_INT:I` | Non supportée | 2 929 | classe non implémentée |
| `Landroid/os/Parcel;->writeNoException()V` | Non supportée | 2 234 | classe non implémentée |
| `Landroid/os/IBinder;->transact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z` | Non supportée | 2 229 | classe non implémentée |
| `Landroid/os/Parcel;->writeInterfaceToken(Ljava/lang/String;)V` | Non supportée | 2 229 | classe non implémentée |
| `Landroid/os/Parcel;->readException()V` | Non supportée | 2 000 | classe non implémentée |
| `Landroid/os/Parcel;->writeInt(I)V` | Non supportée | 1 581 | classe non implémentée |
| `Landroid/os/Parcel;->readInt()I` | Non supportée | 1 453 | classe non implémentée |
| `Ljava/security/GeneralSecurityException;-><init>(Ljava/lang/String;)V` | Non supportée | 1 412 | classe non implémentée |
| `Landroid/os/Parcel;->readStrongBinder()Landroid/os/IBinder;` | Non supportée | 1 196 | classe non implémentée |
| `Landroid/os/Parcel;->writeString(Ljava/lang/String;)V` | Non supportée | 1 170 | classe non implémentée |
| `Landroid/os/Parcel;->dataPosition()I` | Non supportée | 1 093 | classe non implémentée |
| `Landroid/os/Parcel;->writeStrongInterface(Landroid/os/IInterface;)V` | Non supportée | 1 015 | classe non implémentée |
| `Ljava/util/Set;->iterator()Ljava/util/Iterator;` | Non supportée | 972 | classe non implémentée |
| `Ljava/util/Map$Entry;->getValue()Ljava/lang/Object;` | Non supportée | 847 | classe non implémentée |
| `Landroid/os/Parcel;->readString()Ljava/lang/String;` | Non supportée | 823 | classe non implémentée |
| `Ljava/util/Map$Entry;->getKey()Ljava/lang/Object;` | Non supportée | 773 | classe non implémentée |
| `Landroid/os/Parcel;->enforceInterface(Ljava/lang/String;)V` | Non supportée | 546 | classe non implémentée |
| `Ljava/util/Arrays;->copyOf([Ljava/lang/Object;I)[Ljava/lang/Object;` | Non supportée | 536 | classe non implémentée |
| `Landroid/os/Binder;->onTransact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z` | Non supportée | 508 | classe non implémentée |
| `Landroid/os/IBinder;->queryLocalInterface(Ljava/lang/String;)Landroid/os/IInterface;` | Non supportée | 508 | classe non implémentée |
| `Landroid/util/SparseIntArray;->append(II)V` | Non supportée | 497 | classe non implémentée |
| `Landroid/os/Parcelable$Creator;->createFromParcel(Landroid/os/Parcel;)Ljava/lang/Object;` | Non supportée | 496 | classe non implémentée |
| `Ljava/lang/Float;->floatToIntBits(F)I` | Non supportée | 478 | méthode non implémentée |
  … et 13 330 autres

## Prérequis à construire, dans l'ordre suggéré

  1. Couche native : chargeur ELF, traduction bionic → Windows (libc, pthread, mmap, dlopen) et JNI (bibliothèques x86_64 présentes : exécution native possible sans traduction)
  2. Concurrence : java.util.concurrent (exécuteurs, verrous, atomiques) au-dessus des threads du runtime
  3. IPC : Binder/AIDL en processus (Parcel, IBinder, bindService) et gestionnaire de services
  4. Runtime : réflexion java.lang.reflect complète
  5. Ressources : resources.arsc, layouts et chaînes compilés
  6. Plateforme : installation « système privilégié », UID partagé et modèle multi-processus
  7. Services : AccountManager, ContentResolver/ContentProvider, synchronisation
  8. Framework : 13 296 API référencées non implémentées (voir le classement) — plaide pour l'exécution du framework AOSP lui-même plutôt qu'une réécriture API par API
