package ir.altek.link;

import java.util.ArrayDeque;

/** A bounded, decodable H.264 chain while the viewer surface is being created. */
final class ScreenStartupFrames {
  static final int MAX_FRAMES = 8;
  static final int MAX_BYTES = 8 * 1024 * 1024;
  static final class Frame {
    final boolean key;
    final long pts;
    final byte[] bytes;
    Frame(boolean key, long pts, byte[] bytes) {
      this.key = key;
      this.pts = pts;
      this.bytes = bytes;
    }
  }
  private final ArrayDeque<Frame> frames = new ArrayDeque<Frame>();
  private int size;

  void clear() { frames.clear(); size = 0; }

  void offer(boolean key, long pts, byte[] bytes) {
    if (key) clear();
    if ((!key && frames.isEmpty()) || bytes.length > MAX_BYTES) return;
    if (frames.size() >= MAX_FRAMES || size > MAX_BYTES - bytes.length) {
      clear();
      return;
    }
    frames.addLast(new Frame(key, pts, bytes));
    size += bytes.length;
  }

  Frame take() {
    Frame f = frames.pollFirst();
    if (f != null) size -= f.bytes.length;
    return f;
  }
}
