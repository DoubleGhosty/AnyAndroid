package android.app;

import android.os.Binder;
import android.os.IBinder;

public interface INotificationManager {
  abstract class Stub extends Binder implements INotificationManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}

    public static INotificationManager asInterface(IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  boolean enqueueTextToast(String pkg, IBinder token, CharSequence text, int duration, boolean isUiContext,
      int displayId, ITransientNotificationCallback callback);

  boolean enqueueToast(String pkg, IBinder token, ITransientNotification callback, int duration,
      boolean isUiContext, int displayId);

  void cancelToast(String pkg, IBinder token);

  void finishToken(String pkg, IBinder token);

  void setToastRateLimitingEnabled(boolean enable);
}
