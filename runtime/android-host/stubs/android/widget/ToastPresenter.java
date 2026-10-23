package android.widget;

import android.app.INotificationManager;
import android.app.ITransientNotificationCallback;
import android.content.Context;
import android.os.IBinder;
import android.view.View;
import android.view.accessibility.IAccessibilityManager;

// Declaration of the framework class for javac only (see ../../../README.md).
public class ToastPresenter {
  public ToastPresenter(Context context, IAccessibilityManager accessibilityManager,
      INotificationManager notificationManager, String packageName) {
    throw new RuntimeException("stub");
  }

  public static View getTextToastView(Context context, CharSequence text) {
    throw new RuntimeException("stub");
  }

  public void show(View view, IBinder token, IBinder windowToken, int duration, int gravity, int xOffset,
      int yOffset, float horizontalMargin, float verticalMargin, ITransientNotificationCallback callback,
      boolean removeWindowAnimations) {
    throw new RuntimeException("stub");
  }

  public void hide(ITransientNotificationCallback callback) {
    throw new RuntimeException("stub");
  }
}
