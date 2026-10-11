package android.hardware.display;

import android.os.Binder;
import android.view.DisplayInfo;

public interface IDisplayManager {
  abstract class Stub extends Binder implements IDisplayManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}

    public static IDisplayManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  DisplayInfo getDisplayInfo(int displayId);
  int[] getDisplayIds(boolean includeDisabled);
}
