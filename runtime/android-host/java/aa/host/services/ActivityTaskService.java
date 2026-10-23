package aa.host.services;

import aa.host.Reflect;
import android.app.ActivityManager;
import android.app.HandoffActivityData;
import android.app.IActivityClientController;
import android.app.IActivityTaskManager;
import android.app.IApplicationThread;
import android.app.IRequestFinishCallback;
import android.app.ProfilerInfo;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.HashMap;
import java.util.Map;

/**
 * The "activity_task" service (ADR-006 J4, J9): the activity task manager.
 * Manages the activity backstack, lifecycle transitions (pause, stop, resume, destroy),
 * and client callbacks through {@link ActivityClient}.
 */
public final class ActivityTaskService extends IActivityTaskManager.Stub {
  private static final int PROCESS_STATE_TOP = 2;
  /** ActivityManager.START_INTENT_NOT_RESOLVED, START_CLASS_NOT_FOUND. */
  private static final int START_INTENT_NOT_RESOLVED = -91, START_CLASS_NOT_FOUND = -92;

  public static final int STATE_INITIALIZING = 0;
  public static final int STATE_STARTED = 1;
  public static final int STATE_RESUMED = 2;
  public static final int STATE_PAUSED = 3;
  public static final int STATE_STOPPED = 4;
  public static final int STATE_DESTROYED = 5;

  public static final class ActivityRecord {
    public final IBinder token;
    public final Intent intent;
    public final ActivityInfo info;
    public int state = STATE_INITIALIZING;
    public boolean finishing = false;
    boolean launchedFromHome;  // ActivityRecord.LAUNCH_SOURCE_TYPE_HOME: the launcher's own start

    ActivityRecord(IBinder token, Intent intent, ActivityInfo info) {
      this.token = token;
      this.intent = intent;
      this.info = info;
    }
  }

  private static volatile ActivityTaskService sInstance;
  public static volatile Object sApplicationThread;
  public static volatile PackageService sPackageService;
  public static volatile String sPackageName;
  public static volatile Runnable onResumed;
  public static volatile Runnable onStackEmpty;
  /** The task went behind the home screen (BACK on its root activity); there is no home here. */
  public static volatile Runnable onTaskMovedToBack;

  private final Deque<ActivityRecord> stack = new ArrayDeque<>();  // the single task, top first
  private boolean movingToBack;
  private final Map<IBinder, ActivityRecord> activities = new HashMap<>();
  private final ActivityClient client = new ActivityClient(this);

  public ActivityTaskService() {
    super(LocalPermissionEnforcer.INSTANCE);
    sInstance = this;
  }

  public static ActivityTaskService getInstance() {
    return sInstance;
  }

  @Override
  public IActivityClientController getActivityClientController() {
    return client;
  }

  /**
   * Pushes the initial activity token created by the launcher onto the stack: the launcher plays
   * the home screen's part. Static: a public instance method of a binder service must override
   * the framework's interface (ServicesProbe).
   */
  public static void recordInitialActivity(IBinder token, Intent intent, ActivityInfo info) {
    ActivityTaskService service = sInstance;
    if (service == null) return;
    synchronized (service) {
      ActivityRecord rec = new ActivityRecord(token, intent, info);
      rec.state = STATE_INITIALIZING;
      rec.launchedFromHome = true;
      service.stack.push(rec);
      service.activities.put(token, rec);
    }
  }

  @Override
  public synchronized int startActivity(
      IApplicationThread caller,
      String callingPackage,
      String callingFeatureId,
      Intent intent,
      String resolvedType,
      IBinder resultTo,
      String resultWho,
      int requestCode,
      int flags,
      ProfilerInfo profilerInfo,
      Bundle options) {
    if (intent == null) return START_INTENT_NOT_RESOLVED;
    ActivityInfo info = resolveActivity(intent);
    if (info == null) {
      // Instrumentation.checkStartActivityResult turns both into ActivityNotFoundException.
      return intent.getComponent() != null ? START_CLASS_NOT_FOUND : START_INTENT_NOT_RESOLVED;
    }
    IBinder token = new Binder();
    ActivityRecord next = new ActivityRecord(token, intent, info);
    ActivityRecord top = stack.peek();
    activities.put(token, next);
    stack.push(next);

    if (top != null && top.state == STATE_RESUMED) {
      pauseActivity(top, false);
    } else {
      launchAndResume(next);
    }
    return 0; // START_SUCCESS
  }

  private ActivityInfo resolveActivity(Intent intent) {
    ComponentName cn = intent.getComponent();
    if (cn != null && sPackageService != null) {
      return sPackageService.getActivityInfo(cn, 0, 0);
    }
    if (sPackageService != null) {
      android.content.pm.ResolveInfo ri = sPackageService.resolveIntent(intent, null, 0, 0);
      if (ri != null) return ri.activityInfo;
    }
    return null;
  }

  void onActivityPaused(IBinder token) {
    synchronized (this) {
      ActivityRecord rec = activities.get(token);
      if (rec != null) rec.state = STATE_PAUSED;
      if (rec != null && rec.finishing) {
        stack.remove(rec);
        ActivityRecord top = stack.peek();
        if (top != null) {
          resumeActivity(top);
        }
        stopActivity(rec);
        destroyActivity(rec);
        if (stack.isEmpty()) {
          Runnable empty = onStackEmpty;
          if (empty != null) empty.run();
        }
      } else if (movingToBack && rec != null && rec == stack.peekLast()) {
        stopActivity(rec);  // the task is behind the home screen: no longer visible
      } else {
        ActivityRecord top = stack.peek();
        if (top != null && top != rec && top.state == STATE_INITIALIZING) {
          launchAndResume(top);
        }
      }
    }
  }

  void onActivityResumed(IBinder token) {
    synchronized (this) {
      ActivityRecord rec = activities.get(token);
      if (rec != null) rec.state = STATE_RESUMED;
      for (ActivityRecord r : stack) {
        if (r != rec && r.state == STATE_PAUSED && !r.finishing) {
          stopActivity(r);
        }
      }
    }
    Runnable r = onResumed;
    if (r != null) r.run();
  }

  void onActivityStopped(IBinder token) {
    boolean movedToBack;
    synchronized (this) {
      ActivityRecord rec = activities.get(token);
      if (rec != null) rec.state = STATE_STOPPED;
      movedToBack = movingToBack && rec != null && rec == stack.peekLast();
      if (movedToBack) movingToBack = false;
    }
    Runnable back = onTaskMovedToBack;
    if (movedToBack && back != null) back.run();
  }

  /**
   * ActivityClientController.shouldMoveTaskToBack (android-17.0.0_r1): BACK on the last activity
   * of the task, its base activity, started from the home screen with a main intent, moves the
   * task to the back instead of finishing the activity.
   */
  private synchronized boolean shouldMoveTaskToBack(IBinder token) {
    ActivityRecord r = activities.get(token);
    ActivityRecord root = stack.peekLast();
    return r != null && r == root && stack.peek() == r && root.launchedFromHome && isMainIntent(root.intent);
  }

  /** ActivityRecord.isMainIntent. */
  private static boolean isMainIntent(Intent intent) {
    return "android.intent.action.MAIN".equals(intent.getAction())
        && intent.hasCategory("android.intent.category.LAUNCHER") && intent.getCategories().size() == 1
        && intent.getData() == null && intent.getType() == null;
  }

  /** moveActivityTaskToBackInner: the home screen comes in front, the activity pauses then stops. */
  private synchronized void moveTaskToBack(IBinder token) {
    ActivityRecord rec = activities.get(token);
    if (rec == null || movingToBack) return;
    movingToBack = true;
    if (rec.state == STATE_RESUMED) {
      pauseActivity(rec, false);
    } else if (rec.state == STATE_PAUSED) {
      stopActivity(rec);
    }
  }

  void onActivityDestroyed(IBinder token) {
    synchronized (this) {
      activities.remove(token);
    }
  }

  boolean doFinishActivity(IBinder token) {
    synchronized (this) {
      ActivityRecord rec = activities.get(token);
      if (rec == null) return false;
      rec.finishing = true;
      if (rec.state == STATE_RESUMED) {
        pauseActivity(rec, true);
      } else {
        stack.remove(rec);
        ActivityRecord top = stack.peek();
        if (top != null && top.state != STATE_RESUMED) {
          resumeActivity(top);
        }
        stopActivity(rec);
        destroyActivity(rec);
        if (stack.isEmpty()) {
          Runnable empty = onStackEmpty;
          if (empty != null) empty.run();
        }
      }
      return true;
    }
  }

  private void launchAndResume(ActivityRecord rec) {
    Object thread = sApplicationThread;
    if (thread == null) return;
    try {
      Object clientController = client;
      Object launch = Reflect.newInstance(Reflect.type("android.app.servertransaction.LaunchActivityItem"),
          rec.token, rec.intent, 0, rec.info, DisplayService.displayConfiguration(),
          Reflect.newInstance(Reflect.type("android.content.res.Configuration")),
          0, null, null, PROCESS_STATE_TOP, null, null, null, null, null, true, null,
          new Binder(), clientController, new Binder(), false, null, new Binder(),
          Reflect.newInstance(Reflect.type("android.window.ActivityWindowInfo")), 0);
      Object resume = Reflect.newInstance(Reflect.type("android.app.servertransaction.ResumeActivityItem"),
          rec.token, PROCESS_STATE_TOP, true, false);
      Object transaction = Reflect.newInstance(Reflect.type("android.app.servertransaction.ClientTransaction"), thread);
      Reflect.call(transaction, "addTransactionItem", launch);
      Reflect.call(transaction, "addTransactionItem", resume);
      Reflect.call(thread, "scheduleTransaction", transaction);
    } catch (Exception e) {
      throw new RuntimeException("[AnyAndroid] Échec du lancement de l'activité " + rec.info.name, e);
    }
  }

  private void pauseActivity(ActivityRecord rec, boolean finished) {
    Object thread = sApplicationThread;
    if (thread == null) return;
    try {
      Object pause = Reflect.newInstance(Reflect.type("android.app.servertransaction.PauseActivityItem"),
          rec.token, finished, false, false, false);
      Object transaction = Reflect.newInstance(Reflect.type("android.app.servertransaction.ClientTransaction"), thread);
      Reflect.call(transaction, "addTransactionItem", pause);
      Reflect.call(thread, "scheduleTransaction", transaction);
    } catch (Exception e) {
      throw new RuntimeException("[AnyAndroid] Échec de la mise en pause", e);
    }
  }

  private void resumeActivity(ActivityRecord rec) {
    Object thread = sApplicationThread;
    if (thread == null) return;
    try {
      Object resume = Reflect.newInstance(Reflect.type("android.app.servertransaction.ResumeActivityItem"),
          rec.token, PROCESS_STATE_TOP, false, false);
      Object transaction = Reflect.newInstance(Reflect.type("android.app.servertransaction.ClientTransaction"), thread);
      Reflect.call(transaction, "addTransactionItem", resume);
      Reflect.call(thread, "scheduleTransaction", transaction);
    } catch (Exception e) {
      throw new RuntimeException("[AnyAndroid] Échec de la reprise", e);
    }
  }

  private void stopActivity(ActivityRecord rec) {
    Object thread = sApplicationThread;
    if (thread == null) return;
    try {
      Object stop = Reflect.newInstance(Reflect.type("android.app.servertransaction.StopActivityItem"), rec.token);
      Object transaction = Reflect.newInstance(Reflect.type("android.app.servertransaction.ClientTransaction"), thread);
      Reflect.call(transaction, "addTransactionItem", stop);
      Reflect.call(thread, "scheduleTransaction", transaction);
    } catch (Exception e) {
      throw new RuntimeException("[AnyAndroid] Échec de l'arrêt", e);
    }
  }

  private void destroyActivity(ActivityRecord rec) {
    Object thread = sApplicationThread;
    if (thread == null) return;
    try {
      Object destroy = Reflect.newInstance(Reflect.type("android.app.servertransaction.DestroyActivityItem"),
          rec.token, true);
      Object transaction = Reflect.newInstance(Reflect.type("android.app.servertransaction.ClientTransaction"), thread);
      Reflect.call(transaction, "addTransactionItem", destroy);
      Reflect.call(thread, "scheduleTransaction", transaction);
    } catch (Exception e) {
      throw new RuntimeException("[AnyAndroid] Échec de la destruction", e);
    }
  }

  /** The per-activity calls of android.app.ActivityClient. */
  static final class ActivityClient extends IActivityClientController.Stub {
    private final ActivityTaskService service;

    ActivityClient(ActivityTaskService service) {
      this.service = service;
    }

    @Override
    public void setTaskDescription(IBinder token, ActivityManager.TaskDescription description) {}

    @Override
    public void activityResumed(IBinder token, boolean handleSplashScreenExit) {
      service.onActivityResumed(token);
    }

    @Override
    public boolean finishActivity(IBinder token, int resultCode, Intent resultData, int finishTask) {
      return service.doFinishActivity(token);
    }

    @Override
    public void activityPaused(IBinder token) {
      service.onActivityPaused(token);
    }

    @Override
    public void activityStopped(IBinder token, Bundle state, PersistableBundle persistentState,
        HandoffActivityData handoffActivityData, CharSequence description) {
      service.onActivityStopped(token);
    }

    @Override
    public void activityDestroyed(IBinder token) {
      service.onActivityDestroyed(token);
    }

    /**
     * ActivityClientController.onBackPressed: the task goes to the back when
     * shouldMoveTaskToBack says so, otherwise the activity finishes (requestCallbackFinish).
     */
    @Override
    public void onBackPressed(IBinder token, IRequestFinishCallback callback) {
      if (service.shouldMoveTaskToBack(token)) {
        service.moveTaskToBack(token);
        return;
      }
      if (callback != null) callback.requestFinish();  // a local callback: no RemoteException
    }
  }
}
