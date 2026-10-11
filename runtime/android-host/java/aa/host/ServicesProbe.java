package aa.host;

import android.app.ActivityThread;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.ResolveInfo;

/** Self-test of the local system services (ADR-006 J3), run on an application's APK. */
public final class ServicesProbe {
  private ServicesProbe() {}

  public static void main(String[] args) throws Exception {
    String pkg = args[0];
    IPackageManager pm = ActivityThread.getPackageManager();
    ApplicationInfo ai = pm.getApplicationInfo(pkg, 0, 0);
    System.out.println("application : " + ai.packageName + ", uid " + ai.uid + ", cible " + ai.targetSdkVersion
        + ", processus " + ai.processName);
    System.out.println("  code " + ai.sourceDir + ", données " + ai.dataDir);
    Intent launcher = new Intent("android.intent.action.MAIN");
    launcher.addCategory("android.intent.category.LAUNCHER");
    launcher.setPackage(pkg);
    ResolveInfo ri = pm.resolveIntent(launcher, null, 0, 0);
    System.out.println("lanceur : " + (ri == null ? null : ri.activityInfo.name));
    ActivityInfo info = pm.getActivityInfo(new ComponentName(pkg, ri.activityInfo.name), 0, 0);
    System.out.println("activité : " + info.name + ", exportée " + info.exported + ", application "
        + info.applicationInfo.packageName);
    // A method the service does not implement (absent from the javac declaration): neutral value.
    Object installer =
        IPackageManager.class.getMethod("getInstallerPackageName", String.class).invoke(pm, pkg);
    System.out.println("inconnus : " + pm.getApplicationInfo("com.absent", 0, 0) + ", uid "
        + pm.getPackageUid("com.absent", 0, 0) + ", installeur " + installer);
  }
}
