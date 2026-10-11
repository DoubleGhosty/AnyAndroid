package aa.host.services;

import android.graphics.Point;
import android.view.Display;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.IWindowSessionCallback;

/**
 * The "window" service (ADR-006 J4): what the application side asks the window manager about
 * the display (the one of {@link DisplayService}, without system bars) and its window session.
 */
public final class WindowService extends IWindowManager.Stub {
  public WindowService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  @Override
  public boolean hasNavigationBar(int displayId) {
    return false;
  }

  @Override
  public IWindowSession openSession(IWindowSessionCallback callback) {
    return new WindowSession();
  }

  @Override
  public void getInitialDisplaySize(int displayId, Point size) {
    if (displayId == Display.DEFAULT_DISPLAY) size.set(DisplayService.WIDTH, DisplayService.HEIGHT);
  }

  @Override
  public void getBaseDisplaySize(int displayId, Point size) {
    getInitialDisplaySize(displayId, size);
  }

  @Override
  public int getInitialDisplayDensity(int displayId) {
    return displayId == Display.DEFAULT_DISPLAY ? DisplayService.DENSITY_DPI : -1;
  }

  @Override
  public int getBaseDisplayDensity(int displayId) {
    return getInitialDisplayDensity(displayId);
  }

  /** The application's window session: windows are not added yet (J6). */
  private static final class WindowSession extends IWindowSession.Stub {}
}
