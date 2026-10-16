package android.app;

public class ApplicationErrorReport {
  public static class CrashInfo {
    public String exceptionClassName;
    public String exceptionMessage;
    public String stackTrace;
  }

  public static class ParcelableCrashInfo extends CrashInfo {}
}
