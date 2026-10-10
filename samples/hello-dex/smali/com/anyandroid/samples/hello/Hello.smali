# Équivalent Java :
#
#   public class Hello {
#     public static void main(String[] args) {
#       System.out.println("Bonjour depuis Dalvik, exécuté sur Windows sans machine virtuelle !");
#       System.out.println(new StringBuilder("fib(20) = ").append(fib(20)).toString());
#       System.out.println("Arguments : " + args.length);
#       try {
#         int zero = args.length - args.length;
#         int x = 10 / zero;
#       } catch (ArithmeticException e) {
#         System.out.println("Exception rattrapée : " + e.getMessage());
#       }
#     }
#     static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
#   }

.class public Lcom/anyandroid/samples/hello/Hello;
.super Ljava/lang/Object;
.source "Hello.java"

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static main([Ljava/lang/String;)V
    .registers 5
    sget-object v0, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v1, "Bonjour depuis Dalvik, exécuté sur Windows sans machine virtuelle !"
    invoke-virtual {v0, v1}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    new-instance v1, Ljava/lang/StringBuilder;
    const-string v2, "fib(20) = "
    invoke-direct {v1, v2}, Ljava/lang/StringBuilder;-><init>(Ljava/lang/String;)V
    const/16 v2, 0x14
    invoke-static {v2}, Lcom/anyandroid/samples/hello/Hello;->fib(I)I
    move-result v2
    invoke-virtual {v1, v2}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    move-result-object v1
    invoke-virtual {v1}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    new-instance v1, Ljava/lang/StringBuilder;
    invoke-direct {v1}, Ljava/lang/StringBuilder;-><init>()V
    const-string v2, "Arguments : "
    invoke-virtual {v1, v2}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    array-length v2, p0
    invoke-virtual {v1, v2}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v1}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    :try_start
    array-length v2, p0
    sub-int v2, v2, v2
    const/16 v3, 0xa
    div-int/2addr v3, v2
    :try_end
    .catch Ljava/lang/ArithmeticException; {:try_start .. :try_end} :catch
    return-void

    :catch
    move-exception v1
    new-instance v2, Ljava/lang/StringBuilder;
    invoke-direct {v2}, Ljava/lang/StringBuilder;-><init>()V
    const-string v3, "Exception rattrapée : "
    invoke-virtual {v2, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v1}, Ljava/lang/Throwable;->getMessage()Ljava/lang/String;
    move-result-object v3
    invoke-virtual {v2, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v2}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v2
    invoke-virtual {v0, v2}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void
.end method

.method static fib(I)I
    .registers 4
    const/4 v0, 0x2
    if-ge p0, v0, :recurse
    return p0
    :recurse
    add-int/lit8 v0, p0, -0x1
    invoke-static {v0}, Lcom/anyandroid/samples/hello/Hello;->fib(I)I
    move-result v1
    add-int/lit8 v0, p0, -0x2
    invoke-static {v0}, Lcom/anyandroid/samples/hello/Hello;->fib(I)I
    move-result v2
    add-int/2addr v1, v2
    return v1
.end method
