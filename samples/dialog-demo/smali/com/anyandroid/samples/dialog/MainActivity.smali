# Équivalent Java :
#
#   public class MainActivity extends Activity
#       implements View.OnClickListener, DialogInterface.OnClickListener {
#     private TextView answer;
#
#     @Override protected void onCreate(Bundle state) {
#       super.onCreate(state);
#       LinearLayout root = new LinearLayout(this);
#       root.setOrientation(LinearLayout.VERTICAL);
#       answer = new TextView(this);
#       answer.setText("Réponse : aucune");
#       root.addView(answer);
#       Button ask = new Button(this);
#       ask.setText("Demander");
#       ask.setOnClickListener(this);
#       root.addView(ask);
#       setContentView(root);
#     }
#
#     @Override public void onClick(View v) {
#       new AlertDialog.Builder(this)
#           .setTitle("Question")
#           .setMessage("Continuer ?")
#           .setPositiveButton("Oui", this)
#           .setNegativeButton("Non", this)
#           .show();
#     }
#
#     @Override public void onClick(DialogInterface dialog, int which) {
#       answer.setText(which == DialogInterface.BUTTON_POSITIVE ? "Réponse : oui" : "Réponse : non");
#     }
#   }

.class public Lcom/anyandroid/samples/dialog/MainActivity;
.super Landroid/app/Activity;
.source "MainActivity.java"
.implements Landroid/view/View$OnClickListener;
.implements Landroid/content/DialogInterface$OnClickListener;

.field private answer:Landroid/widget/TextView;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/app/Activity;-><init>()V
    return-void
.end method

.method protected onCreate(Landroid/os/Bundle;)V
    .registers 5
    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V

    new-instance v0, Landroid/widget/LinearLayout;
    invoke-direct {v0, p0}, Landroid/widget/LinearLayout;-><init>(Landroid/content/Context;)V
    const/4 v1, 0x1
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->setOrientation(I)V

    new-instance v1, Landroid/widget/TextView;
    invoke-direct {v1, p0}, Landroid/widget/TextView;-><init>(Landroid/content/Context;)V
    iput-object v1, p0, Lcom/anyandroid/samples/dialog/MainActivity;->answer:Landroid/widget/TextView;
    const-string v2, "Réponse : aucune"
    invoke-virtual {v1, v2}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    new-instance v1, Landroid/widget/Button;
    invoke-direct {v1, p0}, Landroid/widget/Button;-><init>(Landroid/content/Context;)V
    const-string v2, "Demander"
    invoke-virtual {v1, v2}, Landroid/widget/Button;->setText(Ljava/lang/CharSequence;)V
    invoke-virtual {v1, p0}, Landroid/widget/Button;->setOnClickListener(Landroid/view/View$OnClickListener;)V
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    invoke-virtual {p0, v0}, Lcom/anyandroid/samples/dialog/MainActivity;->setContentView(Landroid/view/View;)V
    return-void
.end method

.method public onClick(Landroid/view/View;)V
    .registers 4
    new-instance v0, Landroid/app/AlertDialog$Builder;
    invoke-direct {v0, p0}, Landroid/app/AlertDialog$Builder;-><init>(Landroid/content/Context;)V
    const-string v1, "Question"
    invoke-virtual {v0, v1}, Landroid/app/AlertDialog$Builder;->setTitle(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;
    const-string v1, "Continuer ?"
    invoke-virtual {v0, v1}, Landroid/app/AlertDialog$Builder;->setMessage(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;
    const-string v1, "Oui"
    invoke-virtual {v0, v1, p0}, Landroid/app/AlertDialog$Builder;->setPositiveButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;
    const-string v1, "Non"
    invoke-virtual {v0, v1, p0}, Landroid/app/AlertDialog$Builder;->setNegativeButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;
    invoke-virtual {v0}, Landroid/app/AlertDialog$Builder;->show()Landroid/app/AlertDialog;
    return-void
.end method

.method public onClick(Landroid/content/DialogInterface;I)V
    .registers 5
    iget-object v0, p0, Lcom/anyandroid/samples/dialog/MainActivity;->answer:Landroid/widget/TextView;
    const/4 v1, -0x1
    if-ne p2, v1, :no
    const-string v1, "Réponse : oui"
    goto :set
    :no
    const-string v1, "Réponse : non"
    :set
    invoke-virtual {v0, v1}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V
    return-void
.end method
