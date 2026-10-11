package android.app;

import android.os.Binder;

public interface IActivityTaskManager {
  abstract class Stub extends Binder implements IActivityTaskManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}

    public static IActivityTaskManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  IActivityClientController getActivityClientController();
}
