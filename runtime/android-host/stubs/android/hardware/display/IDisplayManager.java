package android.hardware.display;

import android.os.Binder;

public interface IDisplayManager {
  abstract class Stub extends Binder implements IDisplayManager {
    public static IDisplayManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  DisplayInfo getDisplayInfo(int displayId);
  int[] getDisplayIds(boolean includeDisabled);
}
