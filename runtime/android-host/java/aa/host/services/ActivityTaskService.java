package aa.host.services;

import android.app.ActivityManager;
import android.app.IActivityClientController;
import android.app.IActivityTaskManager;
import android.os.IBinder;

/**
 * The "activity_task" service (ADR-006 J4): the activity task manager. There are no tasks or
 * recents screen: the activities of the single application are started by the launcher, and
 * what they report to the system about themselves goes to {@link ActivityClient}.
 */
public final class ActivityTaskService extends IActivityTaskManager.Stub {
  private final ActivityClient client = new ActivityClient();

  public ActivityTaskService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  @Override
  public IActivityClientController getActivityClientController() {
    return client;
  }

  /** The per-activity calls of android.app.ActivityClient. */
  static final class ActivityClient extends IActivityClientController.Stub {
    /** Shown in recents, which do not exist. */
    @Override
    public void setTaskDescription(IBinder token, ActivityManager.TaskDescription description) {}

    /** ResumeActivityItem.postExecute: the activity is in the foreground. */
    @Override
    public void activityResumed(IBinder token, boolean handleSplashScreenExit) {
      Runnable r = onResumed;
      if (r != null) r.run();
    }
  }

  /** Called when an activity reports itself resumed (the host's launcher watches it). */
  public static volatile Runnable onResumed;
}
