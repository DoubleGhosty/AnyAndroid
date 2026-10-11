package com.android.internal.view;

import android.os.Binder;

public interface IInputMethodManager {
  abstract class Stub extends Binder implements IInputMethodManager {
    public static IInputMethodManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }
}
