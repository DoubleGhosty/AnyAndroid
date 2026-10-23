package android.app;

import android.os.IBinder;

public interface ITransientNotification {
  void show(IBinder windowToken);

  void hide();
}
