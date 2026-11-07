package ir.altek.link;

/** Stabilize the location of a second nearby tap for the remote OS double-click detector. */
final class RemoteTap {
  private long previousTime = -1;
  private float previousX, previousY;

  float[] position(long time, float x, float y, int width, int height, float tolerance) {
    float dx = (x - previousX) * width, dy = (y - previousY) * height;
    if (previousTime >= 0 && time >= previousTime && time - previousTime <= 400
        && dx * dx + dy * dy <= tolerance * tolerance) {
      previousTime = -1;
      return new float[] {previousX, previousY};
    }
    previousTime = time;
    previousX = x;
    previousY = y;
    return new float[] {x, y};
  }

  void reset() { previousTime = -1; }
}
