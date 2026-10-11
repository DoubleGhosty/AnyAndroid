package android.view;

import android.os.Binder;

public interface IWindowManager {
  abstract class Stub extends Binder implements IWindowManager {
    public static IWindowManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  boolean hasNavigationBar(int displayId);
  IWindowSession openSession(IWindowSessionCallback callback);
  int getInitialDisplaySize(int displayId, android.graphics.Point size);
  int getBaseDisplayDensity(int displayId);
  void getInitialDisplayDensity(int displayId, android.util.MutableInt density);
  void getStableInsets(int displayId, android.graphics.Rect outInsets);
}
