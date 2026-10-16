package android.app;

import android.content.IContentProvider;
import android.content.pm.ProviderInfo;

public class ContentProviderHolder {
  public final ProviderInfo info;
  public IContentProvider provider;
  public boolean noReleaseNeeded;

  public ContentProviderHolder(ProviderInfo info) {
    this.info = info;
  }
}
