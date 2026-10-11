package android.view;

public final class Display {
  public static final int DEFAULT_DISPLAY = 0;
  public static final int TYPE_INTERNAL = 1;
  public static final int STATE_ON = 2;

  public static final class Mode {
    public static final int INVALID_MODE_ID = -1;

    public Mode(int modeId, int width, int height, float refreshRate) {}
  }
}
