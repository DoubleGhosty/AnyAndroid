# Équivalent Java :
#
#   public class Main {
#     static int total, finished;
#     static final Object LOCK = new Object();
#
#     public static void main(String[] args) throws Exception {
#       System.out.println("Démarrage de 4 threads");
#       Thread[] ts = new Thread[4];
#       for (int k = 0; k < 4; k++) {
#         ts[k] = new Thread(new Worker(k * 2500 + 1, (k + 1) * 2500), "travailleur-" + k);
#         ts[k].start();
#       }
#       synchronized (LOCK) { while (finished < 4) LOCK.wait(); }
#       for (Thread t : ts) t.join();
#       System.out.println("Somme de 1 à 10000 = " + total);
#       System.out.println("Thread principal : " + Thread.currentThread().getName());
#     }
#   }

.class public Lcom/anyandroid/samples/threads/Main;
.super Ljava/lang/Object;
.source "Main.java"

.field static total:I
.field static finished:I
.field static final LOCK:Ljava/lang/Object;

.method static constructor <clinit>()V
    .registers 1
    new-instance v0, Ljava/lang/Object;
    invoke-direct {v0}, Ljava/lang/Object;-><init>()V
    sput-object v0, Lcom/anyandroid/samples/threads/Main;->LOCK:Ljava/lang/Object;
    return-void
.end method

.method public static main([Ljava/lang/String;)V
    .registers 10
    sget-object v0, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v1, "Démarrage de 4 threads"
    invoke-virtual {v0, v1}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    const/4 v1, 0x4
    new-array v2, v1, [Ljava/lang/Thread;
    const/4 v3, 0x0
    :start_loop
    if-ge v3, v1, :started
    mul-int/lit16 v4, v3, 0x9c4
    add-int/lit8 v5, v4, 0x1
    add-int/lit16 v4, v4, 0x9c4
    new-instance v6, Lcom/anyandroid/samples/threads/Main$Worker;
    invoke-direct {v6, v5, v4}, Lcom/anyandroid/samples/threads/Main$Worker;-><init>(II)V
    new-instance v7, Ljava/lang/StringBuilder;
    invoke-direct {v7}, Ljava/lang/StringBuilder;-><init>()V
    const-string v8, "travailleur-"
    invoke-virtual {v7, v8}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v7, v3}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v7}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v7
    new-instance v8, Ljava/lang/Thread;
    invoke-direct {v8, v6, v7}, Ljava/lang/Thread;-><init>(Ljava/lang/Runnable;Ljava/lang/String;)V
    aput-object v8, v2, v3
    invoke-virtual {v8}, Ljava/lang/Thread;->start()V
    add-int/lit8 v3, v3, 0x1
    goto :start_loop
    :started

    sget-object v3, Lcom/anyandroid/samples/threads/Main;->LOCK:Ljava/lang/Object;
    monitor-enter v3
    :wait_loop
    sget v4, Lcom/anyandroid/samples/threads/Main;->finished:I
    if-ge v4, v1, :all_done
    invoke-virtual {v3}, Ljava/lang/Object;->wait()V
    goto :wait_loop
    :all_done
    monitor-exit v3

    const/4 v3, 0x0
    :join_loop
    if-ge v3, v1, :joined
    aget-object v4, v2, v3
    invoke-virtual {v4}, Ljava/lang/Thread;->join()V
    add-int/lit8 v3, v3, 0x1
    goto :join_loop
    :joined

    new-instance v1, Ljava/lang/StringBuilder;
    invoke-direct {v1}, Ljava/lang/StringBuilder;-><init>()V
    const-string v2, "Somme de 1 à 10000 = "
    invoke-virtual {v1, v2}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    sget v2, Lcom/anyandroid/samples/threads/Main;->total:I
    invoke-virtual {v1, v2}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v1}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    new-instance v1, Ljava/lang/StringBuilder;
    invoke-direct {v1}, Ljava/lang/StringBuilder;-><init>()V
    const-string v2, "Thread principal : "
    invoke-virtual {v1, v2}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-static {}, Ljava/lang/Thread;->currentThread()Ljava/lang/Thread;
    move-result-object v2
    invoke-virtual {v2}, Ljava/lang/Thread;->getName()Ljava/lang/String;
    move-result-object v2
    invoke-virtual {v1, v2}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v1}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void
.end method
