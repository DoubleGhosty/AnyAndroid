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

  int relayout(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth, int requestedHeight,
      int viewVisibility, int flags, int seq, int lastSyncSeqId, SurfaceControl outSurfaceControl,
      WindowRelayoutResult outRelayoutResult);

  void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth, int requestedHeight,
      int viewVisibility, int flags, int seq, int lastSyncSeqId, SurfaceControl outSurfaceControl);
}
