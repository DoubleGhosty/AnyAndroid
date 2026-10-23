package aa.host.services;

import android.media.IAudioService;

/**
 * The "audio" service: AnyAndroid has no audio output yet. What the framework asks when a view
 * is clicked (View.playSoundEffect) gets AudioService's answers, and the sound effects are not
 * played.
 */
public final class AudioService extends IAudioService.Stub {
  public AudioService() {
    super(LocalPermissionEnforcer.INSTANCE);
  }

  /** Set by SystemUI on a phone; false until then, as in AudioService. */
  @Override
  public boolean areNavigationRepeatSoundEffectsEnabled() {
    return false;
  }

  @Override
  public boolean isHomeSoundEffectEnabled() {
    return false;
  }

  /** AudioService.playSoundEffect: nothing to play the effect on. */
  @Override
  public void playSoundEffect(int effectType, int userId) {}

  @Override
  public void playSoundEffectVolume(int effectType, float volume) {}
}
