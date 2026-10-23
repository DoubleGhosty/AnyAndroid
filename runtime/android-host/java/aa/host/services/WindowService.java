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
import android.view.WindowLayout;
import android.view.WindowManager;
import android.view.WindowRelayoutResult;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The "window" service (ADR-006 J4, J9): what the application side asks the window manager about
 * the display (the one of {@link DisplayService}, without system bars) and its window session:
 * windows, their frames by the framework's WindowLayout, their layers in the compositor's tree, and
 * their input channels.
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
   * The application's window session (ADR-006 J5, J9): its windows as WindowManagerService keeps
   * them (WindowState), their frames, their layers and their input channels, real pairs whose
   * window manager's end goes to {@link InputWindows} (J8).
   */
  private static final class WindowSession extends IWindowSession.Stub {
    private final Map<IBinder, WindowRecord> windows = new HashMap<>();

    @Override
    public int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility, int displayId,
        int userId, int requestedVisibleTypes, InputChannel outInputChannel, WindowRelayoutResult outResult) {
      if (displayId != Display.DEFAULT_DISPLAY) return ADD_INVALID_DISPLAY;
      WindowRecord rec;
      synchronized (windows) {
        WindowRecord parent = null;
        if (WindowPolicy.isSubWindow(attrs.type)) {
          parent = attrs.token != null ? windows.get(attrs.token) : null;
          if (parent == null) return ADD_BAD_SUBWINDOW_TOKEN;
        }
        rec = new WindowRecord(window, attrs, parent, requestedVisibleTypes);
        windows.put(window.asBinder(), rec);
      }
      layoutWindow(rec, outResult);
      rec.placeSurface(null, viewVisibility == VISIBLE);
      if (outInputChannel != null) {
        InputChannel[] pair = InputChannel.openInputChannelPair(String.valueOf(window));
        // Input (J8): the window joins the dispatcher's list with the server end.
        InputWindows.add(window, attrs, viewVisibility, pair[0], rec.frame);
        pair[1].copyTo(outInputChannel);
      }
      return ADD_FLAG_IN_TOUCH_MODE | ADD_FLAG_APP_VISIBLE;  // ADD_OKAY (0) with these flags
    }

    /**
     * WindowManagerService.relayoutWindow: new attributes and requested size, the frames of a
     * new layout, the client's surface put under the window's layer (WindowState.setClientSurface).
     */
    @Override
    public int relayout(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth, int requestedHeight,
        int viewVisibility, int flags, int seq, int lastSyncSeqId, SurfaceControl surfaceControl,
        WindowRelayoutResult outRelayoutResult) {
      WindowRecord rec;
      synchronized (windows) {
        rec = window != null ? windows.get(window.asBinder()) : null;
      }
      if (rec == null) return 0;
      if (attrs != null) rec.attrs.copyFrom(attrs);
      rec.requestedWidth = requestedWidth;
      rec.requestedHeight = requestedHeight;
      if (outRelayoutResult != null) {
        layoutWindow(rec, outRelayoutResult);
      }
      boolean visible = viewVisibility == VISIBLE;
      rec.placeSurface(visible ? surfaceControl : null, visible);
      // Input (J8): attributes, visibility and frame of the window for the dispatcher.
      InputWindows.update(window, attrs, viewVisibility, outRelayoutResult != null ? rec.frame : null);
      return 0;
    }

    @Override
    public void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth,
        int requestedHeight, int viewVisibility, int flags, int seq, int lastSyncSeqId,
        SurfaceControl surfaceControl) {
      relayout(window, attrs, requestedWidth, requestedHeight, viewVisibility, flags, seq, lastSyncSeqId,
          surfaceControl, null);
    }

    /** The window goes away with its layers (and its sub-windows'), and its input channel (J8). */
    @Override
    public void remove(IBinder clientToken) {
      if (clientToken == null) return;
      WindowRecord rec;
      List<WindowRecord> children = new ArrayList<>();
      synchronized (windows) {
        rec = windows.remove(clientToken);
        for (WindowRecord w : windows.values()) {
          if (rec != null && w.parent == rec) children.add(w);
        }
        for (WindowRecord child : children) windows.remove(child.window.asBinder());
      }
      for (WindowRecord child : children) {
        child.removeSurfaces();
        InputWindows.remove(child.window.asBinder());
      }
      if (rec != null) rec.removeSurfaces();
      InputWindows.remove(clientToken);
    }
  }

  /**
   * A window as WindowManagerService keeps it (WindowState): attributes, requested size, frame, and
   * its layer, under which the application's own surface goes, with Dimmer's dim layer when the
   * window dims behind it.
   */
  private static final class WindowRecord {
    final IWindow window;
    final WindowManager.LayoutParams attrs = new WindowManager.LayoutParams();
    final WindowRecord parent;  // a sub-window's: the window of attrs.token
    final int requestedVisibleTypes;
    int requestedWidth = UNSPECIFIED_LENGTH, requestedHeight = UNSPECIFIED_LENGTH;
    final Rect frame = new Rect();
    final SurfaceControl surface;  // WindowContainer.mSurfaceControl
    SurfaceControl clientSurface;  // WindowStateAnimator.mSurfaceControl, a copy (isClientLocal)
    SurfaceControl dimSurface;     // Dimmer's, while the window dims

    WindowRecord(IWindow window, WindowManager.LayoutParams a, WindowRecord parent, int requestedVisibleTypes) {
      this.window = window;
      this.attrs.copyFrom(a);
      this.parent = parent;
      this.requestedVisibleTypes = requestedVisibleTypes;
      this.surface = new SurfaceControl.Builder().setContainerLayer().setName(toString())
          .setCallsite("WindowContainer.setInitialSurfaceControlProperties").build();
    }

    /**
     * The layers' state after a layout: WindowState.setClientSurface (a new client surface goes
     * under the window's layer), updateSurfacePosition (the frame, less the surface insets, relative
     * to the parent window for a sub-window), the layer by the policy, visibility, and the dim of
     * FLAG_DIM_BEHIND (Dimmer: a black color layer of alpha dimAmount just below the window; no
     * fade).
     */
    void placeSurface(SurfaceControl client, boolean visible) {
      SurfaceControl.Transaction t = new SurfaceControl.Transaction();
      if (client != null && client.isValid()
          && (clientSurface == null || !clientSurface.isSameSurface(client))) {
        if (clientSurface != null) clientSurface.release();
        clientSurface = new SurfaceControl(client, "setClientSurface");
        t.reparent(clientSurface, surface);
      }
      int x = frame.left - attrs.surfaceInsets.left, y = frame.top - attrs.surfaceInsets.top;
      if (parent != null) {
        t.reparent(surface, parent.surface);
        x += parent.attrs.surfaceInsets.left - parent.frame.left;
        y += parent.attrs.surfaceInsets.top - parent.frame.top;
        t.setLayer(surface, WindowPolicy.subWindowLayer(attrs.type));
      } else {
        t.setLayer(surface, WindowPolicy.windowLayer(attrs.type, false));
      }
      t.setPosition(surface, x, y);
      if (visible) {
        t.show(surface);
      } else {
        t.hide(surface);
      }
      if (visible && (attrs.flags & FLAG_DIM_BEHIND) != 0) {
        if (dimSurface == null) {
          dimSurface = new SurfaceControl.Builder().setColorLayer().setName("Dim Layer for - " + this)
              .setCallsite("DimLayer.makeDimLayer").build();
          t.setColor(dimSurface, new float[] {0f, 0f, 0f});  // Color.BLACK
        }
        t.setRelativeLayer(dimSurface, surface, -1);
        t.setAlpha(dimSurface, attrs.dimAmount);
        t.show(dimSurface);
      } else if (dimSurface != null) {
        t.remove(dimSurface);
        dimSurface = null;
      }
      t.apply();
    }

    /** removeImmediately: the window's layer leaves the tree, and the layers under it. */
    void removeSurfaces() {
      SurfaceControl.Transaction t = new SurfaceControl.Transaction();
      if (dimSurface != null) t.remove(dimSurface);
      t.remove(surface);
      t.apply();
      if (clientSurface != null) clientSurface.release();
      dimSurface = clientSurface = null;
    }

    @Override
    public String toString() {
      return "Window{" + Integer.toHexString(System.identityHashCode(this)) + " u0 " + attrs.getTitle() + "}";
    }
  }

  // WindowManagerGlobal's results of addToDisplay.
  private static final int ADD_BAD_SUBWINDOW_TOKEN = -2, ADD_INVALID_DISPLAY = -9;
  /** WindowManagerGlobal.ADD_FLAG_IN_TOUCH_MODE (phones start in touch mode), ADD_FLAG_APP_VISIBLE. */
  private static final int ADD_FLAG_IN_TOUCH_MODE = 1, ADD_FLAG_APP_VISIBLE = 2;
  /** WindowLayout.UNSPECIFIED_LENGTH, WindowConfiguration.WINDOWING_MODE_FULLSCREEN. */
  private static final int UNSPECIFIED_LENGTH = -1, WINDOWING_MODE_FULLSCREEN = 1;
  private static final int FLAG_DIM_BEHIND = 0x2;  // WindowManager.LayoutParams
  private static final int VISIBLE = 0;  // View.VISIBLE

  /** DisplayPolicy's, used under its own lock. */
  private static final WindowLayout WINDOW_LAYOUT = new WindowLayout();

  /**
   * DisplayPolicy.layoutWindowLw: the frames of the framework's own WindowLayout.computeFrames, on
   * a display without bars nor cutout, in full screen; then the configuration of the window.
   */
  private static void layoutWindow(WindowRecord rec, WindowRelayoutResult out) {
    Rect display = new Rect(0, 0, DisplayService.WIDTH, DisplayService.HEIGHT);
    out.insetsState.setDisplayFrame(display);
    Rect cutoutSafe = new Rect();
    out.insetsState.getDisplayCutoutSafe(cutoutSafe);
    out.frames.attachedFrame = rec.parent != null ? new Rect(rec.parent.frame) : null;
    synchronized (WINDOW_LAYOUT) {
      WINDOW_LAYOUT.computeFrames(rec.attrs, out.insetsState, cutoutSafe, display, WINDOWING_MODE_FULLSCREEN,
          rec.requestedWidth, rec.requestedHeight, rec.requestedVisibleTypes, 1f, out.frames);
    }
    out.frames.compatScale = 1f;
    rec.frame.set(out.frames.frame);
    Object override = Reflect.newInstance(Reflect.type("android.content.res.Configuration"));
    DisplayService.fillWindowConfiguration(override);
    Reflect.call(out.mergedConfiguration, "setConfiguration", DisplayService.displayConfiguration(), override);
  }
}
