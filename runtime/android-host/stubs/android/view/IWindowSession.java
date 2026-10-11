package android.view;

import android.os.Binder;

public interface IWindowSession {
  abstract class Stub extends Binder implements IWindowSession {
    public static IWindowSession asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }
}
