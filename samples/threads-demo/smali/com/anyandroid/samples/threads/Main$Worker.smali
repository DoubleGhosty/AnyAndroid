# Équivalent Java :
#
#   static class Worker implements Runnable {
#     final int from, to;
#     Worker(int from, int to) { this.from = from; this.to = to; }
#     public void run() {
#       int s = 0;
#       for (int i = from; i <= to; i++) s += i;
#       synchronized (LOCK) { total += s; finished++; LOCK.notifyAll(); }
#     }
#   }

.class Lcom/anyandroid/samples/threads/Main$Worker;
.super Ljava/lang/Object;
.source "Main.java"
.implements Ljava/lang/Runnable;

.field final from:I
.field final to:I

.method constructor <init>(II)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, Lcom/anyandroid/samples/threads/Main$Worker;->from:I
    iput p2, p0, Lcom/anyandroid/samples/threads/Main$Worker;->to:I
    return-void
.end method

.method public run()V
    .registers 6
    const/4 v0, 0x0
    iget v1, p0, Lcom/anyandroid/samples/threads/Main$Worker;->from:I
    iget v2, p0, Lcom/anyandroid/samples/threads/Main$Worker;->to:I
    :loop
    if-gt v1, v2, :done
    add-int/2addr v0, v1
    add-int/lit8 v1, v1, 0x1
    goto :loop
    :done
    sget-object v3, Lcom/anyandroid/samples/threads/Main;->LOCK:Ljava/lang/Object;
    monitor-enter v3
    sget v4, Lcom/anyandroid/samples/threads/Main;->total:I
    add-int/2addr v4, v0
    sput v4, Lcom/anyandroid/samples/threads/Main;->total:I
    sget v4, Lcom/anyandroid/samples/threads/Main;->finished:I
    add-int/lit8 v4, v4, 0x1
    sput v4, Lcom/anyandroid/samples/threads/Main;->finished:I
    invoke-virtual {v3}, Ljava/lang/Object;->notifyAll()V
    monitor-exit v3
    return-void
.end method
