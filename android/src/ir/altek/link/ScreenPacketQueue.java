package ir.altek.link;

import java.util.ArrayDeque;

/** Bounded screen writer queues; any new packet wakes the single writer. */
final class ScreenPacketQueue {
  private final ArrayDeque<byte[]> controls = new ArrayDeque<byte[]>(32);
  private final ArrayDeque<byte[]> frames = new ArrayDeque<byte[]>(2);
  private boolean closed, h264, waitingForKey;
  private byte[] format;

  synchronized void offerFormat(byte[] data, boolean compressed) {
    if (closed) return;
    format = data;
    frames.clear();
    h264 = compressed;
    waitingForKey = compressed;
    notifyAll();
  }

  /** Returns true when a broken H.264 chain needs a new key frame. */
  synchronized boolean offerFrame(byte[] data, boolean key) {
    if (closed) return false;
    if (h264 && waitingForKey && !key) return false;
    boolean dropped = frames.size() == 2;
    if (dropped && h264) {
      frames.clear();
      waitingForKey = true;
      if (!key) return true;
    } else if (dropped) frames.removeFirst();
    if (h264 && key) waitingForKey = false;
    frames.addLast(data);
    notifyAll();
    return dropped && h264 && !key;
  }

  synchronized boolean offerControl(byte[] data) {
    if (closed) return true;
    if (controls.size() == 32) return false;
    controls.addLast(data);
    notifyAll();
    return true;
  }

  synchronized byte[] take(long timeoutMs) throws InterruptedException {
    long end = System.nanoTime() + timeoutMs * 1000000L;
    while (!closed && controls.isEmpty() && format == null && frames.isEmpty()) {
      long remaining = end - System.nanoTime();
      if (remaining <= 0) return null;
      wait((remaining + 999999L) / 1000000L);
    }
    if (closed) return null;
    if (!controls.isEmpty()) return controls.removeFirst();
    if (format != null) { byte[] next = format; format = null; return next; }
    return frames.removeFirst();
  }

  synchronized void close() {
    closed = true;
    notifyAll();
  }
}
