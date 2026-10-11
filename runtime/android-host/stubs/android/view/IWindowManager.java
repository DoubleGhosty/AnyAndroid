package android.view;

import android.os.Binder;

public interface IWindowManager {
  abstract class Stub extends Binder implements IWindowManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}

    public static IWindowManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  boolean hasNavigationBar(int displayId);
  IWindowSession openSession(IWindowSessionCallback callback);
  void getInitialDisplaySize(int displayId, android.graphics.Point size);
  void getBaseDisplaySize(int displayId, android.graphics.Point size);
  int getInitialDisplayDensity(int displayId);
  int getBaseDisplayDensity(int displayId);
}
