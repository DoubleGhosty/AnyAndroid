package android.view;

import android.os.Binder;

public interface IWindowSession {
  abstract class Stub extends Binder implements IWindowSession {
    public Stub() {}

    public static IWindowSession asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility, int displayId,
      int userId, int requestedVisibleTypes, InputChannel outInputChannel, WindowRelayoutResult outRelayoutResult);
}
