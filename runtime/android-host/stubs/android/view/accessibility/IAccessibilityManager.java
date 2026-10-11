package android.view.accessibility;

import android.os.Binder;

public interface IAccessibilityManager {
  abstract class Stub extends Binder implements IAccessibilityManager {
    public static IAccessibilityManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  int addClient(Object client, int userId);
}
