# Équivalent Java :
#
#   public class Main {
#     public static void main(String[] args) {
#       System.out.println("net-demo : les tests du réseau passent par aa.host.NetProbe");
#     }
#   }

.class public Lcom/anyandroid/samples/net/Main;
.super Ljava/lang/Object;
.source "Main.java"

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static main([Ljava/lang/String;)V
    .registers 2
    sget-object v0, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v1, "net-demo : les tests du réseau passent par aa.host.NetProbe"
    invoke-virtual {v0, v1}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void
.end method
