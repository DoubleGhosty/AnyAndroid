package android.view;

public final class InputDevice {
  public static final int SOURCE_KEYBOARD = 0x00000101;
  public static final int KEYBOARD_TYPE_ALPHABETIC = 2;

  public static final class Builder {
    public Builder() {}

    public Builder setId(int id) {
      throw new RuntimeException("stub");
    }

    public Builder setName(String name) {
      throw new RuntimeException("stub");
    }

    public Builder setDescriptor(String descriptor) {
      throw new RuntimeException("stub");
    }

    public Builder setSources(int sources) {
      throw new RuntimeException("stub");
    }

    public Builder setKeyboardType(int keyboardType) {
      throw new RuntimeException("stub");
    }

    public Builder setKeyCharacterMap(KeyCharacterMap keyCharacterMap) {
      throw new RuntimeException("stub");
    }

    public Builder setIsVirtualDevice(boolean isVirtualDevice) {
      throw new RuntimeException("stub");
    }

    public InputDevice build() {
      throw new RuntimeException("stub");
    }
  }
}
