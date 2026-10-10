# Références d'API typiques d'un paquet de services système (jamais exécutées :
# la fixture sert uniquement à l'analyse statique).

.class public Lcom/anyandroid/samples/gmsprofile/Probe;
.super Ljava/lang/Object;
.source "Probe.java"

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method private static native nativeInit(J)I
.end method

.method public static uses(Landroid/content/Context;)V
    .registers 6
    const-string v0, "gmscore"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V

    const-string v0, "com.google.android.gms"
    invoke-static {v0}, Ljava/lang/Class;->forName(Ljava/lang/String;)Ljava/lang/Class;
    move-result-object v1
    const-string v0, "start"
    const/4 v2, 0x0
    invoke-virtual {v1, v0, v2}, Ljava/lang/Class;->getDeclaredMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;

    new-instance v1, Ljava/lang/Thread;
    invoke-direct {v1}, Ljava/lang/Thread;-><init>()V
    invoke-virtual {v1}, Ljava/lang/Thread;->start()V
    invoke-static {}, Ljava/util/concurrent/Executors;->newCachedThreadPool()Ljava/util/concurrent/ExecutorService;

    invoke-static {}, Landroid/os/Parcel;->obtain()Landroid/os/Parcel;
    move-result-object v1
    const/4 v2, 0x7
    invoke-virtual {v1, v2}, Landroid/os/Parcel;->writeInt(I)V
    invoke-virtual {v1}, Landroid/os/Parcel;->recycle()V

    invoke-static {p0}, Landroid/accounts/AccountManager;->get(Landroid/content/Context;)Landroid/accounts/AccountManager;
    invoke-virtual {p0}, Landroid/content/Context;->getContentResolver()Landroid/content/ContentResolver;
    const-string v0, "account"
    invoke-virtual {p0, v0}, Landroid/content/Context;->getSystemService(Ljava/lang/String;)Ljava/lang/Object;
    invoke-virtual {p0}, Landroid/content/Context;->getPackageManager()Landroid/content/pm/PackageManager;

    const-string v0, "android.app.ActivityThread"
    invoke-static {}, Landroid/app/ActivityThread;->currentApplication()Landroid/app/Application;
    const-string v0, "ro.build.fingerprint"
    invoke-static {v0}, Landroid/os/SystemProperties;->get(Ljava/lang/String;)Ljava/lang/String;

    const-wide/16 v2, 0x0
    invoke-static {v2, v3}, Lcom/anyandroid/samples/gmsprofile/Probe;->nativeInit(J)I
    return-void
.end method
