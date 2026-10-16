package aa.host.services;

import android.content.res.Resources;
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

  /** No user-set timeouts: interactive and non-interactive both 0 (an IntPair). */
  @Override
  public long getRecommendedTimeoutMillis() {
    return 0L;
  }

  /** AccessibilityUserState's default focus highlight, from the framework's resources. */
  @Override
  public int getFocusStrokeWidth() {
    Resources res = Resources.getSystem();
    return res.getDimensionPixelSize(res.getIdentifier("accessibility_focus_highlight_stroke_width", "dimen", "android"));
  }

  @Override
  public int getFocusColor() {
    Resources res = Resources.getSystem();
    return res.getColor(res.getIdentifier("accessibility_focus_highlight_color", "color", "android"));
  }
}
