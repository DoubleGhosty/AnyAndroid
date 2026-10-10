# Classe anonyme générée par javac pour le OnClickListener de MainActivity.

.class Lcom/anyandroid/samples/nav/MainActivity$1;
.super Ljava/lang/Object;
.source "MainActivity.java"
.implements Landroid/view/View$OnClickListener;

.field final synthetic this$0:Lcom/anyandroid/samples/nav/MainActivity;

.method constructor <init>(Lcom/anyandroid/samples/nav/MainActivity;)V
    .registers 2
    iput-object p1, p0, Lcom/anyandroid/samples/nav/MainActivity$1;->this$0:Lcom/anyandroid/samples/nav/MainActivity;
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public onClick(Landroid/view/View;)V
    .registers 6
    new-instance v0, Landroid/content/Intent;
    iget-object v1, p0, Lcom/anyandroid/samples/nav/MainActivity$1;->this$0:Lcom/anyandroid/samples/nav/MainActivity;
    const-class v2, Lcom/anyandroid/samples/nav/DetailActivity;
    invoke-direct {v0, v1, v2}, Landroid/content/Intent;-><init>(Landroid/content/Context;Ljava/lang/Class;)V
    const-string v2, "message"
    const-string v3, "Bonjour de l'écran principal"
    invoke-virtual {v0, v2, v3}, Landroid/content/Intent;->putExtra(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;
    invoke-virtual {v1, v0}, Lcom/anyandroid/samples/nav/MainActivity;->startActivity(Landroid/content/Intent;)V
    return-void
.end method
