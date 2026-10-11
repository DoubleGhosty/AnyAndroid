# Équivalent Java : java.util.zip sur la vraie libcore (natives Inflater,
# Deflater, CRC32 et Adler32 sur miniz). Chaque section renvoie une chaîne ;
# un échec est affiché sans interrompre les suivantes.
#
#   static final String TEXT = "Bonjour zlib ! Bonjour zlib ! Bonjour zlib ! L'été est là.";
#
#   public static void main(String[] args) {
#     for (int i = 0; i < NAMES.length; i++) {
#       String r;
#       try { r = run(i); } catch (Throwable t) { r = "ÉCHEC " + t; }
#       System.out.println(NAMES[i] + " : " + r);
#     }
#   }
#   deflate : 0 0 true true                  (Deflater SYNC_FLUSH répété, Inflater)
#   gzip    : 31 139 true 61                 (GZIPOutputStream, GZIPInputStream)
#   sommes  : cbf43926 11e60398 cbf43926     (CRC32, Adler32, tampon direct)

.class public Lcom/anyandroid/samples/zip/Main;
.super Ljava/lang/Object;
.source "Main.java"

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static main([Ljava/lang/String;)V
    .registers 8
    const/4 v5, 0x3
    new-array v6, v5, [Ljava/lang/String;
    const/4 v0, 0x0
    const-string v1, "deflate"
    aput-object v1, v6, v0
    const/4 v0, 0x1
    const-string v1, "gzip"
    aput-object v1, v6, v0
    const/4 v0, 0x2
    const-string v1, "sommes"
    aput-object v1, v6, v0

    const/4 v0, 0x0
    :loop
    if-ge v0, v5, :done
    :try_start
    invoke-static {v0}, Lcom/anyandroid/samples/zip/Main;->run(I)Ljava/lang/String;
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
    invoke-static {}, Lcom/anyandroid/samples/zip/Main;->deflate()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s1
    invoke-static {}, Lcom/anyandroid/samples/zip/Main;->gzip()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :s2
    invoke-static {}, Lcom/anyandroid/samples/zip/Main;->sums()Ljava/lang/String;
    move-result-object v0
    return-object v0
    :table
    .packed-switch 0x0
        :s0
        :s1
        :s2
    .end packed-switch
.end method

.method static text()Ljava/lang/String;
    .registers 1
    const-string v0, "Bonjour zlib ! Bonjour zlib ! Bonjour zlib ! L'été est là."
    return-object v0
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

# A flush with nothing new to write produces nothing (as zlib): okio's
# DeflaterSink.flush() loops while deflate() returns more than 0 bytes.
#
#   Deflater d = new Deflater();
#   byte[] out = new byte[512];
#   d.setInput(TEXT.getBytes());
#   int n = d.deflate(out, 0, 512, Deflater.SYNC_FLUSH);          // all of the input, flushed
#   int again = d.deflate(out, n, 512 - n, Deflater.SYNC_FLUSH);  // nothing new: 0
#   int loops = 0;
#   while (loops < 100 && d.deflate(out, n, 512 - n, Deflater.SYNC_FLUSH) > 0) loops++;
#   d.finish();
#   n += d.deflate(out, n, 512 - n);
#   boolean done = d.finished();
#   d.end();
#   Inflater inf = new Inflater();
#   inf.setInput(out, 0, n);
#   byte[] back = new byte[512];
#   int m = inf.inflate(back);
#   boolean same = new String(back, 0, m).equals(TEXT) & inf.finished();
#   inf.end();
#   return j(again, loops, done, same);
.method static deflate()Ljava/lang/String;
    .registers 13
    invoke-static {}, Lcom/anyandroid/samples/zip/Main;->text()Ljava/lang/String;
    move-result-object v11
    new-instance v0, Ljava/util/zip/Deflater;
    invoke-direct {v0}, Ljava/util/zip/Deflater;-><init>()V
    invoke-virtual {v11}, Ljava/lang/String;->getBytes()[B
    move-result-object v1
    const/16 v9, 0x200
    new-array v2, v9, [B
    invoke-virtual {v0, v1}, Ljava/util/zip/Deflater;->setInput([B)V
    const/4 v10, 0x2
    const/4 v3, 0x0
    invoke-virtual {v0, v2, v3, v9, v10}, Ljava/util/zip/Deflater;->deflate([BIII)I
    move-result v3
    sub-int v6, v9, v3
    invoke-virtual {v0, v2, v3, v6, v10}, Ljava/util/zip/Deflater;->deflate([BIII)I
    move-result v4
    const/4 v5, 0x0
    :flush_loop
    const/16 v7, 0x64
    if-ge v5, v7, :flush_done
    invoke-virtual {v0, v2, v3, v6, v10}, Ljava/util/zip/Deflater;->deflate([BIII)I
    move-result v7
    if-lez v7, :flush_done
    add-int/lit8 v5, v5, 0x1
    goto :flush_loop
    :flush_done
    invoke-virtual {v0}, Ljava/util/zip/Deflater;->finish()V
    invoke-virtual {v0, v2, v3, v6}, Ljava/util/zip/Deflater;->deflate([BII)I
    move-result v7
    add-int/2addr v3, v7
    invoke-virtual {v0}, Ljava/util/zip/Deflater;->finished()Z
    move-result v8
    invoke-virtual {v0}, Ljava/util/zip/Deflater;->end()V

    new-instance v0, Ljava/util/zip/Inflater;
    invoke-direct {v0}, Ljava/util/zip/Inflater;-><init>()V
    const/4 v7, 0x0
    invoke-virtual {v0, v2, v7, v3}, Ljava/util/zip/Inflater;->setInput([BII)V
    new-array v2, v9, [B
    invoke-virtual {v0, v2}, Ljava/util/zip/Inflater;->inflate([B)I
    move-result v6
    new-instance v1, Ljava/lang/String;
    invoke-direct {v1, v2, v7, v6}, Ljava/lang/String;-><init>([BII)V
    invoke-virtual {v1, v11}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z
    move-result v6
    invoke-virtual {v0}, Ljava/util/zip/Inflater;->finished()Z
    move-result v7
    and-int/2addr v6, v7
    invoke-virtual {v0}, Ljava/util/zip/Inflater;->end()V

    invoke-static {v4}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v4
    invoke-static {v5}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v5
    invoke-static {v8}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v8
    invoke-static {v6}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v6
    filled-new-array {v4, v5, v8, v6}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/zip/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# GZIP round trip. The empty write calls CRC32.update(b, 0, 0), which must
# keep the CRC: otherwise the trailer is wrong and reading fails.
#
#   ByteArrayOutputStream bos = new ByteArrayOutputStream();
#   GZIPOutputStream gz = new GZIPOutputStream(bos);
#   gz.write(TEXT.getBytes());
#   gz.write(new byte[0]);
#   gz.close();
#   byte[] packed = bos.toByteArray();
#   GZIPInputStream in = new GZIPInputStream(new ByteArrayInputStream(packed));
#   ByteArrayOutputStream back = new ByteArrayOutputStream();
#   byte[] b = new byte[16];
#   int n;
#   while ((n = in.read(b)) > 0) back.write(b, 0, n);
#   in.close();
#   return j(packed[0] & 0xff, packed[1] & 0xff, back.toString().equals(TEXT), back.size());
.method static gzip()Ljava/lang/String;
    .registers 9
    new-instance v0, Ljava/io/ByteArrayOutputStream;
    invoke-direct {v0}, Ljava/io/ByteArrayOutputStream;-><init>()V
    new-instance v1, Ljava/util/zip/GZIPOutputStream;
    invoke-direct {v1, v0}, Ljava/util/zip/GZIPOutputStream;-><init>(Ljava/io/OutputStream;)V
    invoke-static {}, Lcom/anyandroid/samples/zip/Main;->text()Ljava/lang/String;
    move-result-object v2
    invoke-virtual {v2}, Ljava/lang/String;->getBytes()[B
    move-result-object v3
    invoke-virtual {v1, v3}, Ljava/util/zip/GZIPOutputStream;->write([B)V
    const/4 v4, 0x0
    new-array v4, v4, [B
    invoke-virtual {v1, v4}, Ljava/util/zip/GZIPOutputStream;->write([B)V
    invoke-virtual {v1}, Ljava/util/zip/GZIPOutputStream;->close()V
    invoke-virtual {v0}, Ljava/io/ByteArrayOutputStream;->toByteArray()[B
    move-result-object v3

    new-instance v1, Ljava/util/zip/GZIPInputStream;
    new-instance v4, Ljava/io/ByteArrayInputStream;
    invoke-direct {v4, v3}, Ljava/io/ByteArrayInputStream;-><init>([B)V
    invoke-direct {v1, v4}, Ljava/util/zip/GZIPInputStream;-><init>(Ljava/io/InputStream;)V
    new-instance v0, Ljava/io/ByteArrayOutputStream;
    invoke-direct {v0}, Ljava/io/ByteArrayOutputStream;-><init>()V
    const/16 v4, 0x10
    new-array v4, v4, [B
    const/4 v5, 0x0
    :read_loop
    invoke-virtual {v1, v4}, Ljava/util/zip/GZIPInputStream;->read([B)I
    move-result v6
    if-lez v6, :read_done
    invoke-virtual {v0, v4, v5, v6}, Ljava/io/ByteArrayOutputStream;->write([BII)V
    goto :read_loop
    :read_done
    invoke-virtual {v1}, Ljava/util/zip/GZIPInputStream;->close()V

    aget-byte v4, v3, v5
    and-int/lit16 v4, v4, 0xff
    invoke-static {v4}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v4
    const/4 v5, 0x1
    aget-byte v5, v3, v5
    and-int/lit16 v5, v5, 0xff
    invoke-static {v5}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v5
    invoke-virtual {v0}, Ljava/io/ByteArrayOutputStream;->toString()Ljava/lang/String;
    move-result-object v6
    invoke-virtual {v6, v2}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z
    move-result v6
    invoke-static {v6}, Ljava/lang/Boolean;->valueOf(Z)Ljava/lang/Boolean;
    move-result-object v6
    invoke-virtual {v0}, Ljava/io/ByteArrayOutputStream;->size()I
    move-result v7
    invoke-static {v7}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v7
    filled-new-array {v4, v5, v6, v7}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/zip/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method

# Known checksums; an empty array leaves them unchanged.
#
#   byte[] digits = "123456789".getBytes(), none = new byte[0], wiki = "Wikipedia".getBytes();
#   CRC32 c = new CRC32(); c.update(digits, 0, 9); c.update(none, 0, 0);
#   Adler32 a = new Adler32(); a.update(wiki, 0, 9); a.update(none, 0, 0);
#   ByteBuffer bb = ByteBuffer.allocateDirect(9); bb.put(digits); bb.flip();
#   CRC32 cd = new CRC32(); cd.update(bb);
#   return j(Long.toHexString(c.getValue()), Long.toHexString(a.getValue()), Long.toHexString(cd.getValue()));
.method static sums()Ljava/lang/String;
    .registers 8
    const-string v0, "123456789"
    invoke-virtual {v0}, Ljava/lang/String;->getBytes()[B
    move-result-object v0
    const/4 v6, 0x0
    new-array v7, v6, [B

    new-instance v1, Ljava/util/zip/CRC32;
    invoke-direct {v1}, Ljava/util/zip/CRC32;-><init>()V
    array-length v2, v0
    invoke-virtual {v1, v0, v6, v2}, Ljava/util/zip/CRC32;->update([BII)V
    invoke-virtual {v1, v7, v6, v6}, Ljava/util/zip/CRC32;->update([BII)V
    invoke-virtual {v1}, Ljava/util/zip/CRC32;->getValue()J
    move-result-wide v2
    invoke-static {v2, v3}, Ljava/lang/Long;->toHexString(J)Ljava/lang/String;
    move-result-object v1

    const-string v2, "Wikipedia"
    invoke-virtual {v2}, Ljava/lang/String;->getBytes()[B
    move-result-object v2
    new-instance v3, Ljava/util/zip/Adler32;
    invoke-direct {v3}, Ljava/util/zip/Adler32;-><init>()V
    array-length v4, v2
    invoke-virtual {v3, v2, v6, v4}, Ljava/util/zip/Adler32;->update([BII)V
    invoke-virtual {v3, v7, v6, v6}, Ljava/util/zip/Adler32;->update([BII)V
    invoke-virtual {v3}, Ljava/util/zip/Adler32;->getValue()J
    move-result-wide v2
    invoke-static {v2, v3}, Ljava/lang/Long;->toHexString(J)Ljava/lang/String;
    move-result-object v2

    array-length v3, v0
    invoke-static {v3}, Ljava/nio/ByteBuffer;->allocateDirect(I)Ljava/nio/ByteBuffer;
    move-result-object v3
    invoke-virtual {v3, v0}, Ljava/nio/ByteBuffer;->put([B)Ljava/nio/ByteBuffer;
    invoke-virtual {v3}, Ljava/nio/Buffer;->flip()Ljava/nio/Buffer;
    new-instance v4, Ljava/util/zip/CRC32;
    invoke-direct {v4}, Ljava/util/zip/CRC32;-><init>()V
    invoke-virtual {v4, v3}, Ljava/util/zip/CRC32;->update(Ljava/nio/ByteBuffer;)V
    invoke-virtual {v4}, Ljava/util/zip/CRC32;->getValue()J
    move-result-wide v4
    invoke-static {v4, v5}, Ljava/lang/Long;->toHexString(J)Ljava/lang/String;
    move-result-object v3

    filled-new-array {v1, v2, v3}, [Ljava/lang/Object;
    move-result-object v0
    invoke-static {v0}, Lcom/anyandroid/samples/zip/Main;->j([Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
