package aa.host.services;

import android.view.accessibility.IAccessibilityManager;

/**
 * The "accessibility" service (ADR-006 J4): minimal stub for AccessibilityManager.
 * No accessibility features yet, returns neutral values.
 */
public final class AccessibilityService extends IAccessibilityManager.Stub {
  public AccessibilityService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  @Override
  public int addClient(Object client, int userId) {
    // Return a state indicating no accessibility services are enabled.
    return 0;
  }
}
