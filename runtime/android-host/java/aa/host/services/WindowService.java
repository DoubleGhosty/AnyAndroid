package aa.host.services;

import aa.host.SystemServer;
import android.graphics.Point;
import android.graphics.Rect;
import android.util.MutableInt;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.IWindowSessionCallback;

/**
 * The "window" service (ADR-006 J4): minimal window manager for the direct launcher.
 * Reports a logical 1080×1920 display at 420 DPI (xxhdpi), no navigation bar.
 */
public final class WindowService extends IWindowManager.Stub {
  /** Logical display size: 1080×1920 (portrait phone, common density bucket). */
  private static final int DISPLAY_WIDTH = 1080;
  private static final int DISPLAY_HEIGHT = 1920;
  /** Display density: 420 DPI (xxhdpi). */
  private static final int DISPLAY_DENSITY = 420;

  public WindowService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  @Override
  public boolean hasNavigationBar(int displayId) {
    // No navigation bar in our logical display (full screen).
    return false;
  }

  @Override
  public IWindowSession openSession(IWindowSessionCallback callback) {
    // Return a session stub. The real session will be needed for J5+ when windows are added.
    return new WindowSessionService();
  }

  @Override
  public int getInitialDisplaySize(int displayId, Point size) {
    if (displayId != 0) return -1;  // Only display 0 exists
    size.x = DISPLAY_WIDTH;
    size.y = DISPLAY_HEIGHT;
    return 0;
  }

  @Override
  public int getBaseDisplayDensity(int displayId) {
    return displayId == 0 ? DISPLAY_DENSITY : -1;
  }

  @Override
  public void getInitialDisplayDensity(int displayId, MutableInt density) {
    if (displayId == 0) {
      density.value = DISPLAY_DENSITY;
    }
  }

  @Override
  public void getStableInsets(int displayId, Rect outInsets) {
    // No insets (status bar, navigation bar) for now.
    if (displayId == 0) {
      outInsets.left = 0;
      outInsets.top = 0;
      outInsets.right = 0;
      outInsets.bottom = 0;
    }
  }

  /** Window session: placeholder for J4, will handle addToDisplay/relayout in J5+. */
  private static final class WindowSessionService extends IWindowSession.Stub {
    WindowSessionService() {
      super(LocalPermissionEnforcer.INSTANCE);
    }
  }
}
