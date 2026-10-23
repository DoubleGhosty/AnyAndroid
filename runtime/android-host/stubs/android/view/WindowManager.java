package android.view;

import android.os.IBinder;

public interface WindowManager {
  class LayoutParams extends ViewGroup.LayoutParams {
    public int x;
    public int y;
    public int type;
    public int flags;
    public int gravity;
    public float dimAmount;
    public IBinder token;

    public final int copyFrom(LayoutParams o) {
      throw new RuntimeException("stub");
    }

    public final CharSequence getTitle() {
      throw new RuntimeException("stub");
    }
  }
}
