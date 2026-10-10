# Équivalent Java :
#
#   static class Quit implements Runnable {
#     static boolean ran;
#     public void run() { ran = true; Looper.myLooper().quit(); }
#   }

.class Lcom/anyandroid/samples/framework/Main$Quit;
.super Ljava/lang/Object;
.source "Main.java"
.implements Ljava/lang/Runnable;

.field static ran:Z

.method constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public run()V
    .registers 2
    const/4 v0, 0x1
    sput-boolean v0, Lcom/anyandroid/samples/framework/Main$Quit;->ran:Z
    invoke-static {}, Landroid/os/Looper;->myLooper()Landroid/os/Looper;
    move-result-object v0
    invoke-virtual {v0}, Landroid/os/Looper;->quit()V
    return-void
.end method
