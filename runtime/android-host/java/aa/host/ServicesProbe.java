package aa.host;

import android.app.ActivityThread;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.ResolveInfo;
import android.os.Binder;
import android.os.ServiceManager;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.List;

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
    checkOverrides();
  }

  /**
   * Checks that each public method of the services (and of the binder objects they declare)
   * overrides a method of the framework's interface. The host code is compiled against
   * hand-written declarations (stubs/): one out of step with framework.jar compiles, yet its
   * implementation is never called.
   */
  static void checkOverrides() {
    List<Class<?>> classes = new ArrayList<>();
    for (String name : ServiceManager.listServices()) addBinderClasses(ServiceManager.getService(name).getClass(), classes);
    int methods = 0;
    List<String> orphans = new ArrayList<>();
    for (Class<?> cls : classes) {
      for (Method m : cls.getDeclaredMethods()) {
        int mod = m.getModifiers();
        if (!Modifier.isPublic(mod) || Modifier.isStatic(mod) || m.isSynthetic() || m.isBridge()) continue;
        methods++;
        if (!overrides(cls.getSuperclass(), m)) orphans.add(cls.getSimpleName() + "." + m.getName());
      }
    }
    System.out.println("surcharges : " + methods + " méthodes dans " + classes.size() + " classes, "
        + (orphans.isEmpty() ? "toutes reconnues" : "orphelines " + orphans));
  }

  private static void addBinderClasses(Class<?> cls, List<Class<?>> out) {
    if (out.contains(cls) || !Binder.class.isAssignableFrom(cls)) return;
    out.add(cls);
    for (Class<?> inner : cls.getDeclaredClasses()) addBinderClasses(inner, out);
  }

  private static boolean overrides(Class<?> parent, Method m) {
    try {
      return parent.getMethod(m.getName(), m.getParameterTypes()).getReturnType() == m.getReturnType();
    } catch (NoSuchMethodException e) {
      return false;
    }
  }
}
