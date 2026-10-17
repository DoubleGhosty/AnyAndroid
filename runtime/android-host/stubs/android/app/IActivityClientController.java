package android.app;

import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;

public interface IActivityClientController {
  abstract class Stub extends Binder implements IActivityClientController {
    public Stub() {}

    public static IActivityClientController asInterface(IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  void setTaskDescription(IBinder token, ActivityManager.TaskDescription description);

  void activityResumed(IBinder token, boolean handleSplashScreenExit);

  boolean finishActivity(IBinder token, int resultCode, Intent resultData, int finishTask);

  void activityPaused(IBinder token);

  void activityStopped(IBinder token, Bundle state, PersistableBundle persistentState, CharSequence description);

  void activityDestroyed(IBinder token);

  void onBackPressed(IBinder token, IRequestFinishCallback callback);
}
