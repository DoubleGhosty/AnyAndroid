package aa.host.services;

import com.android.internal.view.IInputMethodManager;

/**
 * The "input_method" service (ADR-006 J4): minimal stub for InputMethodManager.
 * No soft keyboard yet, returns neutral values.
 */
public final class InputMethodService extends IInputMethodManager.Stub {
  public InputMethodService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }
}
