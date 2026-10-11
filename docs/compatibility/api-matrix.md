# Matrice de compatibilité des API

> Fichier généré par `anyandroid compat --markdown` à partir du registre du pont : ne pas modifier à la main.
> Une API absente de ce tableau n'est **pas** implémentée : tout appel échoue avec une erreur explicite.

| Statut | Signification | Membres |
|---|---|---|
| Supportée | comportement testé et documenté pour le périmètre annoncé | 794 |
| Partielle | chemin courant pris en charge, limites connues | 63 |
| Stub | API reconnue mais sans effet réel | 24 |
| Non supportée | reconnue mais refusée avec une erreur explicite | 26 |

## `android.app.Activity`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `onCreate(Landroid/os/Bundle;)V` | Supportée |  |
| `onPostCreate(Landroid/os/Bundle;)V` | Supportée |  |
| `onStart()V` | Supportée |  |
| `onRestart()V` | Supportée |  |
| `onResume()V` | Supportée |  |
| `onPostResume()V` | Supportée |  |
| `onPause()V` | Supportée |  |
| `onStop()V` | Supportée |  |
| `onDestroy()V` | Supportée |  |
| `onSaveInstanceState(Landroid/os/Bundle;)V` | Partielle | état non restauré (pas de recréation d'activité) |
| `onBackPressed()V` | Supportée |  |
| `setContentView(Landroid/view/View;)V` | Supportée |  |
| `findViewById(I)Landroid/view/View;` | Supportée |  |
| `setTitle(Ljava/lang/CharSequence;)V` | Supportée |  |
| `getTitle()Ljava/lang/CharSequence;` | Supportée |  |
| `finish()V` | Supportée |  |
| `isFinishing()Z` | Supportée |  |
| `getIntent()Landroid/content/Intent;` | Supportée |  |
| `setIntent(Landroid/content/Intent;)V` | Supportée |  |
| `runOnUiThread(Ljava/lang/Runnable;)V` | Supportée |  |
| `getApplication()Landroid/app/Application;` | Supportée |  |
| `startActivityForResult(Landroid/content/Intent;I)V` | Partielle | le résultat n'est jamais renvoyé (onActivityResult non appelé) |
| `setResult(I)V` | Stub | résultats d'activité non pris en charge |
| `setResult(ILandroid/content/Intent;)V` | Stub | résultats d'activité non pris en charge |
| `overridePendingTransition(II)V` | Stub | pas d'animations |
| `getWindow()Landroid/view/Window;` | Non supportée | API Window non implémentée |
| `getActionBar()Landroid/app/ActionBar;` | Non supportée | barre d'action non implémentée |
| `getSharedPreferences(Ljava/lang/String;I)Landroid/content/SharedPreferences;` | Non supportée | stockage persistant prévu en phase 5 |
| `setContentView(I)V` | Supportée |  |
| `getLayoutInflater()Landroid/view/LayoutInflater;` | Supportée |  |
| `setTitle(I)V` | Supportée |  |

## `android.app.Application`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `onCreate()V` | Supportée |  |
| `onTerminate()V` | Stub | jamais appelé |

## `android.content.ActivityNotFoundException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.content.Context`

| Membre | Statut | Note |
|---|---|---|
| `getPackageName()Ljava/lang/String;` | Supportée |  |
| `getApplicationContext()Landroid/content/Context;` | Supportée |  |
| `startActivity(Landroid/content/Intent;)V` | Supportée |  |
| `getMainLooper()Landroid/os/Looper;` | Supportée |  |
| `getSharedPreferences(Ljava/lang/String;I)Landroid/content/SharedPreferences;` | Non supportée | stockage persistant prévu en phase 5 |
| `getFilesDir()Ljava/io/File;` | Non supportée | accès fichiers prévu en phase 5 |
| `getCacheDir()Ljava/io/File;` | Non supportée | accès fichiers prévu en phase 5 |
| `getDataDir()Ljava/io/File;` | Non supportée | accès fichiers prévu en phase 5 |
| `checkSelfPermission(Ljava/lang/String;)I` | Non supportée | modèle de permissions non implémenté |
| `getResources()Landroid/content/res/Resources;` | Supportée |  |
| `getString(I)Ljava/lang/String;` | Supportée |  |
| `getString(I[Ljava/lang/Object;)Ljava/lang/String;` | Supportée |  |
| `getText(I)Ljava/lang/CharSequence;` | Partielle | texte sans styles |
| `getColor(I)I` | Supportée |  |
| `getSystemService(Ljava/lang/String;)Ljava/lang/Object;` | Partielle | seul layout_inflater est disponible en mode pont |

## `android.content.ContextWrapper`

| Membre | Statut | Note |
|---|---|---|
| `getBaseContext()Landroid/content/Context;` | Partielle | renvoie le contexte lui-même |

## `android.content.Intent`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Landroid/content/Context;Ljava/lang/Class;)V` | Supportée |  |
| `setClass(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;` | Supportée |  |
| `setClassName(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;` | Supportée |  |
| `setClassName(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;` | Supportée |  |
| `setAction(Ljava/lang/String;)Landroid/content/Intent;` | Supportée |  |
| `getAction()Ljava/lang/String;` | Supportée |  |
| `putExtra(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;` | Supportée |  |
| `putExtra(Ljava/lang/String;Ljava/lang/CharSequence;)Landroid/content/Intent;` | Supportée |  |
| `putExtra(Ljava/lang/String;I)Landroid/content/Intent;` | Supportée |  |
| `putExtra(Ljava/lang/String;Z)Landroid/content/Intent;` | Supportée |  |
| `putExtra(Ljava/lang/String;J)Landroid/content/Intent;` | Supportée |  |
| `putExtra(Ljava/lang/String;F)Landroid/content/Intent;` | Supportée |  |
| `putExtra(Ljava/lang/String;D)Landroid/content/Intent;` | Supportée |  |
| `getStringExtra(Ljava/lang/String;)Ljava/lang/String;` | Supportée |  |
| `getIntExtra(Ljava/lang/String;I)I` | Supportée |  |
| `getBooleanExtra(Ljava/lang/String;Z)Z` | Supportée |  |
| `getLongExtra(Ljava/lang/String;J)J` | Supportée |  |
| `hasExtra(Ljava/lang/String;)Z` | Supportée |  |
| `getExtras()Landroid/os/Bundle;` | Supportée |  |
| `putExtras(Landroid/os/Bundle;)Landroid/content/Intent;` | Supportée |  |
| `addFlags(I)Landroid/content/Intent;` | Partielle | drapeaux mémorisés mais sans effet |
| `setFlags(I)Landroid/content/Intent;` | Partielle | drapeaux mémorisés mais sans effet |
| `getFlags()I` | Supportée |  |

## `android.content.res.Resources$NotFoundException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.content.res.Resources`

| Membre | Statut | Note |
|---|---|---|
| `getText(I)Ljava/lang/CharSequence;` | Partielle | texte sans styles (balises <b>, <i>... ignorées) |
| `getText(ILjava/lang/CharSequence;)Ljava/lang/CharSequence;` | Supportée |  |
| `getString(I)Ljava/lang/String;` | Supportée |  |
| `getString(I[Ljava/lang/Object;)Ljava/lang/String;` | Supportée |  |
| `getColor(I)I` | Supportée |  |
| `getColor(ILandroid/content/res/Resources$Theme;)I` | Partielle | thème ignoré |
| `getInteger(I)I` | Supportée |  |
| `getBoolean(I)Z` | Supportée |  |
| `getDimension(I)F` | Supportée |  |
| `getDimensionPixelOffset(I)I` | Supportée |  |
| `getDimensionPixelSize(I)I` | Supportée |  |
| `getIdentifier(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I` | Supportée |  |
| `getResourceName(I)Ljava/lang/String;` | Supportée |  |
| `getResourcePackageName(I)Ljava/lang/String;` | Supportée |  |
| `getResourceTypeName(I)Ljava/lang/String;` | Supportée |  |
| `getResourceEntryName(I)Ljava/lang/String;` | Supportée |  |
| `getLayout(I)Landroid/content/res/XmlResourceParser;` | Non supportée | XmlPullParser non disponible en mode pont |
| `getDrawable(I)Landroid/graphics/drawable/Drawable;` | Non supportée | images (drawables) non prises en charge en mode pont |
| `getStringArray(I)[Ljava/lang/String;` | Non supportée | tableaux de ressources non pris en charge en mode pont |

## `android.os.BaseBundle`

| Membre | Statut | Note |
|---|---|---|
| `putString(Ljava/lang/String;Ljava/lang/String;)V` | Supportée |  |
| `putInt(Ljava/lang/String;I)V` | Supportée |  |
| `putBoolean(Ljava/lang/String;Z)V` | Supportée |  |
| `putLong(Ljava/lang/String;J)V` | Supportée |  |
| `putFloat(Ljava/lang/String;F)V` | Supportée |  |
| `putDouble(Ljava/lang/String;D)V` | Supportée |  |
| `getString(Ljava/lang/String;)Ljava/lang/String;` | Supportée |  |
| `getString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;` | Supportée |  |
| `getInt(Ljava/lang/String;)I` | Supportée |  |
| `getInt(Ljava/lang/String;I)I` | Supportée |  |
| `getBoolean(Ljava/lang/String;)Z` | Supportée |  |
| `getBoolean(Ljava/lang/String;Z)Z` | Supportée |  |
| `getLong(Ljava/lang/String;)J` | Supportée |  |
| `containsKey(Ljava/lang/String;)Z` | Supportée |  |
| `remove(Ljava/lang/String;)V` | Supportée |  |
| `size()I` | Supportée |  |
| `isEmpty()Z` | Supportée |  |
| `keySet()Ljava/util/Set;` | Non supportée | java.util.Set non implémenté |

## `android.os.Bundle`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |

## `android.os.Handler`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Landroid/os/Looper;)V` | Partielle | seul le Looper principal est pris en charge |
| `post(Ljava/lang/Runnable;)Z` | Supportée |  |
| `postDelayed(Ljava/lang/Runnable;J)Z` | Partielle | délai ignoré : exécuté au prochain tour de la boucle d'événements |
| `removeCallbacks(Ljava/lang/Runnable;)V` | Supportée |  |

## `android.os.Looper`

| Membre | Statut | Note |
|---|---|---|
| `getMainLooper()Landroid/os/Looper;` (statique) | Supportée |  |
| `myLooper()Landroid/os/Looper;` (statique) | Supportée |  |
| `prepare()V` (statique) | Stub | un seul Looper (thread UI) |
| `loop()V` (statique) | Non supportée | boucle gérée par l'hôte AnyAndroid |

## `android.os.Parcelable` (interface)

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.text.Editable` (interface)

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.text.SpannableStringBuilder`

Classe : **Partielle** — texte brut, sans styles

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/CharSequence;)V` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `length()I` | Supportée |  |
| `charAt(I)C` | Supportée |  |

## `android.util.AndroidRuntimeException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.util.AttributeSet` (interface)

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.util.Log`

| Membre | Statut | Note |
|---|---|---|
| `v(Ljava/lang/String;Ljava/lang/String;)I` (statique) | Supportée |  |
| `v(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I` (statique) | Supportée |  |
| `d(Ljava/lang/String;Ljava/lang/String;)I` (statique) | Supportée |  |
| `d(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I` (statique) | Supportée |  |
| `i(Ljava/lang/String;Ljava/lang/String;)I` (statique) | Supportée |  |
| `i(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I` (statique) | Supportée |  |
| `w(Ljava/lang/String;Ljava/lang/String;)I` (statique) | Supportée |  |
| `w(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I` (statique) | Supportée |  |
| `e(Ljava/lang/String;Ljava/lang/String;)I` (statique) | Supportée |  |
| `e(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I` (statique) | Supportée |  |
| `wtf(Ljava/lang/String;Ljava/lang/String;)I` (statique) | Supportée |  |
| `wtf(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I` (statique) | Supportée |  |
| `isLoggable(Ljava/lang/String;I)Z` (statique) | Supportée |  |

## `android.util.SuperNotCalledException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.view.ContextThemeWrapper`

| Membre | Statut | Note |
|---|---|---|
| `setTheme(I)V` | Stub | thèmes non pris en charge |

## `android.view.InflateException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.view.LayoutInflater`

| Membre | Statut | Note |
|---|---|---|
| `from(Landroid/content/Context;)Landroid/view/LayoutInflater;` (statique) | Supportée |  |
| `getContext()Landroid/content/Context;` | Supportée |  |
| `cloneInContext(Landroid/content/Context;)Landroid/view/LayoutInflater;` | Supportée |  |
| `inflate(ILandroid/view/ViewGroup;)Landroid/view/View;` | Partielle | vues du pont, attributs courants (texte, id, couleurs, tailles, poids, visibilité, onClick) |
| `inflate(ILandroid/view/ViewGroup;Z)Landroid/view/View;` | Partielle | vues du pont, attributs courants (texte, id, couleurs, tailles, poids, visibilité, onClick) |

## `android.view.View$DeclaredOnClickListener`

| Membre | Statut | Note |
|---|---|---|
| `onClick(Landroid/view/View;)V` | Supportée |  |

## `android.view.View$OnClickListener` (interface)

| Membre | Statut | Note |
|---|---|---|
| `onClick(Landroid/view/View;)V` (abstraite) | Supportée |  |

## `android.view.View$OnLongClickListener` (interface)

| Membre | Statut | Note |
|---|---|---|
| `onLongClick(Landroid/view/View;)Z` (abstraite) | Supportée |  |

## `android.view.View`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Landroid/content/Context;)V` | Supportée |  |
| `<init>(Landroid/content/Context;Landroid/util/AttributeSet;)V` | Partielle | attributs XML ignorés |
| `setOnClickListener(Landroid/view/View$OnClickListener;)V` | Supportée |  |
| `performClick()Z` | Supportée |  |
| `callOnClick()Z` | Supportée |  |
| `setId(I)V` | Supportée |  |
| `getId()I` | Supportée |  |
| `setVisibility(I)V` | Supportée |  |
| `getVisibility()I` | Supportée |  |
| `setEnabled(Z)V` | Supportée |  |
| `isEnabled()Z` | Supportée |  |
| `setClickable(Z)V` | Supportée |  |
| `isClickable()Z` | Supportée |  |
| `getContext()Landroid/content/Context;` | Supportée |  |
| `setTag(Ljava/lang/Object;)V` | Supportée |  |
| `getTag()Ljava/lang/Object;` | Supportée |  |
| `setBackgroundColor(I)V` | Partielle | couleur unie uniquement |
| `setLayoutParams(Landroid/view/ViewGroup$LayoutParams;)V` | Partielle | largeur, hauteur et poids uniquement |
| `getLayoutParams()Landroid/view/ViewGroup$LayoutParams;` | Supportée |  |
| `getParent()Landroid/view/ViewParent;` | Supportée |  |
| `post(Ljava/lang/Runnable;)Z` | Supportée |  |
| `setPadding(IIII)V` | Stub | marges internes ignorées |
| `invalidate()V` | Stub | le backend redessine automatiquement |
| `requestLayout()V` | Stub | le backend recalcule la mise en page automatiquement |
| `setOnLongClickListener(Landroid/view/View$OnLongClickListener;)V` | Stub | appui long jamais déclenché |
| `getWidth()I` | Stub | dimensions non calculées côté pont (renvoie 0) |
| `getHeight()I` | Stub | dimensions non calculées côté pont (renvoie 0) |
| `requestFocus()Z` | Stub | focus géré par Windows |

## `android.view.ViewGroup$LayoutParams`

| Membre | Statut | Note |
|---|---|---|
| `<init>(II)V` | Supportée |  |

## `android.view.ViewGroup$MarginLayoutParams`

| Membre | Statut | Note |
|---|---|---|
| `<init>(II)V` | Supportée |  |
| `setMargins(IIII)V` | Stub | marges ignorées |

## `android.view.ViewGroup`

| Membre | Statut | Note |
|---|---|---|
| `addView(Landroid/view/View;)V` | Supportée |  |
| `addView(Landroid/view/View;I)V` | Supportée |  |
| `addView(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V` | Partielle | largeur, hauteur et poids uniquement |
| `removeView(Landroid/view/View;)V` | Supportée |  |
| `removeViewAt(I)V` | Supportée |  |
| `removeAllViews()V` | Supportée |  |
| `getChildCount()I` | Supportée |  |
| `getChildAt(I)Landroid/view/View;` | Supportée |  |
| `indexOfChild(Landroid/view/View;)I` | Supportée |  |

## `android.view.ViewManager` (interface)

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.view.ViewParent` (interface)

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.view.ViewRootImpl$CalledFromWrongThreadException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `android.widget.Button`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Landroid/content/Context;)V` | Supportée |  |
| `<init>(Landroid/content/Context;Landroid/util/AttributeSet;)V` | Partielle | attributs XML ignorés |

## `android.widget.EditText`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Landroid/content/Context;)V` | Supportée |  |
| `<init>(Landroid/content/Context;Landroid/util/AttributeSet;)V` | Partielle | attributs XML ignorés |
| `getText()Landroid/text/Editable;` | Partielle | copie du texte (les modifications de l'Editable ne sont pas répercutées) |
| `setSelection(I)V` | Stub | sélection gérée par Windows |

## `android.widget.FrameLayout`

Classe : **Partielle** — enfants empilés verticalement au lieu d'être superposés

| Membre | Statut | Note |
|---|---|---|
| `<init>(Landroid/content/Context;)V` | Partielle | enfants empilés verticalement |
| `<init>(Landroid/content/Context;Landroid/util/AttributeSet;)V` | Partielle | attributs XML ignorés, enfants empilés verticalement |

## `android.widget.LinearLayout$LayoutParams`

| Membre | Statut | Note |
|---|---|---|
| `<init>(II)V` | Supportée |  |
| `<init>(IIF)V` | Supportée |  |

## `android.widget.LinearLayout`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Landroid/content/Context;)V` | Supportée |  |
| `<init>(Landroid/content/Context;Landroid/util/AttributeSet;)V` | Partielle | attributs XML ignorés |
| `setOrientation(I)V` | Supportée |  |
| `getOrientation()I` | Supportée |  |
| `setGravity(I)V` | Stub | gravité ignorée |
| `setWeightSum(F)V` | Stub | somme des poids calculée automatiquement |

## `android.widget.ScrollView`

Classe : **Partielle** — pas de défilement

| Membre | Statut | Note |
|---|---|---|
| `<init>(Landroid/content/Context;)V` | Partielle | pas de défilement |
| `<init>(Landroid/content/Context;Landroid/util/AttributeSet;)V` | Partielle | attributs XML ignorés, pas de défilement |

## `android.widget.TextView`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Landroid/content/Context;)V` | Supportée |  |
| `<init>(Landroid/content/Context;Landroid/util/AttributeSet;)V` | Partielle | attributs XML ignorés |
| `setText(Ljava/lang/CharSequence;)V` | Supportée |  |
| `getText()Ljava/lang/CharSequence;` | Supportée |  |
| `append(Ljava/lang/CharSequence;)V` | Supportée |  |
| `length()I` | Supportée |  |
| `setHint(Ljava/lang/CharSequence;)V` | Supportée |  |
| `getHint()Ljava/lang/CharSequence;` | Supportée |  |
| `setTextSize(F)V` | Partielle | taille en sp appliquée à la police Windows |
| `setTextSize(IF)V` | Partielle | unité ignorée (traitée comme sp) |
| `setTextColor(I)V` | Partielle | couleur unie uniquement |
| `setGravity(I)V` | Stub | gravité ignorée |
| `setSingleLine()V` | Stub | mise en forme ignorée |
| `setMaxLines(I)V` | Stub | mise en forme ignorée |
| `setInputType(I)V` | Stub | type de saisie ignoré |
| `setText(I)V` | Supportée |  |
| `setHint(I)V` | Supportée |  |

## `android.widget.Toast`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Landroid/content/Context;)V` | Supportée |  |
| `makeText(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;` (statique) | Supportée |  |
| `show()V` | Supportée |  |
| `setText(Ljava/lang/CharSequence;)V` | Supportée |  |
| `setDuration(I)V` | Supportée |  |
| `cancel()V` | Stub | le toast disparaît de lui-même |
| `makeText(Landroid/content/Context;II)Landroid/widget/Toast;` (statique) | Supportée |  |

## `java.io.IOException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.io.PrintStream`

| Membre | Statut | Note |
|---|---|---|
| `println(Ljava/lang/String;)V` | Supportée |  |
| `print(Ljava/lang/String;)V` | Supportée |  |
| `println(Ljava/lang/Object;)V` | Supportée |  |
| `print(Ljava/lang/Object;)V` | Supportée |  |
| `println(I)V` | Supportée |  |
| `print(I)V` | Supportée |  |
| `println(J)V` | Supportée |  |
| `print(J)V` | Supportée |  |
| `println(Z)V` | Supportée |  |
| `print(Z)V` | Supportée |  |
| `println(C)V` | Supportée |  |
| `print(C)V` | Supportée |  |
| `println(F)V` | Supportée |  |
| `print(F)V` | Supportée |  |
| `println(D)V` | Supportée |  |
| `print(D)V` | Supportée |  |
| `println()V` | Supportée |  |
| `flush()V` | Supportée |  |
| `printf(Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;` | Partielle | sous-ensemble des formats (%s %d %x %c %b %f %n) |

## `java.io.Serializable` (interface)

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.lang.AbstractMethodError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.ArithmeticException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.ArrayIndexOutOfBoundsException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.ArrayStoreException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.AssertionError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Object;)V` | Supportée |  |

## `java.lang.AutoCloseable` (interface)

| Membre | Statut | Note |
|---|---|---|
| `close()V` (abstraite) | Supportée |  |

## `java.lang.Boolean`

| Membre | Statut | Note |
|---|---|---|
| `TRUE:Ljava/lang/Boolean;` (statique) | Supportée |  |
| `FALSE:Ljava/lang/Boolean;` (statique) | Supportée |  |
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |
| `<init>(Z)V` | Supportée |  |
| `valueOf(Z)Ljava/lang/Boolean;` (statique) | Supportée |  |
| `booleanValue()Z` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `toString(Z)Ljava/lang/String;` (statique) | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `compareTo(Ljava/lang/Boolean;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |
| `parseBoolean(Ljava/lang/String;)Z` (statique) | Supportée |  |

## `java.lang.Byte`

| Membre | Statut | Note |
|---|---|---|
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |
| `<init>(B)V` | Supportée |  |
| `valueOf(B)Ljava/lang/Byte;` (statique) | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `toString(B)Ljava/lang/String;` (statique) | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `compareTo(Ljava/lang/Byte;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |

## `java.lang.CharSequence` (interface)

| Membre | Statut | Note |
|---|---|---|
| `length()I` (abstraite) | Supportée |  |
| `charAt(I)C` (abstraite) | Supportée |  |
| `toString()Ljava/lang/String;` (abstraite) | Supportée |  |
| `subSequence(II)Ljava/lang/CharSequence;` (abstraite) | Supportée |  |

## `java.lang.Character`

| Membre | Statut | Note |
|---|---|---|
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |
| `<init>(C)V` | Supportée |  |
| `valueOf(C)Ljava/lang/Character;` (statique) | Supportée |  |
| `charValue()C` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `toString(C)Ljava/lang/String;` (statique) | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `compareTo(Ljava/lang/Character;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |
| `isDigit(C)Z` (statique) | Partielle | chiffres ASCII uniquement |
| `isLetter(C)Z` (statique) | Partielle | alphabet latin uniquement |
| `isWhitespace(C)Z` (statique) | Supportée |  |
| `toUpperCase(C)C` (statique) | Partielle | ASCII et Latin-1 uniquement |
| `toLowerCase(C)C` (statique) | Partielle | ASCII et Latin-1 uniquement |

## `java.lang.Class`

| Membre | Statut | Note |
|---|---|---|
| `getName()Ljava/lang/String;` | Supportée |  |
| `getSimpleName()Ljava/lang/String;` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `desiredAssertionStatus()Z` | Supportée |  |
| `isInstance(Ljava/lang/Object;)Z` | Supportée |  |
| `isArray()Z` | Supportée |  |
| `isInterface()Z` | Supportée |  |
| `getSuperclass()Ljava/lang/Class;` | Supportée |  |
| `newInstance()Ljava/lang/Object;` | Partielle | constructeur sans argument uniquement, sans contrôle d'accès |
| `forName(Ljava/lang/String;)Ljava/lang/Class;` (statique) | Supportée |  |
| `forName(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;` (statique) | Partielle | un seul espace de classes (ADR-006 J2) ; sans libcore, le chargeur est ignoré |
| `getDeclaredMethods()[Ljava/lang/reflect/Method;` | Supportée |  |
| `getMethods()[Ljava/lang/reflect/Method;` | Supportée |  |
| `getDeclaredMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;` | Supportée |  |
| `getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;` | Supportée |  |
| `getDeclaredConstructors()[Ljava/lang/reflect/Constructor;` | Supportée |  |
| `getConstructors()[Ljava/lang/reflect/Constructor;` | Supportée |  |
| `getDeclaredConstructor([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;` | Supportée |  |
| `getConstructor([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;` | Supportée |  |
| `getDeclaredFields()[Ljava/lang/reflect/Field;` | Supportée |  |
| `getFields()[Ljava/lang/reflect/Field;` | Supportée |  |
| `getDeclaredField(Ljava/lang/String;)Ljava/lang/reflect/Field;` | Supportée |  |
| `getField(Ljava/lang/String;)Ljava/lang/reflect/Field;` | Supportée |  |
| `getInterfaces()[Ljava/lang/Class;` | Supportée |  |
| `getModifiers()I` | Partielle | modificateurs des classes internes non lus (InnerClass) |
| `isAssignableFrom(Ljava/lang/Class;)Z` | Supportée |  |
| `getComponentType()Ljava/lang/Class;` | Supportée |  |
| `isPrimitive()Z` | Supportée |  |
| `isEnum()Z` | Supportée |  |
| `cast(Ljava/lang/Object;)Ljava/lang/Object;` | Supportée |  |
| `getClassLoader()Ljava/lang/ClassLoader;` | Supportée |  |
| `getAnnotation(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;` | Non supportée | annotations DEX non lues dans cette version (prévu en G2) |
| `getAnnotations()[Ljava/lang/annotation/Annotation;` | Non supportée | annotations DEX non lues dans cette version (prévu en G2) |
| `isAnnotationPresent(Ljava/lang/Class;)Z` | Non supportée | annotations DEX non lues dans cette version (prévu en G2) |
| `getEnclosingClass()Ljava/lang/Class;` | Supportée |  |
| `getDeclaringClass()Ljava/lang/Class;` | Supportée |  |

## `java.lang.ClassCastException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.ClassCircularityError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.ClassFormatError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.ClassLoader`

Classe : **Partielle** — un seul chargeur (classes de l'application) ; pas de chargement dynamique de DEX

| Membre | Statut | Note |
|---|---|---|
| `getSystemClassLoader()Ljava/lang/ClassLoader;` (statique) | Supportée |  |
| `loadClass(Ljava/lang/String;)Ljava/lang/Class;` | Supportée |  |
| `getParent()Ljava/lang/ClassLoader;` | Supportée |  |

## `java.lang.ClassNotFoundException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.CloneNotSupportedException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Cloneable` (interface)

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.lang.Comparable` (interface)

| Membre | Statut | Note |
|---|---|---|
| `compareTo(Ljava/lang/Object;)I` (abstraite) | Supportée |  |

## `java.lang.Double`

| Membre | Statut | Note |
|---|---|---|
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |
| `<init>(D)V` | Supportée |  |
| `valueOf(D)Ljava/lang/Double;` (statique) | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `toString(D)Ljava/lang/String;` (statique) | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `compareTo(Ljava/lang/Double;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |
| `parseDouble(Ljava/lang/String;)D` (statique) | Supportée |  |
| `isNaN(D)Z` (statique) | Supportée |  |

## `java.lang.Enum`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Ljava/lang/String;I)V` | Supportée |  |
| `name()Ljava/lang/String;` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `ordinal()I` | Supportée |  |
| `compareTo(Ljava/lang/Enum;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |
| `getDeclaringClass()Ljava/lang/Class;` | Supportée |  |
| `valueOf(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;` (statique) | Supportée |  |

## `java.lang.Error`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Exception`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.ExceptionInInitializerError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Float`

| Membre | Statut | Note |
|---|---|---|
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |
| `<init>(F)V` | Supportée |  |
| `valueOf(F)Ljava/lang/Float;` (statique) | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `toString(F)Ljava/lang/String;` (statique) | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `compareTo(Ljava/lang/Float;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |
| `parseFloat(Ljava/lang/String;)F` (statique) | Supportée |  |
| `isNaN(F)Z` (statique) | Supportée |  |

## `java.lang.IllegalAccessException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.lang.IllegalArgumentException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.IllegalMonitorStateException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.lang.IllegalStateException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.IllegalThreadStateException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.lang.IncompatibleClassChangeError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.IndexOutOfBoundsException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.InstantiationError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.InstantiationException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Integer`

| Membre | Statut | Note |
|---|---|---|
| `MAX_VALUE:I` (statique) | Supportée |  |
| `MIN_VALUE:I` (statique) | Supportée |  |
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |
| `<init>(I)V` | Supportée |  |
| `valueOf(I)Ljava/lang/Integer;` (statique) | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `toString(I)Ljava/lang/String;` (statique) | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `compareTo(Ljava/lang/Integer;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |
| `parseInt(Ljava/lang/String;)I` (statique) | Supportée |  |
| `parseInt(Ljava/lang/String;I)I` (statique) | Supportée |  |
| `valueOf(Ljava/lang/String;)Ljava/lang/Integer;` (statique) | Supportée |  |
| `toHexString(I)Ljava/lang/String;` (statique) | Supportée |  |
| `toBinaryString(I)Ljava/lang/String;` (statique) | Supportée |  |
| `compare(II)I` (statique) | Supportée |  |

## `java.lang.InternalError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.InterruptedException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Iterable` (interface)

| Membre | Statut | Note |
|---|---|---|
| `iterator()Ljava/util/Iterator;` (abstraite) | Supportée |  |

## `java.lang.LinkageError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Long`

| Membre | Statut | Note |
|---|---|---|
| `MAX_VALUE:J` (statique) | Supportée |  |
| `MIN_VALUE:J` (statique) | Supportée |  |
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |
| `<init>(J)V` | Supportée |  |
| `valueOf(J)Ljava/lang/Long;` (statique) | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `toString(J)Ljava/lang/String;` (statique) | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `compareTo(Ljava/lang/Long;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |
| `parseLong(Ljava/lang/String;)J` (statique) | Supportée |  |

## `java.lang.Math`

| Membre | Statut | Note |
|---|---|---|
| `PI:D` (statique) | Supportée |  |
| `E:D` (statique) | Supportée |  |
| `abs(I)I` (statique) | Supportée |  |
| `abs(J)J` (statique) | Supportée |  |
| `abs(F)F` (statique) | Supportée |  |
| `abs(D)D` (statique) | Supportée |  |
| `max(II)I` (statique) | Supportée |  |
| `min(II)I` (statique) | Supportée |  |
| `max(JJ)J` (statique) | Supportée |  |
| `min(JJ)J` (statique) | Supportée |  |
| `max(FF)F` (statique) | Supportée |  |
| `min(FF)F` (statique) | Supportée |  |
| `max(DD)D` (statique) | Supportée |  |
| `min(DD)D` (statique) | Supportée |  |
| `sqrt(D)D` (statique) | Supportée |  |
| `cbrt(D)D` (statique) | Supportée |  |
| `floor(D)D` (statique) | Supportée |  |
| `ceil(D)D` (statique) | Supportée |  |
| `sin(D)D` (statique) | Supportée |  |
| `cos(D)D` (statique) | Supportée |  |
| `tan(D)D` (statique) | Supportée |  |
| `atan(D)D` (statique) | Supportée |  |
| `exp(D)D` (statique) | Supportée |  |
| `log(D)D` (statique) | Supportée |  |
| `log10(D)D` (statique) | Supportée |  |
| `signum(D)D` (statique) | Supportée |  |
| `toRadians(D)D` (statique) | Supportée |  |
| `toDegrees(D)D` (statique) | Supportée |  |
| `pow(DD)D` (statique) | Supportée |  |
| `atan2(DD)D` (statique) | Supportée |  |
| `hypot(DD)D` (statique) | Supportée |  |
| `round(F)I` (statique) | Supportée |  |
| `round(D)J` (statique) | Supportée |  |
| `random()D` (statique) | Supportée |  |

## `java.lang.NegativeArraySizeException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.NoClassDefFoundError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.NoSuchFieldError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.NoSuchFieldException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.lang.NoSuchMethodError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.NoSuchMethodException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.lang.NullPointerException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Number`

| Membre | Statut | Note |
|---|---|---|
| `intValue()I` | Supportée |  |
| `longValue()J` | Supportée |  |
| `shortValue()S` | Supportée |  |
| `byteValue()B` | Supportée |  |
| `floatValue()F` | Supportée |  |
| `doubleValue()D` | Supportée |  |

## `java.lang.NumberFormatException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Object`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `hashCode()I` | Supportée |  |
| `identityHashCode(Ljava/lang/Object;)I` (statique) | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `getClass()Ljava/lang/Class;` | Supportée |  |
| `clone()Ljava/lang/Object;` | Supportée |  |
| `notify()V` | Supportée |  |
| `notifyAll()V` | Supportée |  |
| `wait()V` | Supportée |  |
| `wait(J)V` | Supportée |  |
| `wait(JI)V` | Supportée |  |
| `finalize()V` | Stub | finaliseurs jamais appelés par le ramasse-miettes |

## `java.lang.OutOfMemoryError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.ReflectiveOperationException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Runnable` (interface)

| Membre | Statut | Note |
|---|---|---|
| `run()V` (abstraite) | Supportée |  |

## `java.lang.RuntimeException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.SecurityException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Short`

| Membre | Statut | Note |
|---|---|---|
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |
| `<init>(S)V` | Supportée |  |
| `valueOf(S)Ljava/lang/Short;` (statique) | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `toString(S)Ljava/lang/String;` (statique) | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `compareTo(Ljava/lang/Short;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |

## `java.lang.StackOverflowError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.String`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>([C)V` | Supportée |  |
| `<init>([CII)V` | Supportée |  |
| `<init>([B)V` | Partielle | décodage UTF-8 uniquement |
| `length()I` | Supportée |  |
| `isEmpty()Z` | Supportée |  |
| `charAt(I)C` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `hashCode()I` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `equalsIgnoreCase(Ljava/lang/String;)Z` | Partielle | casse ASCII et Latin-1 uniquement |
| `compareTo(Ljava/lang/String;)I` | Supportée |  |
| `compareTo(Ljava/lang/Object;)I` | Supportée |  |
| `concat(Ljava/lang/String;)Ljava/lang/String;` | Supportée |  |
| `substring(I)Ljava/lang/String;` | Supportée |  |
| `substring(II)Ljava/lang/String;` | Supportée |  |
| `subSequence(II)Ljava/lang/CharSequence;` | Supportée |  |
| `fastSubstring(II)Ljava/lang/String;` | Supportée |  |
| `getChars(II[CI)V` | Supportée |  |
| `getCharsNoCheck(II[CI)V` | Supportée |  |
| `indexOf(I)I` | Partielle | caractères du plan multilingue de base uniquement |
| `indexOf(II)I` | Partielle | caractères du plan multilingue de base uniquement |
| `indexOf(Ljava/lang/String;)I` | Supportée |  |
| `indexOf(Ljava/lang/String;I)I` | Supportée |  |
| `lastIndexOf(I)I` | Partielle | caractères du plan multilingue de base uniquement |
| `lastIndexOf(Ljava/lang/String;)I` | Supportée |  |
| `contains(Ljava/lang/CharSequence;)Z` | Supportée |  |
| `startsWith(Ljava/lang/String;)Z` | Supportée |  |
| `startsWith(Ljava/lang/String;I)Z` | Supportée |  |
| `endsWith(Ljava/lang/String;)Z` | Supportée |  |
| `trim()Ljava/lang/String;` | Supportée |  |
| `toUpperCase()Ljava/lang/String;` | Partielle | casse ASCII et Latin-1 uniquement |
| `toLowerCase()Ljava/lang/String;` | Partielle | casse ASCII et Latin-1 uniquement |
| `replace(CC)Ljava/lang/String;` | Supportée |  |
| `replace(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;` | Supportée |  |
| `split(Ljava/lang/String;)[Ljava/lang/String;` | Partielle | séparateurs littéraux uniquement |
| `toCharArray()[C` | Supportée |  |
| `getBytes()[B` | Partielle | encodage UTF-8 uniquement |
| `intern()Ljava/lang/String;` | Supportée |  |
| `repeat(I)Ljava/lang/String;` | Supportée |  |
| `valueOf(Ljava/lang/Object;)Ljava/lang/String;` (statique) | Supportée |  |
| `valueOf(I)Ljava/lang/String;` (statique) | Supportée |  |
| `valueOf(J)Ljava/lang/String;` (statique) | Supportée |  |
| `valueOf(Z)Ljava/lang/String;` (statique) | Supportée |  |
| `valueOf(C)Ljava/lang/String;` (statique) | Supportée |  |
| `valueOf(F)Ljava/lang/String;` (statique) | Supportée |  |
| `valueOf(D)Ljava/lang/String;` (statique) | Supportée |  |
| `valueOf([C)Ljava/lang/String;` (statique) | Supportée |  |
| `matches(Ljava/lang/String;)Z` | Non supportée | expressions régulières non prises en charge |
| `replaceAll(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;` | Non supportée | expressions régulières non prises en charge |
| `format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;` (statique) | Partielle | sous-ensemble des formats (%s %d %x %c %b %f %n), sans Locale |

## `java.lang.StringBuffer`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(I)V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/CharSequence;)V` | Supportée |  |
| `append(Ljava/lang/String;)Ljava/lang/StringBuffer;` | Supportée |  |
| `append(Ljava/lang/Object;)Ljava/lang/StringBuffer;` | Supportée |  |
| `append(Ljava/lang/CharSequence;)Ljava/lang/StringBuffer;` | Supportée |  |
| `append(I)Ljava/lang/StringBuffer;` | Supportée |  |
| `append(J)Ljava/lang/StringBuffer;` | Supportée |  |
| `append(Z)Ljava/lang/StringBuffer;` | Supportée |  |
| `append(C)Ljava/lang/StringBuffer;` | Supportée |  |
| `append(F)Ljava/lang/StringBuffer;` | Supportée |  |
| `append(D)Ljava/lang/StringBuffer;` | Supportée |  |
| `append([C)Ljava/lang/StringBuffer;` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `length()I` | Supportée |  |
| `charAt(I)C` | Supportée |  |
| `reverse()Ljava/lang/StringBuffer;` | Partielle | les paires de substitution ne sont pas préservées |
| `setLength(I)V` | Supportée |  |
| `insert(ILjava/lang/String;)Ljava/lang/StringBuffer;` | Supportée |  |
| `deleteCharAt(I)Ljava/lang/StringBuffer;` | Supportée |  |
| `indexOf(Ljava/lang/String;)I` | Supportée |  |

## `java.lang.StringBuilder`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(I)V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/CharSequence;)V` | Supportée |  |
| `append(Ljava/lang/String;)Ljava/lang/StringBuilder;` | Supportée |  |
| `append(Ljava/lang/Object;)Ljava/lang/StringBuilder;` | Supportée |  |
| `append(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;` | Supportée |  |
| `append(I)Ljava/lang/StringBuilder;` | Supportée |  |
| `append(J)Ljava/lang/StringBuilder;` | Supportée |  |
| `append(Z)Ljava/lang/StringBuilder;` | Supportée |  |
| `append(C)Ljava/lang/StringBuilder;` | Supportée |  |
| `append(F)Ljava/lang/StringBuilder;` | Supportée |  |
| `append(D)Ljava/lang/StringBuilder;` | Supportée |  |
| `append([C)Ljava/lang/StringBuilder;` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `length()I` | Supportée |  |
| `charAt(I)C` | Supportée |  |
| `reverse()Ljava/lang/StringBuilder;` | Partielle | les paires de substitution ne sont pas préservées |
| `setLength(I)V` | Supportée |  |
| `insert(ILjava/lang/String;)Ljava/lang/StringBuilder;` | Supportée |  |
| `deleteCharAt(I)Ljava/lang/StringBuilder;` | Supportée |  |
| `indexOf(Ljava/lang/String;)I` | Supportée |  |

## `java.lang.StringIndexOutOfBoundsException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.System`

| Membre | Statut | Note |
|---|---|---|
| `out:Ljava/io/PrintStream;` (statique) | Supportée |  |
| `err:Ljava/io/PrintStream;` (statique) | Supportée |  |
| `currentTimeMillis()J` (statique) | Supportée |  |
| `nanoTime()J` (statique) | Supportée |  |
| `exit(I)V` (statique) | Supportée |  |
| `identityHashCode(Ljava/lang/Object;)I` (statique) | Supportée |  |
| `lineSeparator()Ljava/lang/String;` (statique) | Supportée |  |
| `gc()V` (statique) | Partielle | collecte au prochain point de sûreté |
| `loadLibrary(Ljava/lang/String;)V` (statique) | Non supportée | bibliothèques natives ELF non prises en charge (couche native prévue après le MVP) |
| `load(Ljava/lang/String;)V` (statique) | Non supportée | bibliothèques natives ELF non prises en charge |
| `getProperty(Ljava/lang/String;)Ljava/lang/String;` (statique) | Partielle | quelques propriétés seulement |
| `arraycopy(Ljava/lang/Object;ILjava/lang/Object;II)V` (statique) | Supportée |  |

## `java.lang.Thread$UncaughtExceptionHandler` (interface)

| Membre | Statut | Note |
|---|---|---|
| `uncaughtException(Ljava/lang/Thread;Ljava/lang/Throwable;)V` (abstraite) | Supportée |  |

## `java.lang.Thread`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/Runnable;)V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/Runnable;Ljava/lang/String;)V` | Supportée |  |
| `run()V` | Supportée |  |
| `start()V` | Supportée |  |
| `currentThread()Ljava/lang/Thread;` (statique) | Supportée |  |
| `sleep(J)V` (statique) | Supportée |  |
| `sleep(JI)V` (statique) | Supportée |  |
| `yield()V` (statique) | Supportée |  |
| `interrupted()Z` (statique) | Supportée |  |
| `holdsLock(Ljava/lang/Object;)Z` (statique) | Supportée |  |
| `interrupt()V` | Supportée |  |
| `isInterrupted()Z` | Supportée |  |
| `join()V` | Supportée |  |
| `join(J)V` | Supportée |  |
| `isAlive()Z` | Supportée |  |
| `getName()Ljava/lang/String;` | Supportée |  |
| `setName(Ljava/lang/String;)V` | Supportée |  |
| `getId()J` | Supportée |  |
| `setDaemon(Z)V` | Supportée |  |
| `isDaemon()Z` | Supportée |  |
| `setPriority(I)V` | Partielle | mémorisée, sans effet sur l'ordonnancement (verrou global) |
| `getPriority()I` | Supportée |  |
| `setUncaughtExceptionHandler(Ljava/lang/Thread$UncaughtExceptionHandler;)V` | Supportée |  |
| `getUncaughtExceptionHandler()Ljava/lang/Thread$UncaughtExceptionHandler;` | Supportée |  |
| `setDefaultUncaughtExceptionHandler(Ljava/lang/Thread$UncaughtExceptionHandler;)V` (statique) | Supportée |  |
| `getDefaultUncaughtExceptionHandler()Ljava/lang/Thread$UncaughtExceptionHandler;` (statique) | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |

## `java.lang.Throwable`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |
| `getMessage()Ljava/lang/String;` | Supportée |  |
| `getLocalizedMessage()Ljava/lang/String;` | Supportée |  |
| `getCause()Ljava/lang/Throwable;` | Supportée |  |
| `initCause(Ljava/lang/Throwable;)Ljava/lang/Throwable;` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `printStackTrace()V` | Partielle | écrit dans le journal AnyAndroid |
| `addSuppressed(Ljava/lang/Throwable;)V` | Stub | exceptions supprimées ignorées |
| `getStackTrace()[Ljava/lang/StackTraceElement;` | Non supportée | StackTraceElement non implémenté |

## `java.lang.UnsatisfiedLinkError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.UnsupportedOperationException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.VerifyError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.VirtualMachineError`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.lang.Void`

| Membre | Statut | Note |
|---|---|---|
| `TYPE:Ljava/lang/Class;` (statique) | Supportée |  |

## `java.lang.ref.Reference`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Ljava/lang/Object;)V` | Supportée |  |
| `<init>(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V` | Partielle | jamais mise en file par le ramasse-miettes |
| `get()Ljava/lang/Object;` | Supportée |  |
| `clear()V` | Supportée |  |
| `reachabilityFence(Ljava/lang/Object;)V` (statique) | Supportée |  |

## `java.lang.ref.ReferenceQueue`

Classe : **Partielle** — les références ne sont jamais mises en file

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |

## `java.lang.ref.SoftReference`

Classe : **Partielle** — conservée tant que le prototype n'a pas de notion de pression mémoire

| Membre | Statut | Note |
|---|---|---|
| `<init>(Ljava/lang/Object;)V` | Supportée |  |
| `<init>(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V` | Partielle | jamais mise en file par le ramasse-miettes |

## `java.lang.ref.WeakReference`

| Membre | Statut | Note |
|---|---|---|
| `<init>(Ljava/lang/Object;)V` | Supportée |  |
| `<init>(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V` | Partielle | jamais mise en file par le ramasse-miettes |

## `java.lang.reflect.AccessibleObject`

| Membre | Statut | Note |
|---|---|---|
| `setAccessible(Z)V` | Partielle | aucun contrôle d'accès n'est appliqué par le prototype |
| `isAccessible()Z` | Supportée |  |
| `getAnnotation(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;` | Non supportée | annotations DEX non lues dans cette version (prévu en G2) |
| `isAnnotationPresent(Ljava/lang/Class;)Z` | Non supportée | annotations DEX non lues dans cette version (prévu en G2) |

## `java.lang.reflect.Array`

| Membre | Statut | Note |
|---|---|---|
| `newInstance(Ljava/lang/Class;I)Ljava/lang/Object;` (statique) | Supportée |  |
| `getLength(Ljava/lang/Object;)I` (statique) | Supportée |  |
| `get(Ljava/lang/Object;I)Ljava/lang/Object;` (statique) | Supportée |  |
| `set(Ljava/lang/Object;ILjava/lang/Object;)V` (statique) | Supportée |  |

## `java.lang.reflect.Constructor`

| Membre | Statut | Note |
|---|---|---|
| `getName()Ljava/lang/String;` | Supportée |  |
| `getModifiers()I` | Supportée |  |
| `getDeclaringClass()Ljava/lang/Class;` | Supportée |  |
| `getParameterTypes()[Ljava/lang/Class;` | Supportée |  |
| `getParameterCount()I` | Supportée |  |
| `getExceptionTypes()[Ljava/lang/Class;` | Partielle | clause throws non lue (annotations) |
| `isVarArgs()Z` | Supportée |  |
| `isSynthetic()Z` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `hashCode()I` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `newInstance([Ljava/lang/Object;)Ljava/lang/Object;` | Supportée |  |

## `java.lang.reflect.Field`

| Membre | Statut | Note |
|---|---|---|
| `getName()Ljava/lang/String;` | Supportée |  |
| `getType()Ljava/lang/Class;` | Supportée |  |
| `getModifiers()I` | Supportée |  |
| `getDeclaringClass()Ljava/lang/Class;` | Supportée |  |
| `getArtField()J` | Partielle | identifiant opaque, valable pour les VarHandle |
| `getOffset()I` | Partielle | décalage opaque, valable uniquement pour sun.misc.Unsafe |
| `get(Ljava/lang/Object;)Ljava/lang/Object;` | Supportée |  |
| `set(Ljava/lang/Object;Ljava/lang/Object;)V` | Supportée |  |
| `getInt(Ljava/lang/Object;)I` | Supportée |  |
| `getLong(Ljava/lang/Object;)J` | Supportée |  |
| `getBoolean(Ljava/lang/Object;)Z` | Supportée |  |
| `getFloat(Ljava/lang/Object;)F` | Supportée |  |
| `getDouble(Ljava/lang/Object;)D` | Supportée |  |
| `setInt(Ljava/lang/Object;I)V` | Supportée |  |
| `setBoolean(Ljava/lang/Object;Z)V` | Supportée |  |

## `java.lang.reflect.InvocationTargetException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;Ljava/lang/String;)V` | Supportée |  |
| `getTargetException()Ljava/lang/Throwable;` | Supportée |  |
| `getCause()Ljava/lang/Throwable;` | Supportée |  |

## `java.lang.reflect.Member` (interface)

| Membre | Statut | Note |
|---|---|---|
| `getName()Ljava/lang/String;` (abstraite) | Supportée |  |
| `getModifiers()I` (abstraite) | Supportée |  |
| `getDeclaringClass()Ljava/lang/Class;` (abstraite) | Supportée |  |

## `java.lang.reflect.Method`

| Membre | Statut | Note |
|---|---|---|
| `getName()Ljava/lang/String;` | Supportée |  |
| `getModifiers()I` | Supportée |  |
| `getDeclaringClass()Ljava/lang/Class;` | Supportée |  |
| `getParameterTypes()[Ljava/lang/Class;` | Supportée |  |
| `getParameterCount()I` | Supportée |  |
| `getExceptionTypes()[Ljava/lang/Class;` | Partielle | clause throws non lue (annotations) |
| `isVarArgs()Z` | Supportée |  |
| `isSynthetic()Z` | Supportée |  |
| `isBridge()Z` | Supportée |  |
| `equals(Ljava/lang/Object;)Z` | Supportée |  |
| `hashCode()I` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `getReturnType()Ljava/lang/Class;` | Supportée |  |
| `invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;` | Supportée |  |

## `java.lang.reflect.Modifier`

| Membre | Statut | Note |
|---|---|---|
| `isPublic(I)Z` (statique) | Supportée |  |
| `isPrivate(I)Z` (statique) | Supportée |  |
| `isProtected(I)Z` (statique) | Supportée |  |
| `isStatic(I)Z` (statique) | Supportée |  |
| `isFinal(I)Z` (statique) | Supportée |  |
| `isSynchronized(I)Z` (statique) | Supportée |  |
| `isVolatile(I)Z` (statique) | Supportée |  |
| `isTransient(I)Z` (statique) | Supportée |  |
| `isNative(I)Z` (statique) | Supportée |  |
| `isInterface(I)Z` (statique) | Supportée |  |
| `isAbstract(I)Z` (statique) | Supportée |  |
| `isStrict(I)Z` (statique) | Supportée |  |

## `java.util.ArrayList$Itr`

| Membre | Statut | Note |
|---|---|---|
| `hasNext()Z` | Supportée |  |
| `next()Ljava/lang/Object;` | Supportée |  |
| `remove()V` | Supportée |  |

## `java.util.ArrayList`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(I)V` | Supportée |  |
| `<init>(Ljava/util/Collection;)V` | Partielle | copie depuis une ArrayList uniquement |
| `add(Ljava/lang/Object;)Z` | Supportée |  |
| `add(ILjava/lang/Object;)V` | Supportée |  |
| `addAll(Ljava/util/Collection;)Z` | Partielle | depuis une ArrayList uniquement |
| `get(I)Ljava/lang/Object;` | Supportée |  |
| `set(ILjava/lang/Object;)Ljava/lang/Object;` | Supportée |  |
| `remove(I)Ljava/lang/Object;` | Supportée |  |
| `remove(Ljava/lang/Object;)Z` | Supportée |  |
| `size()I` | Supportée |  |
| `isEmpty()Z` | Supportée |  |
| `clear()V` | Supportée |  |
| `contains(Ljava/lang/Object;)Z` | Supportée |  |
| `indexOf(Ljava/lang/Object;)I` | Supportée |  |
| `toString()Ljava/lang/String;` | Supportée |  |
| `iterator()Ljava/util/Iterator;` | Supportée |  |

## `java.util.Collection` (interface)

| Membre | Statut | Note |
|---|---|---|
| `add(Ljava/lang/Object;)Z` (abstraite) | Supportée |  |
| `size()I` (abstraite) | Supportée |  |
| `isEmpty()Z` (abstraite) | Supportée |  |
| `contains(Ljava/lang/Object;)Z` (abstraite) | Supportée |  |
| `remove(Ljava/lang/Object;)Z` (abstraite) | Supportée |  |
| `clear()V` (abstraite) | Supportée |  |
| `addAll(Ljava/util/Collection;)Z` (abstraite) | Supportée |  |

## `java.util.ConcurrentModificationException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.util.HashMap`

Classe : **Partielle** — recherche linéaire ; keySet/values/entrySet non implémentés

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(I)V` | Supportée |  |
| `put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;` | Supportée |  |
| `get(Ljava/lang/Object;)Ljava/lang/Object;` | Supportée |  |
| `getOrDefault(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;` | Supportée |  |
| `containsKey(Ljava/lang/Object;)Z` | Supportée |  |
| `remove(Ljava/lang/Object;)Ljava/lang/Object;` | Supportée |  |
| `size()I` | Supportée |  |
| `isEmpty()Z` | Supportée |  |
| `clear()V` | Supportée |  |
| `keySet()Ljava/util/Set;` | Non supportée | vues de collection non implémentées |
| `values()Ljava/util/Collection;` | Non supportée | vues de collection non implémentées |
| `entrySet()Ljava/util/Set;` | Non supportée | vues de collection non implémentées |

## `java.util.IllegalFormatConversionException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.util.Iterator` (interface)

| Membre | Statut | Note |
|---|---|---|
| `hasNext()Z` (abstraite) | Supportée |  |
| `next()Ljava/lang/Object;` (abstraite) | Supportée |  |
| `remove()V` (abstraite) | Supportée |  |

## `java.util.List` (interface)

| Membre | Statut | Note |
|---|---|---|
| `get(I)Ljava/lang/Object;` (abstraite) | Supportée |  |
| `set(ILjava/lang/Object;)Ljava/lang/Object;` (abstraite) | Supportée |  |
| `add(ILjava/lang/Object;)V` (abstraite) | Supportée |  |
| `remove(I)Ljava/lang/Object;` (abstraite) | Supportée |  |
| `indexOf(Ljava/lang/Object;)I` (abstraite) | Supportée |  |

## `java.util.Map` (interface)

| Membre | Statut | Note |
|---|---|---|
| `put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;` (abstraite) | Supportée |  |
| `get(Ljava/lang/Object;)Ljava/lang/Object;` (abstraite) | Supportée |  |
| `getOrDefault(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;` (abstraite) | Supportée |  |
| `containsKey(Ljava/lang/Object;)Z` (abstraite) | Supportée |  |
| `remove(Ljava/lang/Object;)Ljava/lang/Object;` (abstraite) | Supportée |  |
| `size()I` (abstraite) | Supportée |  |
| `isEmpty()Z` (abstraite) | Supportée |  |
| `clear()V` (abstraite) | Supportée |  |

## `java.util.MissingFormatArgumentException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.util.NoSuchElementException`

| Membre | Statut | Note |
|---|---|---|
| `<init>()V` | Supportée |  |
| `<init>(Ljava/lang/String;)V` | Supportée |  |
| `<init>(Ljava/lang/String;Ljava/lang/Throwable;)V` | Supportée |  |
| `<init>(Ljava/lang/Throwable;)V` | Supportée |  |

## `java.util.RandomAccess` (interface)

_Aucun membre propre (type déclaré pour la résolution des classes)._

## `java.util.UnknownFormatConversionException`

_Aucun membre propre (type déclaré pour la résolution des classes)._

