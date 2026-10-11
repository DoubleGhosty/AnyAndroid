package aa.host;

import android.app.Activity;
import android.app.ActivityThread;
import android.app.Application;
import android.app.Instrumentation;
import android.app.LoadedApk;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.ResolveInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;

/**
 * Direct start of an application (ADR-006 J4), as Robolectric does: ActivityThread,
 * LoadedApk, Application, then the launcher Activity up to onResume, through the framework's
 * own code, without a window. Each lifecycle step is printed.
 */
public final class Launcher {
  private Launcher() {}

  static void log(String s) {
    System.out.println(s);
  }

  public static void main(String[] args) throws Exception {
    String pkg = args[0];
    Looper.prepareMainLooper();
    // ActivityThread.attach(false) without system_server.
    ActivityThread thread = (ActivityThread) Reflect.newInstance(ActivityThread.class);
    Reflect.setStatic(ActivityThread.class, "sCurrentActivityThread", thread);
    Instrumentation instrumentation = new Instrumentation();
    Reflect.call(instrumentation, "basicInit", thread);
    Reflect.set(thread, "mInstrumentation", instrumentation);
    Reflect.set(thread, "mConfigurationController",
        Reflect.newInstance(Reflect.type("android.app.ConfigurationController"), thread));
    log("démarrage : ActivityThread prêt");

    IPackageManager pm = ActivityThread.getPackageManager();
    ApplicationInfo ai = pm.getApplicationInfo(pkg, 0, 0);
    LoadedApk apk = thread.getPackageInfoNoCheck(ai);
    Application app = apk.makeApplicationInner(false, null);
    Reflect.set(thread, "mInitialApplication", app);
    app.registerActivityLifecycleCallbacks(new LifecycleLog());
    log("application : " + app.getClass().getName());

    Intent intent = new Intent("android.intent.action.MAIN");
    intent.addCategory("android.intent.category.LAUNCHER");
    intent.setPackage(pkg);
    ResolveInfo ri = pm.resolveIntent(intent, null, 0, 0);
    if (ri == null) throw new IllegalStateException("[AnyAndroid] pas d'activité de lancement dans " + pkg);
    ActivityInfo info = ri.activityInfo;
    intent.setComponent(new ComponentName(pkg, info.name));

    IBinder token = new Binder();
    Context context = (Context) Reflect.callStatic(Reflect.type("android.app.ContextImpl"), "createActivityContext",
        thread, apk, info, token, 0, null);
    Activity activity = instrumentation.newActivity(apk.getClassLoader(), info.name, intent);
    Reflect.call(context, "setOuterContext", activity);
    Object config = Reflect.newInstance(Reflect.type("android.content.res.Configuration"),
        Reflect.call(Reflect.call(app, "getResources"), "getConfiguration"));
    Reflect.call(activity, "attach", context, thread, instrumentation, token, 0, app, intent, info, info.name,
        null, null, null, config, pkg, null, null, null, null, null, null);
    int theme = info.getThemeResource();
    if (theme != 0) activity.setTheme(theme);
    log("activité : " + activity.getLocalClassName() + " attachée");

    instrumentation.callActivityOnCreate(activity, null);
    Reflect.call(activity, "performStart", "aa-launcher");
    instrumentation.callActivityOnPostCreate(activity, null);
    Reflect.call(activity, "performResume", false, "aa-launcher");
    log("activité au premier plan");
  }

  /** Prints the lifecycle events of the application's activities. */
  static final class LifecycleLog implements Application.ActivityLifecycleCallbacks {
    @Override
    public void onActivityCreated(Activity a, Bundle state) {
      log("cycle : " + a.getLocalClassName() + ".onCreate");
    }

    @Override
    public void onActivityStarted(Activity a) {
      log("cycle : " + a.getLocalClassName() + ".onStart");
    }

    @Override
    public void onActivityResumed(Activity a) {
      log("cycle : " + a.getLocalClassName() + ".onResume");
    }

    @Override
    public void onActivityPaused(Activity a) {
      log("cycle : " + a.getLocalClassName() + ".onPause");
    }

    @Override
    public void onActivityStopped(Activity a) {
      log("cycle : " + a.getLocalClassName() + ".onStop");
    }

    @Override
    public void onActivitySaveInstanceState(Activity a, Bundle state) {}

    @Override
    public void onActivityDestroyed(Activity a) {
      log("cycle : " + a.getLocalClassName() + ".onDestroy");
    }
  }
}
