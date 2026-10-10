# Équivalent Java (identifiants R tirés de `apkgen build --res ... --r-txt R.txt`) :
#
#   public class MainActivity extends Activity {
#     @Override protected void onCreate(Bundle state) {
#       super.onCreate(state);
#       setContentView(R.layout.main);
#       Resources res = getResources();
#       Log.i("ResDemo", res.getResourceName(R.id.greet));
#       Log.i("ResDemo", Integer.toHexString(res.getColor(R.color.accent)));
#       Log.i("ResDemo", String.valueOf(res.getDimensionPixelSize(R.dimen.gap)));
#       Log.i("ResDemo", String.valueOf(res.getInteger(R.integer.max_greetings)));
#     }
#
#     // android:onClick="onGreet" dans res/layout/main.xml
#     public void onGreet(View v) {
#       String name = ((EditText) findViewById(R.id.name)).getText().toString();
#       ((TextView) findViewById(R.id.output)).setText(getString(R.string.greeting, name));
#       Toast.makeText(this, R.string.greeted_toast, Toast.LENGTH_SHORT).show();
#     }
#   }

.class public Lcom/anyandroid/samples/res/MainActivity;
.super Landroid/app/Activity;
.source "MainActivity.java"

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/app/Activity;-><init>()V
    return-void
.end method

.method protected onCreate(Landroid/os/Bundle;)V
    .registers 5
    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V
    const v0, 0x7f020001    # R.layout.main
    invoke-virtual {p0, v0}, Lcom/anyandroid/samples/res/MainActivity;->setContentView(I)V

    invoke-virtual {p0}, Lcom/anyandroid/samples/res/MainActivity;->getResources()Landroid/content/res/Resources;
    move-result-object v0
    const-string v1, "ResDemo"

    const v2, 0x7f010001    # R.id.greet
    invoke-virtual {v0, v2}, Landroid/content/res/Resources;->getResourceName(I)Ljava/lang/String;
    move-result-object v2
    invoke-static {v1, v2}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I

    const v2, 0x7f040000    # R.color.accent
    invoke-virtual {v0, v2}, Landroid/content/res/Resources;->getColor(I)I
    move-result v2
    invoke-static {v2}, Ljava/lang/Integer;->toHexString(I)Ljava/lang/String;
    move-result-object v2
    invoke-static {v1, v2}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I

    const v2, 0x7f050000    # R.dimen.gap
    invoke-virtual {v0, v2}, Landroid/content/res/Resources;->getDimensionPixelSize(I)I
    move-result v2
    invoke-static {v2}, Ljava/lang/String;->valueOf(I)Ljava/lang/String;
    move-result-object v2
    invoke-static {v1, v2}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I

    const v2, 0x7f060000    # R.integer.max_greetings
    invoke-virtual {v0, v2}, Landroid/content/res/Resources;->getInteger(I)I
    move-result v2
    invoke-static {v2}, Ljava/lang/String;->valueOf(I)Ljava/lang/String;
    move-result-object v2
    invoke-static {v1, v2}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I
    return-void
.end method

.method public onGreet(Landroid/view/View;)V
    .registers 5
    const v0, 0x7f010002    # R.id.name
    invoke-virtual {p0, v0}, Lcom/anyandroid/samples/res/MainActivity;->findViewById(I)Landroid/view/View;
    move-result-object v0
    check-cast v0, Landroid/widget/EditText;
    invoke-virtual {v0}, Landroid/widget/EditText;->getText()Landroid/text/Editable;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/Object;->toString()Ljava/lang/String;
    move-result-object v0

    const/4 v1, 0x1
    new-array v1, v1, [Ljava/lang/Object;
    const/4 v2, 0x0
    aput-object v0, v1, v2
    const v2, 0x7f030004    # R.string.greeting
    invoke-virtual {p0, v2, v1}, Lcom/anyandroid/samples/res/MainActivity;->getString(I[Ljava/lang/Object;)Ljava/lang/String;
    move-result-object v1

    const v2, 0x7f010003    # R.id.output
    invoke-virtual {p0, v2}, Lcom/anyandroid/samples/res/MainActivity;->findViewById(I)Landroid/view/View;
    move-result-object v2
    check-cast v2, Landroid/widget/TextView;
    invoke-virtual {v2, v1}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V

    const v1, 0x7f030003    # R.string.greeted_toast
    const/4 v2, 0x0
    invoke-static {p0, v1, v2}, Landroid/widget/Toast;->makeText(Landroid/content/Context;II)Landroid/widget/Toast;
    move-result-object v1
    invoke-virtual {v1}, Landroid/widget/Toast;->show()V
    return-void
.end method
