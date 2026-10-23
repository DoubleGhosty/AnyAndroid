package aa.host.services;

import android.graphics.Rect;
import android.os.IBinder;
import android.view.IWindow;
import android.view.InputChannel;
import android.view.WindowManager;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The window manager's input windows (ADR-006 J8): what WindowManagerService's InputMonitor
 * tells the input dispatcher about the windows. Each window added by {@link WindowService} keeps
 * the server end of its input channel here, with its type, flags, frame and visibility; the list
 * goes to the bridge's input dispatcher from the topmost window down, the z-order being the
 * policy's layer of the window type (WindowManagerPolicy.getWindowLayerFromTypeLw), then the
 * order of addition, sub-windows next to their parent. The window manager's choices are kept
 * simple: a touch-modal window (neither FLAG_NOT_TOUCH_MODAL nor FLAG_NOT_FOCUSABLE) takes the
 * touches of the whole display, a window can receive keys when it is visible and focusable.
 */
public final class InputWindows {
  private InputWindows() {}

  /** InputMonitor.updateInputWindowsLw: the windows from the top, 12 values each. */
  private static native void nativeSetInputWindows(InputChannel[] channels, String[] names, int[] values);

  /** InputManagerService.removeInputChannel. */
  private static native void nativeRemoveInputChannel(InputChannel channel);

  /** WindowManager.LayoutParams flags and View.VISIBLE. */
  private static final int FLAG_NOT_FOCUSABLE = 0x8, FLAG_NOT_TOUCH_MODAL = 0x20, VISIBLE = 0;

  /** WindowManagerPolicy.APPLICATION_LAYER. */
  private static final int APPLICATION_LAYER = 2;

  private static final class Window {
    IBinder client;
    InputChannel channel;
    IBinder parentToken;  // LayoutParams.token: the parent window's client for a sub-window
    String title = "";
    int type;
    int flags;
    int visibility;
    final Rect frame = new Rect();
    long order;
  }

  private static final Map<IBinder, Window> windows = new LinkedHashMap<>();
  private static long nextOrder;

  /** WindowManagerService.addWindow: the window and the server end of its input channel. */
  public static synchronized void add(IWindow client, WindowManager.LayoutParams attrs, int visibility,
      InputChannel serverChannel, Rect frame) {
    Window w = new Window();
    w.client = client.asBinder();
    w.channel = serverChannel;
    w.order = nextOrder++;
    apply(w, attrs, visibility, frame);
    windows.put(w.client, w);
    publish();
  }

  /** relayout: new attributes (null when unchanged), visibility and frame (null to keep it). */
  public static synchronized void update(IWindow client, WindowManager.LayoutParams attrs, int visibility,
      Rect frame) {
    Window w = windows.get(client.asBinder());
    if (w == null) return;
    apply(w, attrs, visibility, frame);
    publish();
  }

  /** WindowManagerService.removeWindow: the channel is unregistered, then disposed. */
  public static synchronized void remove(IBinder client) {
    Window w = windows.remove(client);
    if (w == null) return;
    nativeRemoveInputChannel(w.channel);
    w.channel.dispose();
    publish();
  }

  private static void apply(Window w, WindowManager.LayoutParams attrs, int visibility, Rect frame) {
    if (attrs != null) {
      w.type = attrs.type;
      w.flags = attrs.flags;
      w.parentToken = attrs.token;
      CharSequence title = attrs.getTitle();
      w.title = title != null ? title.toString() : "";
    }
    w.visibility = visibility;
    if (frame != null) w.frame.set(frame);
  }

  /** The windows from the bottom up: top-level windows by layer then order, sub-windows around. */
  private static List<Window> zOrder() {
    List<Window> top = new ArrayList<>();
    Map<IBinder, List<Window>> children = new LinkedHashMap<>();
    for (Window w : windows.values()) {
      Window parent = isSubWindow(w.type) && w.parentToken != null ? windows.get(w.parentToken) : null;
      if (parent != null) {
        children.computeIfAbsent(parent.client, k -> new ArrayList<>()).add(w);
      } else {
        top.add(w);
      }
    }
    top.sort(Comparator.<Window>comparingInt(w -> layer(w.type)).thenComparingLong(w -> w.order));
    List<Window> out = new ArrayList<>();
    for (Window w : top) {
      List<Window> kids = children.getOrDefault(w.client, new ArrayList<>());
      kids.sort(Comparator.<Window>comparingInt(k -> subLayer(k.type)).thenComparingLong(k -> k.order));
      for (Window k : kids) if (subLayer(k.type) < 0) out.add(k);
      out.add(w);
      for (Window k : kids) if (subLayer(k.type) >= 0) out.add(k);
    }
    return out;
  }

  private static void publish() {
    List<Window> order = zOrder();
    int n = order.size();
    InputChannel[] channels = new InputChannel[n];
    String[] names = new String[n];
    int[] values = new int[n * 12];
    int width = DisplayService.WIDTH, height = DisplayService.HEIGHT;
    for (int i = 0; i < n; i++) {
      Window w = order.get(n - 1 - i);  // from the top
      channels[i] = w.channel;
      names[i] = w.title;
      boolean visible = w.visibility == VISIBLE;
      // WindowState.getSurfaceTouchableRegion: a touch-modal window takes the whole display.
      boolean modal = (w.flags & (FLAG_NOT_TOUCH_MODAL | FLAG_NOT_FOCUSABLE)) == 0;
      Rect touchable = modal ? new Rect(0, 0, width, height) : w.frame;
      int[] v = {w.type, w.flags, w.frame.left, w.frame.top, w.frame.right, w.frame.bottom, touchable.left,
          touchable.top, touchable.right, touchable.bottom, visible ? 1 : 0,
          visible && (w.flags & FLAG_NOT_FOCUSABLE) == 0 ? 1 : 0};
      System.arraycopy(v, 0, values, i * 12, 12);
    }
    nativeSetInputWindows(channels, names, values);
  }

  private static boolean isSubWindow(int type) {
    return type >= 1000 && type <= 1999;  // FIRST_SUB_WINDOW, LAST_SUB_WINDOW
  }

  /** WindowManagerPolicy.getWindowLayerFromTypeLw, for an application's windows. */
  static int layer(int type) {
    if (type >= 1 && type <= 99) return APPLICATION_LAYER;  // application windows
    if (isSubWindow(type)) return APPLICATION_LAYER;  // without its parent
    switch (type) {
      case 2013: return 1;  // TYPE_WALLPAPER
      case 2037: case 2030: case 2034: case 2035: case 2002: return 3;  // presentations, divider, QS, phone
      case 2001: return 4;  // TYPE_SEARCH_BAR
      case 2022: return 5;  // TYPE_INPUT_CONSUMER
      case 2008: return 6;  // TYPE_SYSTEM_DIALOG
      case 2005: return 7;  // TYPE_TOAST of an application (27 when the system adds it)
      case 2007: return 8;  // TYPE_PRIORITY_PHONE
      case 2003: return 9;  // TYPE_SYSTEM_ALERT
      case 2038: return 11;  // TYPE_APPLICATION_OVERLAY
      case 2011: return 13;  // TYPE_INPUT_METHOD
      case 2012: return 14;  // TYPE_INPUT_METHOD_DIALOG
      case 2000: return 15;  // TYPE_STATUS_BAR
      case 2041: return 16;  // TYPE_STATUS_BAR_ADDITIONAL
      case 2040: return 17;  // TYPE_NOTIFICATION_SHADE
      case 2017: return 18;  // TYPE_STATUS_BAR_SUB_PANEL
      case 2009: return 19;  // TYPE_KEYGUARD_DIALOG
      case 2033: return 20;  // TYPE_VOICE_INTERACTION_STARTING
      case 2031: return 21;  // TYPE_VOICE_INTERACTION
      case 2020: return 22;  // TYPE_VOLUME_OVERLAY
      case 2006: return 10;  // TYPE_SYSTEM_OVERLAY of an application
      case 2019: return 24;  // TYPE_NAVIGATION_BAR
      case 2024: return 25;  // TYPE_NAVIGATION_BAR_PANEL
      case 2036: return 26;  // TYPE_SCREENSHOT
      case 2010: return 9;  // TYPE_SYSTEM_ERROR of an application
      case 2027: return 28;  // TYPE_MAGNIFICATION_OVERLAY
      case 2026: return 29;  // TYPE_DISPLAY_OVERLAY
      case 2016: return 30;  // TYPE_DRAG
      case 2032: return 31;  // TYPE_ACCESSIBILITY_OVERLAY
      case 2039: return 32;  // TYPE_ACCESSIBILITY_MAGNIFICATION_OVERLAY
      case 2015: return 33;  // TYPE_SECURE_SYSTEM_OVERLAY
      case 2021: return 34;  // TYPE_BOOT_PROGRESS
      case 2018: return 35;  // TYPE_POINTER
      default: return 3;  // unknown type
    }
  }

  /** WindowManagerPolicy.getSubWindowLayerFromTypeLw: above (> 0) or below its parent. */
  static int subLayer(int type) {
    switch (type) {
      case 1001: return -2;  // TYPE_APPLICATION_MEDIA
      case 1004: return -1;  // TYPE_APPLICATION_MEDIA_OVERLAY
      case 1002: return 2;  // TYPE_APPLICATION_SUB_PANEL
      case 1005: return 3;  // TYPE_APPLICATION_ABOVE_SUB_PANEL
      default: return 1;  // TYPE_APPLICATION_PANEL, TYPE_APPLICATION_ATTACHED_DIALOG
    }
  }
}
