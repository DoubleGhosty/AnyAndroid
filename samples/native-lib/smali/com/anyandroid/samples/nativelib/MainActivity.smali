# Équivalent Java :
#
#   public class MainActivity extends Activity {
#     static { System.loadLibrary("native"); }
#     private static native String stringFromJNI();
#
#     @Override protected void onCreate(Bundle state) {
#       super.onCreate(state);
#       TextView text = new TextView(this);
#       text.setText(stringFromJNI());
#       setContentView(text);
#     }
#   }

.class public Lcom/anyandroid/samples/nativelib/MainActivity;
.super Landroid/app/Activity;
.source "MainActivity.java"

.method static constructor <clinit>()V
    .registers 1
    const-string v0, "native"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
    return-void
.end method

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/app/Activity;-><init>()V
    return-void
.end method

.method private static native stringFromJNI()Ljava/lang/String;
.end method

.method protected onCreate(Landroid/os/Bundle;)V
    .registers 4
    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V
    new-instance v0, Landroid/widget/TextView;
    invoke-direct {v0, p0}, Landroid/widget/TextView;-><init>(Landroid/content/Context;)V
    invoke-static {}, Lcom/anyandroid/samples/nativelib/MainActivity;->stringFromJNI()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Landroid/widget/TextView;->setText(Ljava/lang/CharSequence;)V
    invoke-virtual {p0, v0}, Lcom/anyandroid/samples/nativelib/MainActivity;->setContentView(Landroid/view/View;)V
    return-void
.end method
