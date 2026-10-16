package aa.host.services;

import android.hardware.input.IInputManager;
import android.view.InputDevice;
import android.view.KeyCharacterMap;

/**
 * The "input" service (ADR-006 J8): the input devices of the emulated phone, as system_server's
 * InputReader reports them. For now the virtual keyboard (id -1, always present on Android), with
 * the key character map {@code Virtual.kcm} of the system image; the window's mouse and keyboard
 * reach the application through its InputChannel.
 */
public final class InputService extends IInputManager.Stub {
  private static final String VIRTUAL_KEYBOARD_KCM = "/system/usr/keychars/Virtual.kcm";

  private InputDevice virtualKeyboard;

  public InputService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  /** InputReader's map loading (EventHub, KeyCharacterMap::load), done by the bridge. */
  private static native KeyCharacterMap nativeLoadKeyCharacterMap(int deviceId, String path);

  @Override
  public synchronized InputDevice getInputDevice(int deviceId) {
    if (deviceId != KeyCharacterMap.VIRTUAL_KEYBOARD) {
      return null;
    }
    if (virtualKeyboard == null) {
      virtualKeyboard = new InputDevice.Builder()
          .setId(KeyCharacterMap.VIRTUAL_KEYBOARD)
          .setName("Virtual")
          .setDescriptor("<virtual>")
          .setSources(InputDevice.SOURCE_KEYBOARD)
          .setKeyboardType(InputDevice.KEYBOARD_TYPE_ALPHABETIC)
          .setKeyCharacterMap(nativeLoadKeyCharacterMap(KeyCharacterMap.VIRTUAL_KEYBOARD, VIRTUAL_KEYBOARD_KCM))
          .setIsVirtualDevice(true)
          .build();
    }
    return virtualKeyboard;
  }

  @Override
  public int[] getInputDeviceIds() {
    return new int[] {KeyCharacterMap.VIRTUAL_KEYBOARD};
  }

  /** InputManager.SWITCH_STATE_OFF: a phone, not a tablet folded over its keyboard. */
  @Override
  public int isInTabletMode() {
    return 0;
  }
}
