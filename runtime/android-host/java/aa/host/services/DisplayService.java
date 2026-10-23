package aa.host.services;

import aa.host.Reflect;
import android.content.res.Resources;
import android.graphics.Rect;
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

  private final java.util.List<android.hardware.display.IDisplayManagerCallback> callbacks = new java.util.ArrayList<>();

  /** Display events (added, changed, brightness...): the single display never changes. */
  @Override
  public void registerCallbackWithEventMask(android.hardware.display.IDisplayManagerCallback callback, long eventsMask) {
    synchronized (callbacks) {
      callbacks.add(callback);
    }
  }

  /**
   * The global configuration (the system resources' copy) with the display's, as
   * DisplayContent.computeScreenConfiguration computes it, and the window configuration of a
   * full-screen window on the display, as the window manager gives an activity.
   */
  public static Object displayConfiguration() {
    Object configuration = Reflect.newInstance(Reflect.type("android.content.res.Configuration"),
        Reflect.call(Resources.getSystem(), "getConfiguration"));
    computeScreenConfiguration(configuration);
    fillWindowConfiguration(configuration);
    return configuration;
  }

  /**
   * DisplayContent.computeScreenConfiguration and computeScreenAppConfiguration (android-17.0.0_r1)
   * for this display without decor insets, rotation 0, a touchscreen and no hardware keyboard nor
   * navigation (PhoneWindowManager.adjustConfigurationLw hides them), uiMode of a phone by day
   * (UiModeManagerService), font scale 1.
   */
  private static void computeScreenConfiguration(Object config) {
    float density = DENSITY_DPI / 160f;  // DisplayMetrics.DENSITY_DEFAULT
    int widthDp = (int) (WIDTH / density + 0.5f), heightDp = (int) (HEIGHT / density + 0.5f);
    Reflect.set(config, "screenWidthDp", widthDp);
    Reflect.set(config, "screenHeightDp", heightDp);
    Reflect.set(config, "compatScreenWidthDp", widthDp);  // mCompatibleScreenScale 1
    Reflect.set(config, "compatScreenHeightDp", heightDp);
    Reflect.set(config, "orientation", widthDp <= heightDp ? ORIENTATION_PORTRAIT : ORIENTATION_LANDSCAPE);
    // WindowContainer.computeScreenLayout on Configuration.resetScreenLayout, then not round.
    Class<?> c = Reflect.type("android.content.res.Configuration");
    int layout = (Integer) Reflect.callStatic(c, "resetScreenLayout", (Integer) Reflect.get(config, "screenLayout"));
    layout = (Integer) Reflect.callStatic(c, "reduceScreenLayout",
        layout & (SCREENLAYOUT_LONG_MASK | SCREENLAYOUT_SIZE_MASK), Math.max(widthDp, heightDp),
        Math.min(widthDp, heightDp));
    Reflect.set(config, "screenLayout", (layout & ~SCREENLAYOUT_ROUND_MASK) | SCREENLAYOUT_ROUND_NO);
    // computeSizeRanges and computeCompatSmallestWidth: the smaller side over both rotations.
    int smallestDp = (int) (Math.min(WIDTH, HEIGHT) / density + 0.5f);
    Reflect.set(config, "smallestScreenWidthDp", smallestDp);
    Reflect.set(config, "compatSmallestScreenWidthDp", smallestDp);
    Reflect.set(config, "densityDpi", DENSITY_DPI);
    Reflect.set(config, "uiMode", UI_MODE_TYPE_NORMAL | UI_MODE_NIGHT_NO);
    Reflect.set(config, "colorMode", COLOR_MODE_HDR_NO | COLOR_MODE_WIDE_COLOR_GAMUT_NO);
    Reflect.set(config, "touchscreen", TOUCHSCREEN_FINGER);
    Reflect.set(config, "keyboard", KEYBOARD_NOKEYS);
    Reflect.set(config, "navigation", NAVIGATION_NONAV);
    Reflect.set(config, "keyboardHidden", KEYBOARDHIDDEN_NO);  // a soft input method
    Reflect.set(config, "hardKeyboardHidden", HARDKEYBOARDHIDDEN_YES);
    Reflect.set(config, "navigationHidden", NAVIGATIONHIDDEN_YES);
    Reflect.set(config, "fontScale", 1f);
  }

  // android.content.res.Configuration.
  private static final int ORIENTATION_PORTRAIT = 1, ORIENTATION_LANDSCAPE = 2;
  private static final int SCREENLAYOUT_SIZE_MASK = 0x0f, SCREENLAYOUT_LONG_MASK = 0x30,
      SCREENLAYOUT_ROUND_MASK = 0x300, SCREENLAYOUT_ROUND_NO = 0x100;
  private static final int UI_MODE_TYPE_NORMAL = 0x01, UI_MODE_NIGHT_NO = 0x10;
  private static final int COLOR_MODE_WIDE_COLOR_GAMUT_NO = 0x1, COLOR_MODE_HDR_NO = 0x4;
  private static final int TOUCHSCREEN_FINGER = 3, KEYBOARD_NOKEYS = 1, NAVIGATION_NONAV = 1;
  private static final int KEYBOARDHIDDEN_NO = 1, HARDKEYBOARDHIDDEN_YES = 2, NAVIGATIONHIDDEN_YES = 2;

  /** Bounds, application and maximum bounds of the display, full screen, standard activity, no rotation. */
  public static void fillWindowConfiguration(Object configuration) {
    Rect display = new Rect(0, 0, WIDTH, HEIGHT);
    Object window = Reflect.get(configuration, "windowConfiguration");
    Reflect.call(window, "setBounds", display);
    Reflect.call(window, "setAppBounds", display);
    Reflect.call(window, "setMaxBounds", display);
    Reflect.call(window, "setWindowingMode", WINDOWING_MODE_FULLSCREEN);
    Reflect.call(window, "setActivityType", ACTIVITY_TYPE_STANDARD);
    Reflect.call(window, "setRotation", 0);
    Reflect.call(window, "setDisplayRotation", 0);
  }

  /** WindowConfiguration.WINDOWING_MODE_FULLSCREEN and ACTIVITY_TYPE_STANDARD. */
  private static final int WINDOWING_MODE_FULLSCREEN = 1, ACTIVITY_TYPE_STANDARD = 1;
}
