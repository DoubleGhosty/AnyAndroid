# Équivalent Java :
#
#   public class MainActivity extends Activity implements View.OnClickListener {
#     private TextView label;
#     private int clicks;
#
#     @Override protected void onCreate(Bundle state) {
#       super.onCreate(state);
#       Log.i("Demo", "onCreate");
#       LinearLayout root = new LinearLayout(this);
#       root.setOrientation(LinearLayout.VERTICAL);
#       TextView title = new TextView(this);
#       title.setText("Bonjour depuis Android, sur Windows !");
#       title.setTextSize(20f);
#       root.addView(title);
#       label = new TextView(this);
#       label.setText("Aucun clic pour l'instant.");
#       root.addView(label);
#       Button button = new Button(this);
#       button.setText("Cliquer");
#       button.setOnClickListener(this);
#       root.addView(button);
#       setContentView(root);
#     }
#
#     @Override public void onClick(View v) {
#       clicks++;
#       label.setText("Clics : " + clicks);
#       if (clicks == 3) Toast.makeText(this, "Trois clics !", Toast.LENGTH_SHORT).show();
#     }
#   }

.class public Lcom/anyandroid/samples/basic/MainActivity;
.super Landroid/app/Activity;
.source "MainActivity.java"
.implements Landroid/view/View$OnClickListener;

.field private clicks:I
.field private label:Landroid/widget/TextView;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/app/Activity;-><init>()V
    return-void
.end method

.method protected onCreate(Landroid/os/Bundle;)V
    .registers 6
    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V
    const-string v0, "Demo"
    const-string v1, "onCreate"
    invoke-static {v0, v1}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I

    new-instance v0, Landroid/widget/LinearLayout;
    invoke-direct {v0, p0}, Landroid/widget/LinearLayout;-><init>(Landroid/content/Context;)V
    const/4 v1, 0x1
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->setOrientation(I)V

    new-instance v1, Landroid/widget/TextView;
    invoke-direct {v1, p0}, Landroid/widget/TextView;-><init>(Landroid/content/Context;)V
    const-string v2, "Bonjour depuis Android, sur Windows !"
    invoke-virtual {v1, v2}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V
    const/high16 v2, 0x41a00000    # 20.0f
    invoke-virtual {v1, v2}, Landroid/widget/TextView;->setTextSize(F)V
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    new-instance v1, Landroid/widget/TextView;
    invoke-direct {v1, p0}, Landroid/widget/TextView;-><init>(Landroid/content/Context;)V
    iput-object v1, p0, Lcom/anyandroid/samples/basic/MainActivity;->label:Landroid/widget/TextView;
    const-string v2, "Aucun clic pour l'instant."
    invoke-virtual {v1, v2}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    new-instance v1, Landroid/widget/Button;
    invoke-direct {v1, p0}, Landroid/widget/Button;-><init>(Landroid/content/Context;)V
    const-string v2, "Cliquer"
    invoke-virtual {v1, v2}, Landroid/widget/Button;->setText(Ljava/lang/CharSequence;)V
    invoke-virtual {v1, p0}, Landroid/widget/Button;->setOnClickListener(Landroid/view/View$OnClickListener;)V
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    invoke-virtual {p0, v0}, Lcom/anyandroid/samples/basic/MainActivity;->setContentView(Landroid/view/View;)V
    return-void
.end method

.method public onClick(Landroid/view/View;)V
    .registers 6
    iget v0, p0, Lcom/anyandroid/samples/basic/MainActivity;->clicks:I
    add-int/lit8 v0, v0, 0x1
    iput v0, p0, Lcom/anyandroid/samples/basic/MainActivity;->clicks:I

    iget-object v1, p0, Lcom/anyandroid/samples/basic/MainActivity;->label:Landroid/widget/TextView;
    new-instance v2, Ljava/lang/StringBuilder;
    invoke-direct {v2}, Ljava/lang/StringBuilder;-><init>()V
    const-string v3, "Clics : "
    invoke-virtual {v2, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    invoke-virtual {v2, v0}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    invoke-virtual {v2}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v2
    invoke-virtual {v1, v2}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V

    const/4 v1, 0x3
    if-ne v0, v1, :done
    const-string v2, "Trois clics !"
    const/4 v3, 0x0
    invoke-static {p0, v2, v3}, Landroid/widget/Toast;->makeText(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;
    move-result-object v2
    invoke-virtual {v2}, Landroid/widget/Toast;->show()V
    :done
    return-void
.end method
