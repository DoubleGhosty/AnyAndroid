package android.media;

import android.os.Binder;

public interface IAudioService {
  abstract class Stub extends Binder implements IAudioService {
    public Stub(android.os.PermissionEnforcer enforcer) {}

    public static IAudioService asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  boolean areNavigationRepeatSoundEffectsEnabled();
  boolean isHomeSoundEffectEnabled();
  void playSoundEffect(int effectType, int userId);
  void playSoundEffectVolume(int effectType, float volume);
}
