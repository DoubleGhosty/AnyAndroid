package android.content;

import android.net.Uri;
import java.util.Set;

public class Intent {
  public Intent(String action) {}

  public String getAction() {
    return null;
  }

  public Uri getData() {
    return null;
  }

  public String getScheme() {
    return null;
  }

  public String getType() {
    return null;
  }

  public String getPackage() {
    return null;
  }

  public ComponentName getComponent() {
    return null;
  }

  public Set<String> getCategories() {
    return null;
  }

  public Intent addCategory(String category) {
    return this;
  }

  public Intent setPackage(String packageName) {
    return this;
  }

  public Intent setComponent(ComponentName component) {
    return this;
  }
}
