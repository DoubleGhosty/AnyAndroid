package android.hardware.display;

public class DisplayInfo {
  public static final int TYPE_INTERNAL = 1;

  public int displayId;
  public int type = TYPE_INTERNAL;
  public String name;
  public int logicalWidth;
  public int logicalHeight;
  public int logicalDensityDpi;
  public float refreshRate;
  public int rotation;
  public int state;
}
