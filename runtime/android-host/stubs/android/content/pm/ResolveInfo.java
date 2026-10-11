package android.content.pm;

import android.content.IntentFilter;

public class ResolveInfo {
  public ActivityInfo activityInfo;
  public ServiceInfo serviceInfo;
  public ProviderInfo providerInfo;
  public IntentFilter filter;
  public int match;

  public ResolveInfo() {}
}
