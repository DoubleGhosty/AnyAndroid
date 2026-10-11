package aa.host.services;

import android.hardware.display.DisplayInfo;
import android.hardware.display.IDisplayManager;

/**
 * The "display" service (ADR-006 J4): reports a single logical display (display 0).
 * 1080×1920 at 420 DPI, 60 Hz refresh, portrait orientation.
 */
public final class DisplayService extends IDisplayManager.Stub {
  private static final int DISPLAY_WIDTH = 1080;
  private static final int DISPLAY_HEIGHT = 1920;
  private static final int DISPLAY_DENSITY = 420;
  private static final float REFRESH_RATE = 60.0f;

  private static final int DISPLAY_STATE_ON = 2;
  private static final int SURFACE_ROTATION_0 = 0;

  public DisplayService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  @Override
  public DisplayInfo getDisplayInfo(int displayId) {
    if (displayId != 0) return null;

    DisplayInfo info = new DisplayInfo();
    info.displayId = 0;
    info.type = DisplayInfo.TYPE_INTERNAL;
    info.name = "AnyAndroid Display";
    info.logicalWidth = DISPLAY_WIDTH;
    info.logicalHeight = DISPLAY_HEIGHT;
    info.logicalDensityDpi = DISPLAY_DENSITY;
    info.refreshRate = REFRESH_RATE;
    info.rotation = SURFACE_ROTATION_0;
    info.state = DISPLAY_STATE_ON;
    return info;
  }

  @Override
  public int[] getDisplayIds(boolean includeDisabled) {
    return new int[] { 0 };
  }
}
