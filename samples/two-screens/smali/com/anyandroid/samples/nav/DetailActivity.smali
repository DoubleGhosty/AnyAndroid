# Équivalent Java :
#
#   public class DetailActivity extends Activity implements View.OnClickListener {
#     @Override protected void onCreate(Bundle state) {
#       super.onCreate(state);
#       LinearLayout root = new LinearLayout(this);
#       root.setOrientation(LinearLayout.VERTICAL);
#       String message = getIntent().getStringExtra("message");
#       TextView text = new TextView(this);
#       text.setText("Message reçu : " + message);
#       root.addView(text);
#       Button back = new Button(this);
#       back.setText("Retour");
#       back.setOnClickListener(this);
#       root.addView(back);
#       setContentView(root);
#       setTitle("Détail");
#     }
#
#     @Override public void onClick(View v) { finish(); }
#   }

.class public Lcom/anyandroid/samples/nav/DetailActivity;
.super Landroid/app/Activity;
.source "DetailActivity.java"
.implements Landroid/view/View$OnClickListener;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/app/Activity;-><init>()V
    return-void
.end method

.method protected onCreate(Landroid/os/Bundle;)V
    .registers 7
    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V
    new-instance v0, Landroid/widget/LinearLayout;
    invoke-direct {v0, p0}, Landroid/widget/LinearLayout;-><init>(Landroid/content/Context;)V
    const/4 v1, 0x1
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->setOrientation(I)V

    invoke-virtual {p0}, Lcom/anyandroid/samples/nav/DetailActivity;->getIntent()Landroid/content/Intent;
    move-result-object v1
    const-string v2, "message"
    invoke-virtual {v1, v2}, Landroid/content/Intent;->getStringExtra(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v1

    new-instance v2, Landroid/widget/TextView;
    invoke-direct {v2, p0}, Landroid/widget/TextView;-><init>(Landroid/content/Context;)V
    new-instance v3, Ljava/lang/StringBuilder;
    invoke-direct {v3}, Ljava/lang/StringBuilder;-><init>()V
    const-string v4, "Message reçu : "
    invoke-virtual {v3, v4}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v3, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v3}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v3
    invoke-virtual {v2, v3}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V
    invoke-virtual {v0, v2}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    new-instance v2, Landroid/widget/Button;
    invoke-direct {v2, p0}, Landroid/widget/Button;-><init>(Landroid/content/Context;)V
    const-string v3, "Retour"
    invoke-virtual {v2, v3}, Landroid/widget/Button;->setText(Ljava/lang/CharSequence;)V
    invoke-virtual {v2, p0}, Landroid/widget/Button;->setOnClickListener(Landroid/view/View$OnClickListener;)V
    invoke-virtual {v0, v2}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    invoke-virtual {p0, v0}, Lcom/anyandroid/samples/nav/DetailActivity;->setContentView(Landroid/view/View;)V
    const-string v4, "Détail"
    invoke-virtual {p0, v4}, Lcom/anyandroid/samples/nav/DetailActivity;->setTitle(Ljava/lang/CharSequence;)V
    return-void
.end method

.method public onClick(Landroid/view/View;)V
    .registers 2
    invoke-virtual {p0}, Lcom/anyandroid/samples/nav/DetailActivity;->finish()V
    return-void
.end method
