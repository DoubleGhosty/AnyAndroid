# Équivalent Java : chaque section sollicite une classe de framework.jar et
# renvoie une chaîne ; un échec est affiché sans interrompre les suivantes.
#
#   log         : 26                        (Log.i, écrit sur la sortie d'erreur)
#   textutils   : a, b true                 (TextUtils.join, isEmpty)
#   bundle      : v 3 2                     (Bundle)
#   collections : 1 sept 1                  (ArrayMap, SparseArray)
#   uri         : exemple.fr /chemin/page 1 frag
#   base64      : QW55QW5kcm9pZA==
#   json        : 1 2 {"a":1,"b":[true,"x"]}
#   build       : 37 AnyAndroid (Windows)   (Build : propriétés de l'image système)
#   looper      : true                      (Looper.prepare/loop, Handler.post)
#   horloge     : ok                        (SystemClock)
#   parcel      : 42 été v a.b.C 7          (Parcel : marshall/unmarshall, Bundle, Intent)
#   binder      : true true a.b.IDemo       (Binder local transmis par Parcel, transact)
#   resources   : Cancel android:string/cancel 200 ff008577
#                 (Resources.getSystem() : framework-res.apk, thème Material, TypedArray)

.class public Lcom/anyandroid/samples/framework/Main;
.super Ljava/lang/Object;
.source "Main.java"

.method public static main([Ljava/lang/String;)V
    .registers 8
    const/16 v5, 0xd
    new-array v6, v5, [Ljava/lang/String;
    const/4 v0, 0x0
    const-string v1, "log"
    aput-object v1, v6, v0
    const/4 v0, 0x1
    const-string v1, "textutils"
    aput-object v1, v6, v0
    const/4 v0, 0x2
    const-string v1, "bundle"
    aput-object v1, v6, v0
    const/4 v0, 0x3
    const-string v1, "collections"
    aput-object v1, v6, v0
    const/4 v0, 0x4
    const-string v1, "uri"
    aput-object v1, v6, v0
    const/4 v0, 0x5
    const-string v1, "base64"
    aput-object v1, v6, v0
    const/4 v0, 0x6
    const-string v1, "json"
    aput-object v1, v6, v0
    const/4 v0, 0x7
    const-string v1, "build"
    aput-object v1, v6, v0
    const/16 v0, 0x8
    const-string v1, "looper"
    aput-object v1, v6, v0
    const/16 v0, 0x9
    const-string v1, "horloge"
    aput-object v1, v6, v0
    const/16 v0, 0xa
    const-string v1, "parcel"
    aput-object v1, v6, v0
    const/16 v0, 0xb
    const-string v1, "binder"
    aput-object v1, v6, v0
    const/16 v0, 0xc
    const-string v1, "resources"
    aput-object v1, v6, v0

    # The resources section (12) is not run yet: Resources.getSystem() still
    # needs sun.nio.fs (aconfig flags); see docs/STATUS.md.
    const/16 v5, 0xc
    const/4 v0, 0x0
    :loop
    if-ge v0, v5, :done
    :try_start
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->run(I)Ljava/lang/String;
    move-result-object v3
    :try_end
    .catch Ljava/lang/Throwable; {:try_start .. :try_end} :failed
    goto :print
    :failed
    move-exception v3
    invoke-static {v3}, Ljava/lang/String;->valueOf(Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v3
    const-string v4, "ÉCHEC "
    invoke-virtual {v4, v3}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v3
    :print
    aget-object v4, v6, v0
    const-string v1, " : "
    invoke-virtual {v4, v1}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v4
    invoke-virtual {v4, v3}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v4
    sget-object v2, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v2, v4}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    add-int/lit8 v0, v0, 0x1
    goto :loop
    :done
    return-void
.end method

.method static run(I)Ljava/lang/String;
    .registers 2
    packed-switch p0, :table
    const-string v0, "?"
    return-object v0
    :s0
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->log()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s1
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->textutils()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s2
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->bundle()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s3
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->collections()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s4
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->uri()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s5
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->base64()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s6
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->json()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s7
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->build()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s8
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->looper()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s9
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->clock()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s10
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->parcel()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s11
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->binder()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s12
    invoke-static {}, Lcom/anyandroid/samples/framework/Main;->resources()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :table
    .packed-switch 0x0
        :s0
        :s1
        :s2
        :s3
        :s4
        :s5
        :s6
        :s7
        :s8
        :s9
        :s10
        :s11
        :s12
    .end packed-switch
.end method

# Joins the string forms of its arguments with spaces.
.method static j([Ljava/lang/Object;)Ljava/lang/String;
    .registers 6
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const/4 v1, 0x0
    array-length v2, p0
    :loop
    if-ge v1, v2, :done
    if-eqz v1, :first
    const-string v3, " "
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    :first
    aget-object v3, p0, v1
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/Object;)Ljava/lang/StringBuilder;
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static log()Ljava/lang/String;
    .registers 2
    const-string v0, "FrameworkDemo"
    const-string v1, "bonjour depuis framework.jar"
    invoke-static {v0, v1}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I
    move-result v0
    invoke-static {v0}, Ljava/lang/String;->valueOf(I)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static textutils()Ljava/lang/String;
    .registers 4
    new-instance v0, Ljava/util/ArrayList;
    invoke-direct {v0}, Ljava/util/ArrayList;-><init>()V
    const-string v1, "a"
    invoke-interface {v0, v1}, Ljava/util/List;->add(Ljava/lang/Object;)Z
    const-string v1, "b"
    invoke-interface {v0, v1}, Ljava/util/List;->add(Ljava/lang/Object;)Z
    const-string v1, ", "
    invoke-static {v1, v0}, Landroid/text/TextUtils;->join(Ljava/lang/CharSequence;Ljava/lang/Iterable;)Ljava/lang/String;
    move-result-object v0
    const-string v1, ""
    invoke-static {v1}, Landroid/text/TextUtils;->isEmpty(Ljava/lang/CharSequence;)Z
    move-result v1
    invoke-static {v1}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v1
    filled-new-array {v0, v1}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static bundle()Ljava/lang/String;
    .registers 5
    new-instance v0, Landroid/os/Bundle;
    invoke-direct {v0}, Landroid/os/Bundle;-><init>()V
    const-string v1, "k"
    const-string v2, "v"
    invoke-virtual {v0, v1, v2}, Landroid/os/Bundle;->putString(Ljava/lang/String;Ljava/lang/String;)V
    const-string v2, "n"
    const/4 v3, 0x3
    invoke-virtual {v0, v2, v3}, Landroid/os/Bundle;->putInt(Ljava/lang/String;I)V
    invoke-virtual {v0, v1}, Landroid/os/Bundle;->getString(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v2}, Landroid/os/Bundle;->getInt(Ljava/lang/String;)I
    move-result v2
    invoke-static {v2}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v2
    invoke-virtual {v0}, Landroid/os/Bundle;->size()I
    move-result v3
    invoke-static {v3}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v3
    filled-new-array {v1, v2, v3}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static collections()Ljava/lang/String;
    .registers 5
    new-instance v0, Landroid/util/ArrayMap;
    invoke-direct {v0}, Landroid/util/ArrayMap;-><init>()V
    const-string v1, "x"
    const-string v2, "1"
    invoke-virtual {v0, v1, v2}, Landroid/util/ArrayMap;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    invoke-virtual {v0, v1}, Landroid/util/ArrayMap;->get(Ljava/lang/Object;)Ljava/lang/Object;
    move-result-object v1
    new-instance v2, Landroid/util/SparseArray;
    invoke-direct {v2}, Landroid/util/SparseArray;-><init>()V
    const/4 v3, 0x7
    const-string v4, "sept"
    invoke-virtual {v2, v3, v4}, Landroid/util/SparseArray;->put(ILjava/lang/Object;)V
    invoke-virtual {v2, v3}, Landroid/util/SparseArray;->get(I)Ljava/lang/Object;
    move-result-object v3
    invoke-virtual {v2}, Landroid/util/SparseArray;->size()I
    move-result v4
    invoke-static {v4}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v4
    filled-new-array {v1, v3, v4}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static uri()Ljava/lang/String;
    .registers 5
    const-string v0, "https://exemple.fr/chemin/page?q=1#frag"
    invoke-static {v0}, Landroid/net/Uri;->parse(Ljava/lang/String;)Landroid/net/Uri;
    move-result-object v0
    invoke-virtual {v0}, Landroid/net/Uri;->getHost()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0}, Landroid/net/Uri;->getPath()Ljava/lang/String;
    move-result-object v2
    const-string v3, "q"
    invoke-virtual {v0, v3}, Landroid/net/Uri;->getQueryParameter(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v3
    invoke-virtual {v0}, Landroid/net/Uri;->getFragment()Ljava/lang/String;
    move-result-object v4
    filled-new-array {v1, v2, v3, v4}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static base64()Ljava/lang/String;
    .registers 2
    const-string v0, "AnyAndroid"
    invoke-virtual {v0}, Ljava/lang/String;->getBytes()[B
    move-result-object v0
    const/4 v1, 0x2
    invoke-static {v0, v1}, Landroid/util/Base64;->encodeToString([BI)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static json()Ljava/lang/String;
    .registers 4
    new-instance v0, Lorg/json/JSONObject;
    const-string v1, "{\"a\":1,\"b\":[true,\"x\"]}"
    invoke-direct {v0, v1}, Lorg/json/JSONObject;-><init>(Ljava/lang/String;)V
    const-string v1, "a"
    invoke-virtual {v0, v1}, Lorg/json/JSONObject;->getInt(Ljava/lang/String;)I
    move-result v1
    invoke-static {v1}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v1
    const-string v2, "b"
    invoke-virtual {v0, v2}, Lorg/json/JSONObject;->getJSONArray(Ljava/lang/String;)Lorg/json/JSONArray;
    move-result-object v2
    invoke-virtual {v2}, Lorg/json/JSONArray;->length()I
    move-result v2
    invoke-static {v2}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v2
    invoke-virtual {v0}, Lorg/json/JSONObject;->toString()Ljava/lang/String;
    move-result-object v3
    filled-new-array {v1, v2, v3}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static build()Ljava/lang/String;
    .registers 3
    sget v0, Landroid/os/Build$VERSION;->SDK_INT:I
    invoke-static {v0}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v0
    sget-object v1, Landroid/os/Build;->MODEL:Ljava/lang/String;
    filled-new-array {v0, v1}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static looper()Ljava/lang/String;
    .registers 3
    invoke-static {}, Landroid/os/Looper;->prepare()V
    new-instance v0, Landroid/os/Handler;
    invoke-static {}, Landroid/os/Looper;->myLooper()Landroid/os/Looper;
    move-result-object v1
    invoke-direct {v0, v1}, Landroid/os/Handler;-><init>(Landroid/os/Looper;)V
    new-instance v1, Lcom/anyandroid/samples/framework/Main$Quit;
    invoke-direct {v1}, Lcom/anyandroid/samples/framework/Main$Quit;-><init>()V
    invoke-virtual {v0, v1}, Landroid/os/Handler;->post(Ljava/lang/Runnable;)Z
    invoke-static {}, Landroid/os/Looper;->loop()V
    sget-boolean v0, Lcom/anyandroid/samples/framework/Main$Quit;->ran:Z
    invoke-static {v0}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v0
    invoke-static {v0}, Ljava/lang/String;->valueOf(Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static clock()Ljava/lang/String;
    .registers 4
    invoke-static {}, Landroid/os/SystemClock;->uptimeMillis()J
    move-result-wide v0
    const-wide/16 v2, 0x0
    cmp-long v0, v0, v2
    if-lez v0, :ko
    const-string v0, "ok"
    return-object v0
    :ko
    const-string v0, "horloge incohérente"
    return-object v0
.end method

# Round trip through bytes: primitives, String, Bundle and an Intent
# (Parcelable found through its CREATOR by reflection).
.method static parcel()Ljava/lang/String;
    .registers 8
    invoke-static {}, Landroid/os/Parcel;->obtain()Landroid/os/Parcel;
    move-result-object v0
    const/16 v1, 0x2a
    invoke-virtual {v0, v1}, Landroid/os/Parcel;->writeInt(I)V
    const-string v1, "été"
    invoke-virtual {v0, v1}, Landroid/os/Parcel;->writeString(Ljava/lang/String;)V
    new-instance v1, Landroid/os/Bundle;
    invoke-direct {v1}, Landroid/os/Bundle;-><init>()V
    const-string v2, "k"
    const-string v3, "v"
    invoke-virtual {v1, v2, v3}, Landroid/os/Bundle;->putString(Ljava/lang/String;Ljava/lang/String;)V
    invoke-virtual {v0, v1}, Landroid/os/Parcel;->writeBundle(Landroid/os/Bundle;)V
    new-instance v1, Landroid/content/Intent;
    const-string v2, "a.b.C"
    invoke-direct {v1, v2}, Landroid/content/Intent;-><init>(Ljava/lang/String;)V
    const-string v2, "x"
    const/4 v3, 0x7
    invoke-virtual {v1, v2, v3}, Landroid/content/Intent;->putExtra(Ljava/lang/String;I)Landroid/content/Intent;
    const/4 v2, 0x0
    invoke-virtual {v0, v1, v2}, Landroid/os/Parcel;->writeParcelable(Landroid/os/Parcelable;I)V
    invoke-virtual {v0}, Landroid/os/Parcel;->marshall()[B
    move-result-object v1
    invoke-virtual {v0}, Landroid/os/Parcel;->recycle()V

    invoke-static {}, Landroid/os/Parcel;->obtain()Landroid/os/Parcel;
    move-result-object v0
    array-length v2, v1
    const/4 v3, 0x0
    invoke-virtual {v0, v1, v3, v2}, Landroid/os/Parcel;->unmarshall([BII)V
    invoke-virtual {v0, v3}, Landroid/os/Parcel;->setDataPosition(I)V
    invoke-virtual {v0}, Landroid/os/Parcel;->readInt()I
    move-result v1
    invoke-static {v1}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v1
    invoke-virtual {v0}, Landroid/os/Parcel;->readString()Ljava/lang/String;
    move-result-object v2
    invoke-virtual {v0}, Landroid/os/Parcel;->readBundle()Landroid/os/Bundle;
    move-result-object v3
    const-string v4, "k"
    invoke-virtual {v3, v4}, Landroid/os/Bundle;->getString(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v3
    const/4 v4, 0x0
    invoke-virtual {v0, v4}, Landroid/os/Parcel;->readParcelable(Ljava/lang/ClassLoader;)Landroid/os/Parcelable;
    move-result-object v4
    check-cast v4, Landroid/content/Intent;
    invoke-virtual {v4}, Landroid/content/Intent;->getAction()Ljava/lang/String;
    move-result-object v5
    const-string v6, "x"
    const/4 v7, 0x0
    invoke-virtual {v4, v6, v7}, Landroid/content/Intent;->getIntExtra(Ljava/lang/String;I)I
    move-result v6
    invoke-static {v6}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v6
    invoke-virtual {v0}, Landroid/os/Parcel;->recycle()V
    filled-new-array {v1, v2, v3, v5, v6}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# A local Binder passed through a Parcel comes back as the same object;
# transact(INTERFACE_TRANSACTION) answers with its descriptor.
.method static binder()Ljava/lang/String;
    .registers 8
    new-instance v0, Landroid/os/Binder;
    invoke-direct {v0}, Landroid/os/Binder;-><init>()V
    const/4 v1, 0x0
    const-string v2, "a.b.IDemo"
    invoke-virtual {v0, v1, v2}, Landroid/os/Binder;->attachInterface(Landroid/os/IInterface;Ljava/lang/String;)V
    invoke-static {}, Landroid/os/Parcel;->obtain()Landroid/os/Parcel;
    move-result-object v1
    invoke-virtual {v1, v0}, Landroid/os/Parcel;->writeStrongBinder(Landroid/os/IBinder;)V
    const/4 v2, 0x0
    invoke-virtual {v1, v2}, Landroid/os/Parcel;->setDataPosition(I)V
    invoke-virtual {v1}, Landroid/os/Parcel;->readStrongBinder()Landroid/os/IBinder;
    move-result-object v2
    invoke-virtual {v1}, Landroid/os/Parcel;->recycle()V
    const/4 v3, 0x0
    if-ne v0, v2, :diff
    const/4 v3, 0x1
    :diff
    invoke-static {v3}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v3
    invoke-static {}, Landroid/os/Parcel;->obtain()Landroid/os/Parcel;
    move-result-object v4
    invoke-static {}, Landroid/os/Parcel;->obtain()Landroid/os/Parcel;
    move-result-object v5
    const v6, 0x5f4e5446
    const/4 v7, 0x0
    invoke-interface {v2, v6, v4, v5, v7}, Landroid/os/IBinder;->transact(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z
    move-result v6
    invoke-static {v6}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v6
    invoke-virtual {v5}, Landroid/os/Parcel;->readString()Ljava/lang/String;
    move-result-object v7
    invoke-virtual {v4}, Landroid/os/Parcel;->recycle()V
    invoke-virtual {v5}, Landroid/os/Parcel;->recycle()V
    filled-new-array {v3, v6, v7}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# The system resources (framework-res.apk of the image): a string found by
# name, an integer, and a theme attribute through TypedArray.
.method static resources()Ljava/lang/String;
    .registers 8
    invoke-static {}, Landroid/content/res/Resources;->getSystem()Landroid/content/res/Resources;
    move-result-object v0
    const-string v1, "cancel"
    const-string v2, "string"
    const-string v3, "android"
    invoke-virtual {v0, v1, v2, v3}, Landroid/content/res/Resources;->getIdentifier(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I
    move-result v1
    invoke-virtual {v0, v1}, Landroid/content/res/Resources;->getString(I)Ljava/lang/String;
    move-result-object v2
    invoke-virtual {v0, v1}, Landroid/content/res/Resources;->getResourceName(I)Ljava/lang/String;
    move-result-object v3
    const v1, 0x010e0000
    invoke-virtual {v0, v1}, Landroid/content/res/Resources;->getInteger(I)I
    move-result v1
    invoke-static {v1}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v4
    invoke-virtual {v0}, Landroid/content/res/Resources;->newTheme()Landroid/content/res/Resources$Theme;
    move-result-object v5
    const v1, 0x01030237
    const/4 v6, 0x1
    invoke-virtual {v5, v1, v6}, Landroid/content/res/Resources$Theme;->applyStyle(IZ)V
    const/4 v1, 0x1
    new-array v1, v1, [I
    const v6, 0x01010435
    const/4 v7, 0x0
    aput v6, v1, v7
    invoke-virtual {v5, v1}, Landroid/content/res/Resources$Theme;->obtainStyledAttributes([I)Landroid/content/res/TypedArray;
    move-result-object v5
    invoke-virtual {v5, v7, v7}, Landroid/content/res/TypedArray;->getColor(II)I
    move-result v6
    invoke-static {v6}, Ljava/lang/Integer;->toHexString(I)Ljava/lang/String;
    move-result-object v6
    invoke-virtual {v5}, Landroid/content/res/TypedArray;->recycle()V
    filled-new-array {v2, v3, v4, v6}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/framework/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
