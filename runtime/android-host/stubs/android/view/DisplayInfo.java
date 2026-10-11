package android.view;

public final class DisplayInfo {
  public int layerStack;
  public int flags;
  public int type;
  public int displayId;
  public int displayGroupId;
  public String name;
  public String uniqueId;
  public int appWidth;
  public int appHeight;
  public int smallestNominalAppWidth;
  public int smallestNominalAppHeight;
  public int largestNominalAppWidth;
  public int largestNominalAppHeight;
  public int logicalWidth;
  public int logicalHeight;
  public int rotation;
  public int modeId;
  public float renderFrameRate;
  public int defaultModeId;
  public int userPreferredModeId;
  public Display.Mode[] supportedModes;
  public int colorMode;
  public int[] supportedColorModes;
  public int logicalDensityDpi;
  public float physicalXDpi;
  public float physicalYDpi;
  public long appVsyncOffsetNanos;
  public long presentationDeadlineNanos;
  public int state;
  public int committedState;
  public float brightnessMinimum;
  public float brightnessMaximum;
  public float brightnessDefault;
  public float brightnessDim;
  public boolean canHostTasks;

  public DisplayInfo() {}
}
