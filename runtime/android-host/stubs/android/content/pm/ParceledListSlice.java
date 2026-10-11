package android.content.pm;

import java.util.List;

public class ParceledListSlice<T> {
  public ParceledListSlice(List<T> list) {}

  public static <T> ParceledListSlice<T> emptyList() {
    return null;
  }
}
