package aa.host.services;

import aa.host.Launcher;
import android.app.ActivityThread;
import android.app.INotificationManager;
import android.app.ITransientNotification;
import android.app.ITransientNotificationCallback;
import android.content.Context;
import android.content.res.Resources;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.ServiceManager;
import android.view.accessibility.IAccessibilityManager;
import android.widget.ToastPresenter;
import java.util.ArrayList;

/**
 * The "notification" service, its toasts (NotificationManagerService): one toast at a time, in
 * queue order, at most {@link #MAX_PACKAGE_TOASTS} per package, shown for LENGTH_SHORT 2 s or
 * LENGTH_LONG 3.5 s (a text toast 333 ms more for its "in" animation, 250 ms more after a text
 * toast for its "out" one), then hidden and the next one shown.
 *
 * <p>Text toasts are shown as SystemUI's ToastUI does, with the framework's ToastPresenter, here
 * in the application's process and on its main thread, with the framework's transient_notification
 * view (SystemUI's own layout, with the application's icon, is not loaded). Custom toasts are shown
 * by the application's Toast.TN, as on a phone. Not done: rate limiting, toasts of background or
 * suspended packages, accessibility timeouts, registering the window token with the window manager.
 */
public final class NotificationService extends INotificationManager.Stub {
  static final int SHORT_DELAY = 2000, LONG_DELAY = 3500, MAX_PACKAGE_TOASTS = 5;
  private static final int LENGTH_LONG = 1;

  /** ToastRecord: a text toast (TextToastRecord) or a custom one (CustomToastRecord). */
  private final class ToastRecord {
    final String pkg;
    final IBinder token;
    final CharSequence text;
    final ITransientNotification callback;
    final ITransientNotificationCallback textCallback;
    final IBinder windowToken = new Binder();
    int duration;
    final Runnable durationReached = () -> handleDurationReached(this);

    ToastRecord(String pkg, IBinder token, CharSequence text, ITransientNotification callback,
        ITransientNotificationCallback textCallback, int duration) {
      this.pkg = pkg;
      this.token = token;
      this.text = text;
      this.callback = callback;
      this.textCallback = textCallback;
      this.duration = duration;
    }
  }

  private final ArrayList<ToastRecord> queue = new ArrayList<>();  // guarded by itself
  private boolean currentShown;
  private Handler handler;

  // SystemUI's side (ToastUI), on the main thread: the text toast shown.
  private ToastPresenter presenter;
  private ToastRecord presented;

  public NotificationService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  @Override
  public boolean enqueueTextToast(String pkg, IBinder token, CharSequence text, int duration, boolean isUiContext,
      int displayId, ITransientNotificationCallback callback) {
    return enqueueToast(pkg, token, text, null, duration, callback);
  }

  @Override
  public boolean enqueueToast(String pkg, IBinder token, ITransientNotification callback, int duration,
      boolean isUiContext, int displayId) {
    return enqueueToast(pkg, token, null, callback, duration, null);
  }

  private boolean enqueueToast(String pkg, IBinder token, CharSequence text, ITransientNotification callback,
      int duration, ITransientNotificationCallback textCallback) {
    if (pkg == null || (text == null && callback == null) || (text != null && callback != null) || token == null) {
      return false;
    }
    synchronized (queue) {
      int index = indexOfToastLocked(pkg, token);
      if (index >= 0) {
        queue.get(index).duration = duration;
      } else {
        int count = 0;
        for (ToastRecord r : queue) {
          if (r.pkg.equals(pkg) && ++count >= MAX_PACKAGE_TOASTS) return false;
        }
        queue.add(new ToastRecord(pkg, token, text, callback, textCallback, duration));
        index = queue.size() - 1;
      }
      if (index == 0) showNextToastLocked(false);
    }
    return true;
  }

  @Override
  public void cancelToast(String pkg, IBinder token) {
    if (pkg == null || token == null) return;
    synchronized (queue) {
      int index = indexOfToastLocked(pkg, token);
      if (index >= 0) cancelToastLocked(index);
    }
  }

  /** The window token is not registered with the window manager: nothing to remove. */
  @Override
  public void finishToken(String pkg, IBinder token) {}

  @Override
  public void setToastRateLimitingEnabled(boolean enable) {}

  private int indexOfToastLocked(String pkg, IBinder token) {
    for (int i = 0; i < queue.size(); i++) {
      ToastRecord r = queue.get(i);
      if (r.pkg.equals(pkg) && r.token == token) return i;
    }
    return -1;
  }

  private Handler handler() {
    if (handler == null) handler = new Handler(Looper.getMainLooper());
    return handler;
  }

  private void showNextToastLocked(boolean lastToastWasText) {
    if (currentShown) return;
    while (!queue.isEmpty()) {
      ToastRecord record = queue.get(0);
      if (show(record)) {
        scheduleDurationReachedLocked(record, lastToastWasText);
        currentShown = true;
        return;
      }
      queue.remove(0);
    }
  }

  private boolean show(ToastRecord record) {
    if (record.text != null) {
      handler().post(() -> showTextToast(record));  // StatusBarManagerInternal.showToast
      return true;
    }
    try {
      record.callback.show(record.windowToken);
      return true;
    } catch (RuntimeException e) {
      return false;
    }
  }

  private void hide(ToastRecord record) {
    if (record.text != null) {
      handler().post(() -> hideTextToast(record));
    } else {
      try {
        record.callback.hide();
      } catch (RuntimeException ignored) {
        // As NotificationManagerService: the application is gone, the window too.
      }
    }
  }

  private void scheduleDurationReachedLocked(ToastRecord record, boolean lastToastWasText) {
    handler().removeCallbacks(record.durationReached);
    int delay = record.duration == LENGTH_LONG ? LONG_DELAY : SHORT_DELAY;
    if (lastToastWasText) delay += 250;  // the previous toast's "out" animation
    if (record.text != null) delay += 333;  // this toast's "in" animation
    handler().postDelayed(record.durationReached, delay);
  }

  private void handleDurationReached(ToastRecord record) {
    synchronized (queue) {
      int index = indexOfToastLocked(record.pkg, record.token);
      if (index >= 0) cancelToastLocked(index);
    }
  }

  private void cancelToastLocked(int index) {
    ToastRecord record = queue.get(index);
    handler().removeCallbacks(record.durationReached);
    hide(record);
    if (index == 0) currentShown = false;
    queue.remove(index);
    if (!queue.isEmpty()) showNextToastLocked(record.text != null);
  }

  // --- SystemUI's ToastUI, on the main thread ---

  /** ToastUI.showToast: the default SystemUIToast's gravity and offset, from the framework. */
  private void showTextToast(ToastRecord record) {
    if (presenter != null) hideTextToast(presented);
    Context context = ActivityThread.currentApplication();
    Resources res = Resources.getSystem();
    int gravity = res.getInteger(res.getIdentifier("config_toastDefaultGravity", "integer", "android"));
    int yOffset = res.getDimensionPixelSize(res.getIdentifier("toast_y_offset", "dimen", "android"));
    IAccessibilityManager accessibility = (IAccessibilityManager) ServiceManager.getService("accessibility");
    presenter = new ToastPresenter(context, accessibility, this, record.pkg);
    presented = record;
    presenter.show(ToastPresenter.getTextToastView(context, record.text), record.token, record.windowToken,
        record.duration, gravity, 0, yOffset, 0, 0, record.textCallback, false);
    Launcher.log("toast : « " + record.text + " » montré");
  }

  /** ToastUI.hideToast: only the toast shown. */
  private void hideTextToast(ToastRecord record) {
    if (presenter == null || presented != record) return;
    presenter.hide(record.textCallback);
    presenter = null;
    presented = null;
    Launcher.log("toast : « " + record.text + " » masqué");
  }
}
