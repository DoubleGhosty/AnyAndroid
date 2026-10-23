package aa.host.services;

/**
 * The window manager's policy for the order of windows (WindowManagerPolicy of android-17.0.0_r1):
 * the layer of a window by its type, the sub-layer of a sub-window.
 */
final class WindowPolicy {
  private WindowPolicy() {}

  // WindowManager.LayoutParams types.
  static final int FIRST_APPLICATION_WINDOW = 1, LAST_APPLICATION_WINDOW = 99;
  static final int FIRST_SUB_WINDOW = 1000, LAST_SUB_WINDOW = 1999;
  private static final int TYPE_APPLICATION_PANEL = 1000, TYPE_APPLICATION_MEDIA = 1001,
      TYPE_APPLICATION_SUB_PANEL = 1002, TYPE_APPLICATION_ATTACHED_DIALOG = 1003,
      TYPE_APPLICATION_MEDIA_OVERLAY = 1004, TYPE_APPLICATION_ABOVE_SUB_PANEL = 1005,
      TYPE_APPLICATION_CAPTION_BAR = 1006;
  private static final int TYPE_STATUS_BAR = 2000, TYPE_SEARCH_BAR = 2001, TYPE_PHONE = 2002,
      TYPE_SYSTEM_ALERT = 2003, TYPE_TOAST = 2005, TYPE_SYSTEM_OVERLAY = 2006, TYPE_PRIORITY_PHONE = 2007,
      TYPE_SYSTEM_DIALOG = 2008, TYPE_KEYGUARD_DIALOG = 2009, TYPE_SYSTEM_ERROR = 2010, TYPE_INPUT_METHOD = 2011,
      TYPE_INPUT_METHOD_DIALOG = 2012, TYPE_WALLPAPER = 2013, TYPE_SECURE_SYSTEM_OVERLAY = 2015, TYPE_DRAG = 2016,
      TYPE_STATUS_BAR_SUB_PANEL = 2017, TYPE_POINTER = 2018, TYPE_NAVIGATION_BAR = 2019,
      TYPE_VOLUME_OVERLAY = 2020, TYPE_BOOT_PROGRESS = 2021, TYPE_INPUT_CONSUMER = 2022,
      TYPE_NAVIGATION_BAR_PANEL = 2024, TYPE_DISPLAY_OVERLAY = 2026, TYPE_MAGNIFICATION_OVERLAY = 2027,
      TYPE_PRIVATE_PRESENTATION = 2030, TYPE_VOICE_INTERACTION = 2031, TYPE_ACCESSIBILITY_OVERLAY = 2032,
      TYPE_VOICE_INTERACTION_STARTING = 2033, TYPE_DOCK_DIVIDER = 2034, TYPE_QS_DIALOG = 2035,
      TYPE_SCREENSHOT = 2036, TYPE_PRESENTATION = 2037, TYPE_APPLICATION_OVERLAY = 2038,
      TYPE_ACCESSIBILITY_MAGNIFICATION_OVERLAY = 2039, TYPE_NOTIFICATION_SHADE = 2040,
      TYPE_STATUS_BAR_ADDITIONAL = 2041;

  // WindowManagerPolicyConstants.
  static final int APPLICATION_LAYER = 2;
  private static final int APPLICATION_MEDIA_SUBLAYER = -2, APPLICATION_MEDIA_OVERLAY_SUBLAYER = -1,
      APPLICATION_PANEL_SUBLAYER = 1, APPLICATION_SUB_PANEL_SUBLAYER = 2, APPLICATION_ABOVE_SUB_PANEL_SUBLAYER = 3;

  static boolean isSubWindow(int type) {
    return type >= FIRST_SUB_WINDOW && type <= LAST_SUB_WINDOW;
  }

  /**
   * getWindowLayerFromTypeLw(type, canAddInternalSystemWindow, false): the application's
   * process adds every window here, none with INTERNAL_SYSTEM_WINDOW.
   */
  static int windowLayer(int type, boolean canAddInternalSystemWindow) {
    if (type >= FIRST_APPLICATION_WINDOW && type <= LAST_APPLICATION_WINDOW) return APPLICATION_LAYER;
    switch (type) {
      case TYPE_WALLPAPER:
        return 1;
      case TYPE_PRESENTATION:
      case TYPE_PRIVATE_PRESENTATION:
      case TYPE_DOCK_DIVIDER:
      case TYPE_QS_DIALOG:
      case TYPE_PHONE:
        return 3;
      case TYPE_SEARCH_BAR:
        return 4;
      case TYPE_INPUT_CONSUMER:
        return 5;
      case TYPE_SYSTEM_DIALOG:
        return 6;
      case TYPE_TOAST:
        return canAddInternalSystemWindow ? 27 : 7;
      case TYPE_PRIORITY_PHONE:
        return 8;
      case TYPE_SYSTEM_ALERT:
        return canAddInternalSystemWindow ? 12 : 9;
      case TYPE_APPLICATION_OVERLAY:
        return 11;
      case TYPE_INPUT_METHOD:
        return 13;
      case TYPE_INPUT_METHOD_DIALOG:
        return 14;
      case TYPE_STATUS_BAR:
        return 15;
      case TYPE_STATUS_BAR_ADDITIONAL:
        return 16;
      case TYPE_NOTIFICATION_SHADE:
        return 17;
      case TYPE_STATUS_BAR_SUB_PANEL:
        return 18;
      case TYPE_KEYGUARD_DIALOG:
        return 19;
      case TYPE_VOICE_INTERACTION_STARTING:
        return 20;
      case TYPE_VOICE_INTERACTION:
        return 21;
      case TYPE_VOLUME_OVERLAY:
        return 22;
      case TYPE_SYSTEM_OVERLAY:
        return canAddInternalSystemWindow ? 23 : 10;
      case TYPE_NAVIGATION_BAR:
        return 24;
      case TYPE_NAVIGATION_BAR_PANEL:
        return 25;
      case TYPE_SCREENSHOT:
        return 26;
      case TYPE_SYSTEM_ERROR:
        return canAddInternalSystemWindow ? 27 : 9;
      case TYPE_MAGNIFICATION_OVERLAY:
        return 28;
      case TYPE_DISPLAY_OVERLAY:
        return 29;
      case TYPE_DRAG:
        return 30;
      case TYPE_ACCESSIBILITY_OVERLAY:
        return 31;
      case TYPE_ACCESSIBILITY_MAGNIFICATION_OVERLAY:
        return 32;
      case TYPE_SECURE_SYSTEM_OVERLAY:
        return 33;
      case TYPE_BOOT_PROGRESS:
        return 34;
      case TYPE_POINTER:
        return 35;
      default:
        return 3;
    }
  }

  /** getSubWindowLayerFromTypeLw: below (media) or above the parent window's own surface. */
  static int subWindowLayer(int type) {
    switch (type) {
      case TYPE_APPLICATION_PANEL:
      case TYPE_APPLICATION_CAPTION_BAR:
      case TYPE_APPLICATION_ATTACHED_DIALOG:
        return APPLICATION_PANEL_SUBLAYER;
      case TYPE_APPLICATION_MEDIA:
        return APPLICATION_MEDIA_SUBLAYER;
      case TYPE_APPLICATION_MEDIA_OVERLAY:
        return APPLICATION_MEDIA_OVERLAY_SUBLAYER;
      case TYPE_APPLICATION_SUB_PANEL:
        return APPLICATION_SUB_PANEL_SUBLAYER;
      case TYPE_APPLICATION_ABOVE_SUB_PANEL:
        return APPLICATION_ABOVE_SUB_PANEL_SUBLAYER;
      default:
        return 0;
    }
  }
}
