package aa.host.services;

import android.content.pm.FeatureInfo;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * The device features AnyAndroid provides (what SystemConfig reads from /etc/permissions on a
 * phone): only those it implements, so that applications do not take a camera or a GPS for
 * granted. The mouse stands in for a touch screen.
 */
public final class SystemFeatures {
  private static final List<String> NAMES = Collections.unmodifiableList(Arrays.asList(
      "android.hardware.faketouch",
      "android.hardware.screen.landscape",
      "android.hardware.screen.portrait",
      "android.hardware.touchscreen"));

  private SystemFeatures() {}

  public static boolean has(String name) {
    return NAMES.contains(name);
  }

  public static List<FeatureInfo> list() {
    List<FeatureInfo> out = new ArrayList<>();
    for (String n : NAMES) {
      FeatureInfo f = new FeatureInfo();
      f.name = n;
      out.add(f);
    }
    return out;
  }
}
