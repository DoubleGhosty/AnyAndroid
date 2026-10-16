package android.view.accessibility;

import android.os.Binder;

public interface IAccessibilityManager {
  abstract class Stub extends Binder implements IAccessibilityManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}

    public static IAccessibilityManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  long addClient(IAccessibilityManagerClient client, int userId);
  long getRecommendedTimeoutMillis();
  int getFocusStrokeWidth();
  int getFocusColor();
}
