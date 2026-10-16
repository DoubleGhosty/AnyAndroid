package android.hardware.input;

import android.os.Binder;
import android.view.InputDevice;

public interface IInputManager {
  abstract class Stub extends Binder implements IInputManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}

    public static IInputManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  InputDevice getInputDevice(int deviceId);
  int[] getInputDeviceIds();
  int isInTabletMode();
}
