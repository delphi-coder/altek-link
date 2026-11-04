package ir.altek.link;

import android.media.*;
import android.os.*;
import android.view.Surface;
import java.nio.*;
import java.util.concurrent.atomic.AtomicInteger;

/** Bounded decoder jobs; after loss/overload, wait for a sync frame instead of building latency. */
public final class VideoDisplay {
  public interface Listener {
    void size(int width, int height);
  }

  private final Listener listener;
  private final HandlerThread thread = new HandlerThread("video-display");
  private final Handler handler;
  private final Surface surface;
  private MediaCodec codec;
  private ByteBuffer[] inputs;
  private final AtomicInteger queued = new AtomicInteger();
  private volatile boolean closed, waitSync = true;
  private long stats, frames;
  private final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();

  public VideoDisplay(Surface surface, Listener listener) {
    this.listener = listener;
    this.surface = surface;
    thread.start();
    handler = new Handler(thread.getLooper());
    handler.post(pump);
  }

  public void format(final int w, final int h, final byte[] config) {
    DebugLog.event("DECR_FORMAT", "wanted=" + w + "x" + h + " config=" + config.length);
    handler.post(
        new Runnable() {
          public void run() {
            if (closed) return;
            try {
              release();
              if (w < 1 || h < 1 || w > 4096 || h > 4096 || config.length > 65536)
                throw new Exception("Invalid video format");
              listener.size(w, h);
              codec = MediaCodec.createDecoderByType("video/avc");
              MediaFormat m = MediaFormat.createVideoFormat("video/avc", w, h);
              if (config.length > 0) m.setByteBuffer("csd-0", ByteBuffer.wrap(config));
              // API30+: ask the decoder to output decoded frames immediately
              // instead of buffering them, minimizing the display delay.
              if (Build.VERSION.SDK_INT >= 30)
              {
                try {
                  java.lang.reflect.Field f =
                      Class.forName("android.media.MediaFormat").getField("KEY_LOW_LATENCY");
                  m.setInteger((String) f.get(null), 1);
                  DebugLog.event("DECR_FORMAT", "low-latency decode requested");
                } catch (Throwable t) {
                  DebugLog.error("DECODER", new Exception("KEY_LOW_LATENCY", t));
                }
              }
              codec.configure(m, surface, null, 0);
              codec.start();
              inputs = codec.getInputBuffers();
              waitSync = true;
              DebugLog.event("VIDEO_RX_FORMAT", w + "x" + h);
            } catch (Exception e) {
              DebugLog.error("DECODER", e);
              release();
            }
          }
        });
  }

  public void frame(final long pts, final int flags, final byte[] bytes) {
    if (closed) return;
    if (queued.incrementAndGet() > 12) {
      queued.decrementAndGet();
      waitSync = true;
      return;
    }
    handler.post(
        new Runnable() {
          public void run() {
            try {
              if (closed || codec == null) { DebugLog.event("DECODER", "codec null/closed"); return; }
              if (waitSync && (flags & 1) == 0) { DebugLog.event("DECODER", "waitSync drop keyless"); return; }
              drain();
              int n = codec.dequeueInputBuffer(0);
              if (n < 0) {
                waitSync = true;
                return;
              }
              ByteBuffer b = inputs[n];
              ((Buffer) b).clear();
              if (b.remaining() < bytes.length) throw new Exception("Decoder buffer small");
              b.put(bytes);
              codec.queueInputBuffer(n, 0, bytes.length, pts, 0);
              waitSync = false;
              drain();
            } catch (Exception e) {
              DebugLog.error("DECODE", e);
              waitSync = true;
            } finally {
              queued.decrementAndGet();
            }
          }
        });
  }

  private final Runnable pump =
      new Runnable() {
        public void run() {
          if (closed) return;
          try {
            if (codec != null) drain();
          } catch (Exception e) {
            DebugLog.error("DECODE_DRAIN", e);
          }
          handler.postDelayed(this, 10);
        }
      };

  private void drain() {
    for (int i = 0; i < 16; i++) {
      int n = codec.dequeueOutputBuffer(info, 0);
      if (n >= 0) {
        codec.releaseOutputBuffer(n, true);
        frames++;
      } else if (n == MediaCodec.INFO_TRY_AGAIN_LATER) break;
    }
    long now = SystemClock.elapsedRealtime();
    if (now - stats >= 2000) {
      if (stats != 0) DebugLog.event("VIDEO_RX", "displayFPS=" + frames * 1000 / (now - stats));
      stats = now;
      frames = 0;
    }
  }

  private void release() {
    if (codec != null) {
      try {
        codec.stop();
      } catch (Exception ignored) {
      }
      try {
        codec.release();
      } catch (Exception ignored) {
      }
      codec = null;
    }
  }

  public void stop() {
    closed = true;
    handler.post(
        new Runnable() {
          public void run() {
            handler.removeCallbacks(pump);
            release();
            thread.quit();
          }
        });
  }
}
