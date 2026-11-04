package ir.altek.link;

/** Map an initially stretched texture into a centered, aspect-fit 90-degree clockwise image. */
public final class VideoFit {
  public static float[] clockwise(int viewWidth, int viewHeight, int videoWidth, int videoHeight) {
    if (viewWidth <= 0 || viewHeight <= 0 || videoWidth <= 0 || videoHeight <= 0)
      throw new IllegalArgumentException("Video dimensions");
    float scale = Math.min((float) viewWidth / videoHeight, (float) viewHeight / videoWidth);
    float width = videoHeight * scale, height = videoWidth * scale;
    float left = (viewWidth - width) / 2, top = (viewHeight - height) / 2;
    return new float[] {0, -width / viewHeight, left + width, height / viewWidth, 0, top, 0, 0, 1};
  }
}
