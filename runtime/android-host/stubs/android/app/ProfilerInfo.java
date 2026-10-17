package android.app;

import android.os.Parcel;
import android.os.Parcelable;

/** Stub of android.app.ProfilerInfo for IActivityTaskManager.startActivity. */
public class ProfilerInfo implements Parcelable {
  public static final Creator<ProfilerInfo> CREATOR = new Creator<ProfilerInfo>() {
    public ProfilerInfo createFromParcel(Parcel in) { return null; }
    public ProfilerInfo[] newArray(int size) { return new ProfilerInfo[size]; }
  };

  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int flags) {}
}
