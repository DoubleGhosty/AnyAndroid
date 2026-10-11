package android.app;

import android.content.Intent;
import android.os.Bundle;

public class Instrumentation {
  public Instrumentation() {}

  public Activity newActivity(ClassLoader cl, String className, Intent intent)
      throws InstantiationException, IllegalAccessException, ClassNotFoundException {
    return null;
  }

  public void callActivityOnCreate(Activity activity, Bundle icicle) {}

  public void callActivityOnPostCreate(Activity activity, Bundle savedInstanceState) {}
}
