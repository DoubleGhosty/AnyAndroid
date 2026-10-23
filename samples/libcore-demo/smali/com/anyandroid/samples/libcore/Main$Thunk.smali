# Équivalent Java : la tâche d'un sun.misc.Cleaner de la section « références ».
#   static final class Thunk implements Runnable {
#     final CountDownLatch done;
#     Thunk(CountDownLatch done) { this.done = done; }
#     public void run() { done.countDown(); }
#   }

.class final Lcom/anyandroid/samples/libcore/Main$Thunk;
.super Ljava/lang/Object;
.implements Ljava/lang/Runnable;
.source "Main.java"

.field final done:Ljava/util/concurrent/CountDownLatch;

.method constructor <init>(Ljava/util/concurrent/CountDownLatch;)V
    .registers 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput-object p1, p0, Lcom/anyandroid/samples/libcore/Main$Thunk;->done:Ljava/util/concurrent/CountDownLatch;
    return-void
.end method

.method public run()V
    .registers 2
    iget-object v0, p0, Lcom/anyandroid/samples/libcore/Main$Thunk;->done:Ljava/util/concurrent/CountDownLatch;
    invoke-virtual {v0}, Ljava/util/concurrent/CountDownLatch;->countDown()V
    return-void
.end method
