package aa.host.services;

import aa.host.Reflect;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.IBinder;
import android.view.Display;
import android.view.IWindow;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.IWindowSessionCallback;
import android.view.InputChannel;
import android.view.SurfaceControl;
import android.view.WindowManager;
import android.view.WindowRelayoutResult;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The "window" service (ADR-006 J4, J9): what the application side asks the window manager about
 * the display (the one of {@link DisplayService}, without system bars) and its window session.
 * Tracks windows, computes bounds with Gravity for dialogs and popups, and manages their input channels.
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

  /**
   * The application's window session (ADR-006 J5, J9). Manages the z-order, geometry
   * and input channel pairs for both the main activity and secondary windows (dialogs, popups).
   */
  private static final class WindowSession extends IWindowSession.Stub {
    private static final class WindowRecord {
      final IWindow window;
      final WindowManager.LayoutParams attrs = new WindowManager.LayoutParams();
      final Rect frame = new Rect();
      InputChannel serverChannel;

      WindowRecord(IWindow window, WindowManager.LayoutParams a) {
        this.window = window;
        if (a != null) this.attrs.copyFrom(a);
      }
    }

    private final Map<IBinder, WindowRecord> windows = new HashMap<>();
    private final List<InputChannel> serverChannels = new ArrayList<>();

    @Override
    public int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility, int displayId,
        int userId, int requestedVisibleTypes, InputChannel outInputChannel, WindowRelayoutResult outResult) {
      if (displayId != Display.DEFAULT_DISPLAY) return -9;  // WindowManagerGlobal.ADD_INVALID_DISPLAY
      WindowRecord rec = new WindowRecord(window, attrs);
      synchronized (windows) {
        windows.put(window.asBinder(), rec);
      }
      fillFrames(rec, attrs, 0, 0, outResult);
      if (outInputChannel != null) {
        InputChannel[] pair = InputChannel.openInputChannelPair(String.valueOf(window));
        rec.serverChannel = pair[0];
        synchronized (serverChannels) {
          serverChannels.add(pair[0]);
        }
        pair[1].copyTo(outInputChannel);
      }
      return ADD_FLAG_IN_TOUCH_MODE | ADD_FLAG_APP_VISIBLE;  // ADD_OKAY (0) with these flags
    }

    @Override
    public int relayout(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth, int requestedHeight,
        int viewVisibility, int flags, int seq, int lastSyncSeqId, SurfaceControl outSurfaceControl,
        WindowRelayoutResult outRelayoutResult) {
      WindowRecord rec;
      synchronized (windows) {
        rec = window != null ? windows.get(window.asBinder()) : null;
      }
      if (attrs != null && rec != null) {
        rec.attrs.copyFrom(attrs);
      }
      if (outRelayoutResult != null) {
        fillFrames(rec, attrs, requestedWidth, requestedHeight, outRelayoutResult);
      }
      return 0;
    }

    @Override
    public void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth,
        int requestedHeight, int viewVisibility, int flags, int seq, int lastSyncSeqId,
        SurfaceControl outSurfaceControl) {
      relayout(window, attrs, requestedWidth, requestedHeight, viewVisibility, flags, seq, lastSyncSeqId, outSurfaceControl, null);
    }

    @Override
    public void remove(IWindow window) {
      if (window == null) return;
      WindowRecord rec;
      synchronized (windows) {
        rec = windows.remove(window.asBinder());
      }
      if (rec != null && rec.serverChannel != null) {
        try {
          rec.serverChannel.dispose();
        } catch (Throwable ignored) {}
      }
    }
  }

  /** WindowManagerGlobal.ADD_FLAG_IN_TOUCH_MODE (phones start in touch mode), ADD_FLAG_APP_VISIBLE. */
  private static final int ADD_FLAG_IN_TOUCH_MODE = 1, ADD_FLAG_APP_VISIBLE = 2;

  /** Computes frames according to window type, gravity and dimensions. */
  static void fillFrames(WindowSession.WindowRecord rec, WindowManager.LayoutParams attrs,
      int requestedWidth, int requestedHeight, WindowRelayoutResult out) {
    Rect display = new Rect(0, 0, DisplayService.WIDTH, DisplayService.HEIGHT);
    WindowManager.LayoutParams lp = (attrs != null) ? attrs : (rec != null ? rec.attrs : null);
    int type = (lp != null) ? lp.type : 1; // 1 = TYPE_BASE_APPLICATION

    Rect frameRect;
    if (type == 1 || type == 2) {
      // Full-screen activity window
      frameRect = display;
    } else {
      // Dialog, popup, or panel: calculate frame from requested size or layout params
      int w = (lp != null && lp.width > 0) ? lp.width : (requestedWidth > 0 ? requestedWidth : Math.min(840, DisplayService.WIDTH - 80));
      int h = (lp != null && lp.height > 0) ? lp.height : (requestedHeight > 0 ? requestedHeight : Math.min(600, DisplayService.HEIGHT - 200));
      int x = (lp != null) ? lp.x : 0;
      int y = (lp != null) ? lp.y : 0;
      int gravity = (lp != null && lp.gravity != 0) ? lp.gravity : 17; // Gravity.CENTER
      int left, top;
      int hGrav = gravity & 7; // Gravity.HORIZONTAL_GRAVITY_MASK
      if (hGrav == 3) { // LEFT
        left = x;
      } else if (hGrav == 5) { // RIGHT
        left = DisplayService.WIDTH - w - x;
      } else { // CENTER_HORIZONTAL
        left = (DisplayService.WIDTH - w) / 2 + x;
      }
      int vGrav = gravity & 112; // Gravity.VERTICAL_GRAVITY_MASK
      if (vGrav == 48) { // TOP
        top = y;
      } else if (vGrav == 80) { // BOTTOM
        top = DisplayService.HEIGHT - h - y;
      } else { // CENTER_VERTICAL
        top = (DisplayService.HEIGHT - h) / 2 + y;
      }
      frameRect = new Rect(left, top, left + w, top + h);
    }

    if (rec != null) rec.frame.set(frameRect);
    out.frames.frame.set(frameRect);
    out.frames.displayFrame.set(display);
    out.frames.parentFrame.set(display);
    out.frames.attachedFrame = null;
    out.frames.compatScale = 1f;
    out.insetsState.setDisplayFrame(display);
    Object override = Reflect.newInstance(Reflect.type("android.content.res.Configuration"));
    DisplayService.fillWindowConfiguration(override);
    Reflect.call(out.mergedConfiguration, "setConfiguration", DisplayService.displayConfiguration(), override);
  }

  /** Frames, insets and configuration of a window that covers the display. */
  static void fillDisplayFrames(WindowRelayoutResult out) {
    fillFrames(null, null, 0, 0, out);
  }
}
