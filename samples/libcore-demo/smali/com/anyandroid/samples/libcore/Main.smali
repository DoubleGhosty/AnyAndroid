# Équivalent Java : chaque section sollicite une partie de la libcore et
# renvoie une chaîne ; un échec est affiché sans interrompre les suivantes.
#
#   public static void main(String[] args) {
#     for (int i = 0; i < NAMES.length; i++) {
#       String r;
#       try { r = run(i); } catch (Throwable t) { r = "ÉCHEC " + t; }
#       System.out.println(NAMES[i] + " : " + r);
#     }
#   }
#   collections : [banane, cerise, pomme] 2 2
#   nombres     : -42 ff 101 1.5 1.4142135623730951
#   formatage   : 7-x-3.14            (java.util.Formatter)
#   builder     : 21üé                (StringBuilder Unicode, reverse)
#   atomiques   : 6 v                 (AtomicInteger, ConcurrentHashMap)
#   exceptions  : java.lang.NullPointerException For input string: "abc"
#   propriétés  : Dalvik x86_64 UTF-8
#   threadlocal : tl
#   tri         : [1, 3, 5, 9]
#   temps       : ok
#   chaînes     : a-b-c ÉTÉ espace    (String.join, toUpperCase, strip)
#   encapsulés  : true true
#   fichiers    : Bonjour fichier [essai.txt] true false   (FileOutputStream, BufferedReader, File)
#   apex        : 30533808 true      (données ICU et framework.jar vus par le VFS)
#   nio         : [alpha, bêta] true [lignes.txt, vide.txt] 12 -1 false
#                 (java.nio.file.Files : write, readAllLines, getLastModifiedTime,
#                  newDirectoryStream, delete ; FileChannel.read jusqu'à -1)

.class public Lcom/anyandroid/samples/libcore/Main;
.super Ljava/lang/Object;
.source "Main.java"

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static main([Ljava/lang/String;)V
    .registers 8
    const/16 v5, 0xf
    new-array v6, v5, [Ljava/lang/String;
    const/4 v0, 0x0
    const-string v1, "collections"
    aput-object v1, v6, v0
    const/4 v0, 0x1
    const-string v1, "nombres"
    aput-object v1, v6, v0
    const/4 v0, 0x2
    const-string v1, "formatage"
    aput-object v1, v6, v0
    const/4 v0, 0x3
    const-string v1, "builder"
    aput-object v1, v6, v0
    const/4 v0, 0x4
    const-string v1, "atomiques"
    aput-object v1, v6, v0
    const/4 v0, 0x5
    const-string v1, "exceptions"
    aput-object v1, v6, v0
    const/4 v0, 0x6
    const-string v1, "propriétés"
    aput-object v1, v6, v0
    const/4 v0, 0x7
    const-string v1, "threadlocal"
    aput-object v1, v6, v0
    const/16 v0, 0x8
    const-string v1, "tri"
    aput-object v1, v6, v0
    const/16 v0, 0x9
    const-string v1, "temps"
    aput-object v1, v6, v0
    const/16 v0, 0xa
    const-string v1, "chaînes"
    aput-object v1, v6, v0
    const/16 v0, 0xb
    const-string v1, "encapsulés"
    aput-object v1, v6, v0
    const/16 v0, 0xc
    const-string v1, "fichiers"
    aput-object v1, v6, v0
    const/16 v0, 0xd
    const-string v1, "apex"
    aput-object v1, v6, v0
    const/16 v0, 0xe
    const-string v1, "nio"
    aput-object v1, v6, v0

    const/4 v0, 0x0
    :loop
    if-ge v0, v5, :done
    :try_start
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->run(I)Ljava/lang/String;
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
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->collections()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s1
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->numbers()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s2
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->format()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s3
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->builder()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s4
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->atomics()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s5
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->exceptions()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s6
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->props()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s7
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->threadLocal()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s8
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->sorting()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s9
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->time()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s10
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->strings()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s11
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->boxes()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s12
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->files()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s13
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->apex()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s14
    invoke-static {}, Lcom/anyandroid/samples/libcore/Main;->nio()Ljava/lang/String;
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
        :s13
        :s14
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

.method static collections()Ljava/lang/String;
    .registers 5
    new-instance v0, Ljava/util/ArrayList;
    invoke-direct {v0}, Ljava/util/ArrayList;-><init>()V
    const-string v1, "pomme"
    invoke-interface {v0, v1}, Ljava/util/List;->add(Ljava/lang/Object;)Z
    const-string v1, "banane"
    invoke-interface {v0, v1}, Ljava/util/List;->add(Ljava/lang/Object;)Z
    const-string v1, "cerise"
    invoke-interface {v0, v1}, Ljava/util/List;->add(Ljava/lang/Object;)Z
    invoke-static {v0}, Ljava/util/Collections;->sort(Ljava/util/List;)V
    new-instance v1, Ljava/util/HashMap;
    invoke-direct {v1}, Ljava/util/HashMap;-><init>()V
    const-string v2, "a"
    const/4 v3, 0x1
    invoke-static {v3}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v3
    invoke-interface {v1, v2, v3}, Ljava/util/Map;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    const-string v2, "b"
    const/4 v3, 0x2
    invoke-static {v3}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v3
    invoke-interface {v1, v2, v3}, Ljava/util/Map;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    invoke-interface {v1, v2}, Ljava/util/Map;->get(Ljava/lang/Object;)Ljava/lang/Object;
    move-result-object v2
    invoke-interface {v1}, Ljava/util/Map;->size()I
    move-result v3
    invoke-static {v3}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v3
    filled-new-array {v0, v2, v3}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static numbers()Ljava/lang/String;
    .registers 8
    const-string v0, "-42"
    invoke-static {v0}, Ljava/lang/Integer;->parseInt(Ljava/lang/String;)I
    move-result v0
    invoke-static {v0}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v0
    const-wide/16 v2, 0xff
    invoke-static {v2, v3}, Ljava/lang/Long;->toHexString(J)Ljava/lang/String;
    move-result-object v1
    const/4 v2, 0x5
    invoke-static {v2}, Ljava/lang/Integer;->toBinaryString(I)Ljava/lang/String;
    move-result-object v2
    const-wide/high16 v4, 0x3ff8000000000000L
    invoke-static {v4, v5}, Ljava/lang/Double;->toString(D)Ljava/lang/String;
    move-result-object v3
    const-wide/high16 v4, 0x4000000000000000L
    invoke-static {v4, v5}, Ljava/lang/Math;->sqrt(D)D
    move-result-wide v4
    invoke-static {v4, v5}, Ljava/lang/Double;->valueOf(D)Ljava/lang/Double;
    move-result-object v4
    filled-new-array {v0, v1, v2, v3, v4}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static format()Ljava/lang/String;
    .registers 7
    new-instance v0, Ljava/util/Formatter;
    invoke-direct {v0}, Ljava/util/Formatter;-><init>()V
    const-string v1, "%d-%s-%.2f"
    const/4 v2, 0x7
    invoke-static {v2}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v2
    const-string v3, "x"
    const-wide v4, 0x400921f9f01b866eL
    invoke-static {v4, v5}, Ljava/lang/Double;->valueOf(D)Ljava/lang/Double;
    move-result-object v4
    filled-new-array {v2, v3, v4}, [Ljava/lang/Object;
    move-result-object v2
    invoke-virtual {v0, v1, v2}, Ljava/util/Formatter;->format(Ljava/lang/String;[Ljava/lang/Object;)Ljava/util/Formatter;
    move-result-object v0
    invoke-virtual {v0}, Ljava/util/Formatter;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static builder()Ljava/lang/String;
    .registers 2
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const-string v1, "é"
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    const/16 v1, 0xfc
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(C)Ljava/lang/StringBuilder;
    const/16 v1, 0xc
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->reverse()Ljava/lang/StringBuilder;
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static atomics()Ljava/lang/String;
    .registers 5
    new-instance v0, Ljava/util/concurrent/atomic/AtomicInteger;
    invoke-direct {v0}, Ljava/util/concurrent/atomic/AtomicInteger;-><init>()V
    invoke-virtual {v0}, Ljava/util/concurrent/atomic/AtomicInteger;->incrementAndGet()I
    const/4 v1, 0x5
    invoke-virtual {v0, v1}, Ljava/util/concurrent/atomic/AtomicInteger;->addAndGet(I)I
    invoke-virtual {v0}, Ljava/util/concurrent/atomic/AtomicInteger;->get()I
    move-result v1
    invoke-static {v1}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v1
    new-instance v2, Ljava/util/concurrent/ConcurrentHashMap;
    invoke-direct {v2}, Ljava/util/concurrent/ConcurrentHashMap;-><init>()V
    const-string v3, "k"
    const-string v4, "v"
    invoke-virtual {v2, v3, v4}, Ljava/util/concurrent/ConcurrentHashMap;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
    invoke-virtual {v2, v3}, Ljava/util/concurrent/ConcurrentHashMap;->get(Ljava/lang/Object;)Ljava/lang/Object;
    move-result-object v2
    filled-new-array {v1, v2}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static exceptions()Ljava/lang/String;
    .registers 3
    :npe_start
    const/4 v0, 0x0
    invoke-virtual {v0}, Ljava/lang/Object;->hashCode()I
    :npe_end
    .catch Ljava/lang/NullPointerException; {:npe_start .. :npe_end} :npe
    const-string v0, "pas de NullPointerException"
    return-object v0
    :npe
    move-exception v0
    invoke-virtual {v0}, Ljava/lang/Object;->getClass()Ljava/lang/Class;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/Class;->getName()Ljava/lang/String;
    move-result-object v0
    :nfe_start
    const-string v1, "abc"
    invoke-static {v1}, Ljava/lang/Integer;->parseInt(Ljava/lang/String;)I
    :nfe_end
    .catch Ljava/lang/NumberFormatException; {:nfe_start .. :nfe_end} :nfe
    const-string v1, "pas de NumberFormatException"
    return-object v1
    :nfe
    move-exception v1
    invoke-virtual {v1}, Ljava/lang/Throwable;->getMessage()Ljava/lang/String;
    move-result-object v1
    filled-new-array {v0, v1}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static props()Ljava/lang/String;
    .registers 3
    const-string v0, "java.vm.name"
    invoke-static {v0}, Ljava/lang/System;->getProperty(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    const-string v1, "os.arch"
    invoke-static {v1}, Ljava/lang/System;->getProperty(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v1
    const-string v2, "file.encoding"
    invoke-static {v2}, Ljava/lang/System;->getProperty(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v2
    filled-new-array {v0, v1, v2}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static threadLocal()Ljava/lang/String;
    .registers 2
    new-instance v0, Ljava/lang/ThreadLocal;
    invoke-direct {v0}, Ljava/lang/ThreadLocal;-><init>()V
    const-string v1, "tl"
    invoke-virtual {v0, v1}, Ljava/lang/ThreadLocal;->set(Ljava/lang/Object;)V
    invoke-virtual {v0}, Ljava/lang/ThreadLocal;->get()Ljava/lang/Object;
    move-result-object v0
    check-cast v0, Ljava/lang/String;
    return-object v0
.end method

.method static sorting()Ljava/lang/String;
    .registers 2
    const/4 v0, 0x4
    new-array v0, v0, [I
    fill-array-data v0, :data
    invoke-static {v0}, Ljava/util/Arrays;->sort([I)V
    invoke-static {v0}, Ljava/util/Arrays;->toString([I)Ljava/lang/String;
    move-result-object v0
    return-object v0
    :data
    .array-data 4
        0x5
        0x3
        0x9
        0x1
    .end array-data
.end method

.method static time()Ljava/lang/String;
    .registers 4
    invoke-static {}, Ljava/lang/System;->currentTimeMillis()J
    move-result-wide v0
    const-wide v2, 0x18bcfe56800L
    cmp-long v0, v0, v2
    if-lez v0, :ko
    const-string v0, "ok"
    return-object v0
    :ko
    const-string v0, "horloge incohérente"
    return-object v0
.end method

.method static strings()Ljava/lang/String;
    .registers 4
    const-string v0, "-"
    const-string v1, "a"
    const-string v2, "b"
    const-string v3, "c"
    filled-new-array {v1, v2, v3}, [Ljava/lang/CharSequence;
    move-result-object v1
    invoke-static {v0, v1}, Ljava/lang/String;->join(Ljava/lang/CharSequence;[Ljava/lang/CharSequence;)Ljava/lang/String;
    move-result-object v0
    const-string v1, "Été"
    invoke-virtual {v1}, Ljava/lang/String;->toUpperCase()Ljava/lang/String;
    move-result-object v1
    const-string v2, "  espace  "
    invoke-virtual {v2}, Ljava/lang/String;->strip()Ljava/lang/String;
    move-result-object v2
    filled-new-array {v0, v1, v2}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static boxes()Ljava/lang/String;
    .registers 5
    const/16 v0, 0x7f
    invoke-static {v0}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v1
    invoke-static {v0}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v2
    const/4 v3, 0x0
    if-ne v1, v2, :differ
    const/4 v3, 0x1
    :differ
    invoke-static {v3}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v3
    const/16 v4, 0x37
    invoke-static {v4}, Ljava/lang/Character;->isDigit(C)Z
    move-result v4
    invoke-static {v4}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v4
    filled-new-array {v3, v4}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static files()Ljava/lang/String;
    .registers 7
    new-instance v0, Ljava/io/File;
    const-string v1, "/data/local/tmp/essai.txt"
    invoke-direct {v0, v1}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    new-instance v1, Ljava/io/FileOutputStream;
    invoke-direct {v1, v0}, Ljava/io/FileOutputStream;-><init>(Ljava/io/File;)V
    const-string v2, "Bonjour fichier"
    invoke-virtual {v2}, Ljava/lang/String;->getBytes()[B
    move-result-object v2
    invoke-virtual {v1, v2}, Ljava/io/FileOutputStream;->write([B)V
    invoke-virtual {v1}, Ljava/io/FileOutputStream;->close()V
    new-instance v1, Ljava/io/BufferedReader;
    new-instance v2, Ljava/io/FileReader;
    invoke-direct {v2, v0}, Ljava/io/FileReader;-><init>(Ljava/io/File;)V
    invoke-direct {v1, v2}, Ljava/io/BufferedReader;-><init>(Ljava/io/Reader;)V
    invoke-virtual {v1}, Ljava/io/BufferedReader;->readLine()Ljava/lang/String;
    move-result-object v2
    invoke-virtual {v1}, Ljava/io/BufferedReader;->close()V
    new-instance v3, Ljava/io/File;
    const-string v4, "/data/local/tmp"
    invoke-direct {v3, v4}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-virtual {v3}, Ljava/io/File;->list()[Ljava/lang/String;
    move-result-object v3
    invoke-static {v3}, Ljava/util/Arrays;->toString([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v3
    invoke-virtual {v0}, Ljava/io/File;->delete()Z
    move-result v4
    invoke-static {v4}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v4
    invoke-virtual {v0}, Ljava/io/File;->exists()Z
    move-result v5
    invoke-static {v5}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v5
    filled-new-array {v2, v3, v4, v5}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

.method static apex()Ljava/lang/String;
    .registers 4
    new-instance v0, Ljava/io/File;
    const-string v1, "/apex/com.android.i18n/etc/icu/icudt78l.dat"
    invoke-direct {v0, v1}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-virtual {v0}, Ljava/io/File;->length()J
    move-result-wide v0
    invoke-static {v0, v1}, Ljava/lang/Long;->valueOf(J)Ljava/lang/Long;
    move-result-object v0
    new-instance v1, Ljava/io/File;
    const-string v2, "/system/framework/framework.jar"
    invoke-direct {v1, v2}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-virtual {v1}, Ljava/io/File;->isFile()Z
    move-result v1
    invoke-static {v1}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v1
    filled-new-array {v0, v1}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# java.nio.file on the sandboxed /data/local/tmp (sun.nio.fs and sun.nio.ch natives):
#
#   Path dir = Paths.get("/data/local/tmp/nio"), f = dir.resolve("lignes.txt"), e = dir.resolve("vide.txt");
#   Files.deleteIfExists(f); Files.deleteIfExists(e); Files.deleteIfExists(dir);  // leftovers
#   Files.createDirectory(dir);
#   Files.write(f, "alpha\nbêta\n".getBytes()); Files.write(e, new byte[0]);
#   List<String> lines = Files.readAllLines(f);
#   boolean recent = Files.getLastModifiedTime(f).toMillis() >= 978307200000L;  // after 2000
#   List<String> names = new ArrayList<>();
#   try (DirectoryStream<Path> ds = Files.newDirectoryStream(dir)) { for (Path p : ds) names.add(p.getFileName().toString()); }
#   Collections.sort(names);
#   FileChannel ch = FileChannel.open(f); ByteBuffer bb = ByteBuffer.allocate(5);
#   int total = 0, n; while (true) { bb.clear(); n = ch.read(bb); if (n < 0) break; total += n; } ch.close();
#   Files.delete(f); Files.delete(e); Files.delete(dir);
#   return j(lines, recent, names, total, n, Files.exists(dir));
.method static nio()Ljava/lang/String;
    .registers 12
    const-string v3, "/data/local/tmp/nio"
    const/4 v4, 0x0
    new-array v5, v4, [Ljava/lang/String;
    invoke-static {v3, v5}, Ljava/nio/file/Paths;->get(Ljava/lang/String;[Ljava/lang/String;)Ljava/nio/file/Path;
    move-result-object v0
    const-string v3, "lignes.txt"
    invoke-interface {v0, v3}, Ljava/nio/file/Path;->resolve(Ljava/lang/String;)Ljava/nio/file/Path;
    move-result-object v1
    const-string v3, "vide.txt"
    invoke-interface {v0, v3}, Ljava/nio/file/Path;->resolve(Ljava/lang/String;)Ljava/nio/file/Path;
    move-result-object v2
    invoke-static {v1}, Ljava/nio/file/Files;->deleteIfExists(Ljava/nio/file/Path;)Z
    invoke-static {v2}, Ljava/nio/file/Files;->deleteIfExists(Ljava/nio/file/Path;)Z
    invoke-static {v0}, Ljava/nio/file/Files;->deleteIfExists(Ljava/nio/file/Path;)Z
    new-array v3, v4, [Ljava/nio/file/attribute/FileAttribute;
    invoke-static {v0, v3}, Ljava/nio/file/Files;->createDirectory(Ljava/nio/file/Path;[Ljava/nio/file/attribute/FileAttribute;)Ljava/nio/file/Path;

    new-array v5, v4, [Ljava/nio/file/OpenOption;
    const-string v3, "alpha\nbêta\n"
    invoke-virtual {v3}, Ljava/lang/String;->getBytes()[B
    move-result-object v3
    invoke-static {v1, v3, v5}, Ljava/nio/file/Files;->write(Ljava/nio/file/Path;[B[Ljava/nio/file/OpenOption;)Ljava/nio/file/Path;
    new-array v3, v4, [B
    invoke-static {v2, v3, v5}, Ljava/nio/file/Files;->write(Ljava/nio/file/Path;[B[Ljava/nio/file/OpenOption;)Ljava/nio/file/Path;
    invoke-static {v1}, Ljava/nio/file/Files;->readAllLines(Ljava/nio/file/Path;)Ljava/util/List;
    move-result-object v6

    new-array v3, v4, [Ljava/nio/file/LinkOption;
    invoke-static {v1, v3}, Ljava/nio/file/Files;->getLastModifiedTime(Ljava/nio/file/Path;[Ljava/nio/file/LinkOption;)Ljava/nio/file/attribute/FileTime;
    move-result-object v3
    invoke-virtual {v3}, Ljava/nio/file/attribute/FileTime;->toMillis()J
    move-result-wide v8
    const-wide v10, 0xe3c7a73400L
    cmp-long v3, v8, v10
    const/4 v7, 0x0
    if-ltz v3, :old
    const/4 v7, 0x1
    :old
    invoke-static {v7}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v7

    invoke-static {v0}, Ljava/nio/file/Files;->newDirectoryStream(Ljava/nio/file/Path;)Ljava/nio/file/DirectoryStream;
    move-result-object v3
    new-instance v8, Ljava/util/ArrayList;
    invoke-direct {v8}, Ljava/util/ArrayList;-><init>()V
    invoke-interface {v3}, Ljava/nio/file/DirectoryStream;->iterator()Ljava/util/Iterator;
    move-result-object v9
    :list_loop
    invoke-interface {v9}, Ljava/util/Iterator;->hasNext()Z
    move-result v10
    if-eqz v10, :list_done
    invoke-interface {v9}, Ljava/util/Iterator;->next()Ljava/lang/Object;
    move-result-object v10
    check-cast v10, Ljava/nio/file/Path;
    invoke-interface {v10}, Ljava/nio/file/Path;->getFileName()Ljava/nio/file/Path;
    move-result-object v10
    invoke-virtual {v10}, Ljava/lang/Object;->toString()Ljava/lang/String;
    move-result-object v10
    invoke-interface {v8, v10}, Ljava/util/List;->add(Ljava/lang/Object;)Z
    goto :list_loop
    :list_done
    invoke-interface {v3}, Ljava/io/Closeable;->close()V
    invoke-static {v8}, Ljava/util/Collections;->sort(Ljava/util/List;)V

    new-array v3, v4, [Ljava/nio/file/OpenOption;
    invoke-static {v1, v3}, Ljava/nio/channels/FileChannel;->open(Ljava/nio/file/Path;[Ljava/nio/file/OpenOption;)Ljava/nio/channels/FileChannel;
    move-result-object v3
    const/4 v9, 0x5
    invoke-static {v9}, Ljava/nio/ByteBuffer;->allocate(I)Ljava/nio/ByteBuffer;
    move-result-object v9
    const/4 v10, 0x0
    :read_loop
    invoke-virtual {v9}, Ljava/nio/Buffer;->clear()Ljava/nio/Buffer;
    invoke-virtual {v3, v9}, Ljava/nio/channels/FileChannel;->read(Ljava/nio/ByteBuffer;)I
    move-result v11
    if-ltz v11, :read_done
    add-int/2addr v10, v11
    goto :read_loop
    :read_done
    invoke-virtual {v3}, Ljava/nio/channels/spi/AbstractInterruptibleChannel;->close()V

    invoke-static {v1}, Ljava/nio/file/Files;->delete(Ljava/nio/file/Path;)V
    invoke-static {v2}, Ljava/nio/file/Files;->delete(Ljava/nio/file/Path;)V
    invoke-static {v0}, Ljava/nio/file/Files;->delete(Ljava/nio/file/Path;)V
    new-array v3, v4, [Ljava/nio/file/LinkOption;
    invoke-static {v0, v3}, Ljava/nio/file/Files;->exists(Ljava/nio/file/Path;[Ljava/nio/file/LinkOption;)Z
    move-result v3
    invoke-static {v3}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v3

    invoke-static {v10}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v10
    invoke-static {v11}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v11
    const/4 v4, 0x6
    new-array v4, v4, [Ljava/lang/Object;
    const/4 v5, 0x0
    aput-object v6, v4, v5
    const/4 v5, 0x1
    aput-object v7, v4, v5
    const/4 v5, 0x2
    aput-object v8, v4, v5
    const/4 v5, 0x3
    aput-object v10, v4, v5
    const/4 v5, 0x4
    aput-object v11, v4, v5
    const/4 v5, 0x5
    aput-object v3, v4, v5
    invoke-static {v4}, Lcom/anyandroid/samples/libcore/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
