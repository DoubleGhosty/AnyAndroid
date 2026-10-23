package android.view;

public final class SurfaceControl {
  public SurfaceControl(SurfaceControl other, String callsite) {
    throw new RuntimeException("stub");
  }

  public boolean isValid() {
    throw new RuntimeException("stub");
  }

  public boolean isSameSurface(SurfaceControl other) {
    throw new RuntimeException("stub");
  }

  public void release() {
    throw new RuntimeException("stub");
  }

  public static class Builder {
    public Builder() {
      throw new RuntimeException("stub");
    }

    public Builder setName(String name) {
      throw new RuntimeException("stub");
    }

    public Builder setContainerLayer() {
      throw new RuntimeException("stub");
    }

    public Builder setColorLayer() {
      throw new RuntimeException("stub");
    }

    public Builder setCallsite(String callsite) {
      throw new RuntimeException("stub");
    }

    public SurfaceControl build() {
      throw new RuntimeException("stub");
    }
  }

  public static class Transaction {
    public Transaction() {
      throw new RuntimeException("stub");
    }

    public Transaction setPosition(SurfaceControl sc, float x, float y) {
      throw new RuntimeException("stub");
    }

    public Transaction setLayer(SurfaceControl sc, int z) {
      throw new RuntimeException("stub");
    }

    public Transaction setRelativeLayer(SurfaceControl sc, SurfaceControl relativeTo, int z) {
      throw new RuntimeException("stub");
    }

    public Transaction reparent(SurfaceControl sc, SurfaceControl newParent) {
      throw new RuntimeException("stub");
    }

    public Transaction show(SurfaceControl sc) {
      throw new RuntimeException("stub");
    }

    public Transaction hide(SurfaceControl sc) {
      throw new RuntimeException("stub");
    }

    public Transaction setAlpha(SurfaceControl sc, float alpha) {
      throw new RuntimeException("stub");
    }

    public Transaction setColor(SurfaceControl sc, float[] color) {
      throw new RuntimeException("stub");
    }

    public Transaction remove(SurfaceControl sc) {
      throw new RuntimeException("stub");
    }

    public void apply() {
      throw new RuntimeException("stub");
    }
  }
}
