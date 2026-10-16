package android.app;

import android.os.Binder;
import android.os.IBinder;

public interface IActivityClientController {
  abstract class Stub extends Binder implements IActivityClientController {
    public Stub() {}

    public static IActivityClientController asInterface(IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  void setTaskDescription(IBinder token, ActivityManager.TaskDescription description);

  void activityResumed(IBinder token, boolean handleSplashScreenExit);
}
