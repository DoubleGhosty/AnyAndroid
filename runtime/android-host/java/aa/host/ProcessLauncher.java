package aa.host;

import aa.host.services.ActivityService;
import aa.host.services.ActivityTaskService;
import aa.host.services.DisplayService;
import android.app.ActivityThread;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.ResolveInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import java.util.ArrayList;

/**
 * Start of an application by the framework's own path (ADR-006 J5), as zygote and
 * system_server do it: ActivityThread.main attaches the process to the "activity" service,
 * which hands it to this launcher; the launcher binds the application (bindApplication) and
 * sends the launch of its launcher activity as a ClientTransaction (LaunchActivityItem then
 * ResumeActivityItem). The real handlers of ActivityThread run both on the main Looper. Once
 * the activity reports itself resumed, the main loop is left by a {@link Stop}.
 */
public final class ProcessLauncher implements ActivityService.AttachHandler {
  /** ActivityManager.PROCESS_STATE_TOP. */
  private static final int PROCESS_STATE_TOP = 2;

  /** Thrown on the main thread to leave Looper.loop (the main Looper cannot quit). */
  static final class Stop extends RuntimeException {
    Stop() {
      super("arrêt du lanceur", null, false, false);
    }
  }

  private final String pkg;

  private ProcessLauncher(String pkg) {
    this.pkg = pkg;
  }

  public static void main(String[] args) {
    ActivityService.onAttach(new ProcessLauncher(args[0]));
    ActivityTaskService.onResumed = () -> new Handler(Looper.getMainLooper()).post(() -> {
      Launcher.log("activité au premier plan");
      throw new Stop();
    });
    // Software drawing (ADR-006 §3): ViewRootImpl draws through drawSoftware, without HWUI.
    Reflect.setStatic(Reflect.type("android.view.ThreadedRenderer"), "sRendererEnabled", false);
    Launcher.log("démarrage : ActivityThread.main");
    try {
      ActivityThread.main(new String[0]);
    } catch (Stop stop) {
      Launcher.log("boucle principale quittée");
    }
    // An Android process ends when it is killed, not when main returns: the
    // framework's HandlerThreads are not daemons.
    System.exit(0);
  }

  @Override
  public void attached(android.app.IApplicationThread thread) {
    try {
      IPackageManager pm = ActivityThread.getPackageManager();
      ApplicationInfo ai = pm.getApplicationInfo(pkg, 0, 0);
      Launcher.log("processus attaché : " + ai.processName);
      bind(thread, ai);
      // After BIND_APPLICATION, before the launch: the Application exists.
      new Handler(Looper.getMainLooper()).post(() -> {
        ActivityThread.currentApplication().registerActivityLifecycleCallbacks(new Launcher.LifecycleLog());
        Launcher.log("application : " + ActivityThread.currentApplication().getClass().getName());
      });
      launch(thread, pm);
    } catch (Exception e) {
      throw e instanceof RuntimeException ? (RuntimeException) e : new IllegalStateException(e);
    }
  }

  /** IApplicationThread.bindApplication, with what system_server passes for a plain app. */
  private void bind(Object thread, ApplicationInfo ai) {
    Class<?> asm = Reflect.type("com.android.internal.os.ApplicationSharedMemory");
    Object sharedMemoryFd = Reflect.call(Reflect.callStatic(asm, "getInstance"), "getReadOnlyFileDescriptor");
    Object providers = Reflect.callStatic(Reflect.type("android.content.pm.ProviderInfoList"), "fromList", new ArrayList<>());
    Object compat = Reflect.getStatic(Reflect.type("android.content.res.CompatibilityInfo"), "DEFAULT_COMPATIBILITY_INFO");
    // system_server and the application share this process: the application installs its own
    // copy of the system features, read back from the shared memory, as in a fresh process.
    Reflect.callStatic(Reflect.type("android.content.pm.SystemFeaturesCache"), "clearInstance");
    Reflect.setStatic(asm, "sInstance", null);  // likewise its read-only mapping of the shared memory
    Reflect.call(thread, "bindApplication",
        ai.processName, ai, null, null, false, providers,
        null, null, null, null, null,  // instrumentation, profiler, arguments, watcher, UI automation
        0, false, false, false, false,  // debug mode, binder tracking, allocations, backup, persistent
        configuration(), compat, null, new Bundle(), "unknown",  // services, core settings, serial
        null, null, new long[0], new long[0], new long[0], false,  // autofill, capture, compat changes
        null, sharedMemoryFd, 0L, 0L);  // font map: the preinstalled one is loaded
  }

  /** LaunchActivityItem then ResumeActivityItem, in one ClientTransaction. */
  private void launch(Object thread, IPackageManager pm) throws Exception {
    Intent intent = new Intent("android.intent.action.MAIN");
    intent.addCategory("android.intent.category.LAUNCHER");
    intent.setPackage(pkg);
    ResolveInfo ri = pm.resolveIntent(intent, null, 0, 0);
    if (ri == null) throw new IllegalStateException("[AnyAndroid] pas d'activité de lancement dans " + pkg);
    ActivityInfo info = ri.activityInfo;
    intent.setComponent(new ComponentName(pkg, info.name));
    IBinder token = new Binder();
    Object client = Reflect.call(Reflect.callStatic(Reflect.type("android.app.ActivityTaskManager"), "getService"),
        "getActivityClientController");
    Object launch = Reflect.newInstance(Reflect.type("android.app.servertransaction.LaunchActivityItem"),
        token, intent, 0, info, configuration(), Reflect.newInstance(Reflect.type("android.content.res.Configuration")),
        0, null, null, PROCESS_STATE_TOP, null, null, null, null, null, true, null,
        new Binder(), client, new Binder(), false, null, new Binder(),
        Reflect.newInstance(Reflect.type("android.window.ActivityWindowInfo")), 0);
    Object resume = Reflect.newInstance(Reflect.type("android.app.servertransaction.ResumeActivityItem"),
        token, PROCESS_STATE_TOP, true, false);
    Object transaction = Reflect.newInstance(Reflect.type("android.app.servertransaction.ClientTransaction"), thread);
    Reflect.call(transaction, "addTransactionItem", launch);
    Reflect.call(transaction, "addTransactionItem", resume);
    Reflect.call(thread, "scheduleTransaction", transaction);
  }

  /** The global configuration, with the window configuration of a full-screen window. */
  private static Object configuration() {
    return DisplayService.displayConfiguration();
  }
}
