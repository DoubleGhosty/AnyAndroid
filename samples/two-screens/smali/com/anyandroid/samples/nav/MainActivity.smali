# Équivalent Java :
#
#   public class MainActivity extends Activity {
#     @Override protected void onCreate(Bundle state) {
#       super.onCreate(state);
#       LinearLayout root = new LinearLayout(this);
#       root.setOrientation(LinearLayout.VERTICAL);
#       TextView title = new TextView(this);
#       title.setText("Écran principal");
#       root.addView(title);
#       Button open = new Button(this);
#       open.setText("Ouvrir le détail");
#       open.setOnClickListener(new View.OnClickListener() {      // MainActivity$1
#         public void onClick(View v) {
#           startActivity(new Intent(MainActivity.this, DetailActivity.class)
#               .putExtra("message", "Bonjour de l'écran principal"));
#         }
#       });
#       root.addView(open);
#       setContentView(root);
#     }
#
#     @Override protected void onResume() {
#       super.onResume();
#       Log.d("Nav", "écran principal visible");
#     }
#   }

.class public Lcom/anyandroid/samples/nav/MainActivity;
.super Landroid/app/Activity;
.source "MainActivity.java"

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/app/Activity;-><init>()V
    return-void
.end method

.method protected onCreate(Landroid/os/Bundle;)V
    .registers 6
    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V
    new-instance v0, Landroid/widget/LinearLayout;
    invoke-direct {v0, p0}, Landroid/widget/LinearLayout;-><init>(Landroid/content/Context;)V
    const/4 v1, 0x1
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->setOrientation(I)V

    new-instance v1, Landroid/widget/TextView;
    invoke-direct {v1, p0}, Landroid/widget/TextView;-><init>(Landroid/content/Context;)V
    const-string v2, "Écran principal"
    invoke-virtual {v1, v2}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    new-instance v1, Landroid/widget/Button;
    invoke-direct {v1, p0}, Landroid/widget/Button;-><init>(Landroid/content/Context;)V
    const-string v2, "Ouvrir le détail"
    invoke-virtual {v1, v2}, Landroid/widget/Button;->setText(Ljava/lang/CharSequence;)V
    new-instance v2, Lcom/anyandroid/samples/nav/MainActivity$1;
    invoke-direct {v2, p0}, Lcom/anyandroid/samples/nav/MainActivity$1;-><init>(Lcom/anyandroid/samples/nav/MainActivity;)V
    invoke-virtual {v1, v2}, Landroid/widget/Button;->setOnClickListener(Landroid/view/View$OnClickListener;)V
    invoke-virtual {v0, v1}, Landroid/widget/LinearLayout;->addView(Landroid/view/View;)V

    invoke-virtual {p0, v0}, Lcom/anyandroid/samples/nav/MainActivity;->setContentView(Landroid/view/View;)V
    return-void
.end method

.method protected onResume()V
    .registers 3
    invoke-super {p0}, Landroid/app/Activity;->onResume()V
    const-string v0, "Nav"
    const-string v1, "écran principal visible"
    invoke-static {v0, v1}, Landroid/util/Log;->d(Ljava/lang/String;Ljava/lang/String;)I
    return-void
.end method
