package aa.host.services;

import aa.host.Reflect;
import android.content.res.Resources;
import android.graphics.Point;
import android.graphics.Rect;
import android.view.Display;
import android.view.IWindow;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.IWindowSessionCallback;
import android.view.InputChannel;
import android.view.WindowManager;
import android.view.WindowRelayoutResult;
import java.util.ArrayList;
import java.util.List;

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

  /**
   * The application's window session (ADR-006 J5). A window covers the display, which has no
   * system bars: its frames are the display's, its insets empty. Its input channel is a real
   * pair; the window manager's end stays here for the input of J8.
   */
  private static final class WindowSession extends IWindowSession.Stub {
    private final List<InputChannel> serverChannels = new ArrayList<>();

    @Override
    public int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility, int displayId,
        int userId, int requestedVisibleTypes, InputChannel outInputChannel, WindowRelayoutResult outResult) {
      if (displayId != Display.DEFAULT_DISPLAY) return -9;  // WindowManagerGlobal.ADD_INVALID_DISPLAY
      fillDisplayFrames(outResult);
      if (outInputChannel != null) {
        InputChannel[] pair = InputChannel.openInputChannelPair(String.valueOf(window));
        synchronized (serverChannels) {
          serverChannels.add(pair[0]);
        }
        pair[1].copyTo(outInputChannel);
      }
      return ADD_FLAG_IN_TOUCH_MODE | ADD_FLAG_APP_VISIBLE;  // ADD_OKAY (0) with these flags
    }
  }

  /** WindowManagerGlobal.ADD_FLAG_IN_TOUCH_MODE (phones start in touch mode), ADD_FLAG_APP_VISIBLE. */
  private static final int ADD_FLAG_IN_TOUCH_MODE = 1, ADD_FLAG_APP_VISIBLE = 2;

  /** Frames, insets and configuration of a window that covers the display. */
  static void fillDisplayFrames(WindowRelayoutResult out) {
    Rect display = new Rect(0, 0, DisplayService.WIDTH, DisplayService.HEIGHT);
    out.frames.frame.set(display);
    out.frames.displayFrame.set(display);
    out.frames.parentFrame.set(display);
    out.frames.attachedFrame = null;
    out.frames.compatScale = 1f;
    out.insetsState.setDisplayFrame(display);
    Object global = Reflect.newInstance(Reflect.type("android.content.res.Configuration"),
        Reflect.call(Resources.getSystem(), "getConfiguration"));
    Reflect.call(out.mergedConfiguration, "setConfiguration", global,
        Reflect.newInstance(Reflect.type("android.content.res.Configuration")));
  }
}
