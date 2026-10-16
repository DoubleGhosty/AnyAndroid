package android.content.pm;

import java.util.Collection;

public final class SystemFeaturesCache {
  public SystemFeaturesCache(Collection<FeatureInfo> features) {}

  public static void setInstance(SystemFeaturesCache cache) {}

  public static SystemFeaturesCache getInstance() {
    return null;
  }

  public int[] getSdkFeatureVersions() {
    throw new RuntimeException("stub");
  }
}
