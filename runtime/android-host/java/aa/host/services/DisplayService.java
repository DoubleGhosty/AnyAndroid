package aa.host.services;

import android.hardware.display.IDisplayManager;
import android.view.Display;
import android.view.DisplayInfo;

/**
 * The "display" service (ADR-006 J4): a single built-in display (display 0), portrait
 * 1080×1920 at 420 dpi and 60 Hz, filled in as LogicalDisplay.updateLocked fills it on a phone.
 */
public final class DisplayService extends IDisplayManager.Stub {
  static final int WIDTH = 1080;
  static final int HEIGHT = 1920;
  static final int DENSITY_DPI = 420;
  private static final float REFRESH_RATE = 60f;
  private static final int MODE_ID = 1;

  public DisplayService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  @Override
  public DisplayInfo getDisplayInfo(int displayId) {
    if (displayId != Display.DEFAULT_DISPLAY) return null;
    DisplayInfo info = new DisplayInfo();
    info.type = Display.TYPE_INTERNAL;
    info.displayId = Display.DEFAULT_DISPLAY;
    info.name = "AnyAndroid";
    info.uniqueId = "local:0";
    info.logicalWidth = info.appWidth = WIDTH;
    info.logicalHeight = info.appHeight = HEIGHT;
    // Over both orientations (no system bars, so the whole display).
    info.smallestNominalAppWidth = info.smallestNominalAppHeight = Math.min(WIDTH, HEIGHT);
    info.largestNominalAppWidth = info.largestNominalAppHeight = Math.max(WIDTH, HEIGHT);
    info.modeId = info.defaultModeId = MODE_ID;
    info.userPreferredModeId = Display.Mode.INVALID_MODE_ID;
    info.supportedModes = new Display.Mode[] {new Display.Mode(MODE_ID, WIDTH, HEIGHT, REFRESH_RATE)};
    info.renderFrameRate = REFRESH_RATE;
    info.supportedColorModes = new int[] {0};  // COLOR_MODE_DEFAULT
    info.logicalDensityDpi = DENSITY_DPI;
    info.physicalXDpi = info.physicalYDpi = DENSITY_DPI;
    info.presentationDeadlineNanos = 1_000_000_000L / (long) REFRESH_RATE;
    info.state = info.committedState = Display.STATE_ON;
    info.brightnessMinimum = 0f;
    info.brightnessMaximum = 1f;
    info.brightnessDefault = 0.5f;
    info.canHostTasks = true;
    return info;
  }

  @Override
  public int[] getDisplayIds(boolean includeDisabled) {
    return new int[] {Display.DEFAULT_DISPLAY};
  }

  /** No wide color gamut: the composition's wide color space is sRGB (ColorSpace.Named.SRGB, 0). */
  @Override
  public int getPreferredWideGamutColorSpaceId() {
    return 0;
  }
}
