package aa.host.services;

import android.view.accessibility.IAccessibilityManager;
import android.view.accessibility.IAccessibilityManagerClient;

/** The "accessibility" service (ADR-006 J4): no accessibility service is ever enabled. */
public final class AccessibilityService extends IAccessibilityManager.Stub {
  public AccessibilityService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  /** The client's state (low half) and relevant event types (high half): none. */
  @Override
  public long addClient(IAccessibilityManagerClient client, int userId) {
    return 0L;
  }
}
