package ir.altek.link;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.media.projection.MediaProjection;
import android.os.Build;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.Surface;
import android.view.WindowManager;
import java.io.*;
import java.nio.Buffer;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * Captures the screen for the "screen" call kind, picking the best available method for this
 * device and falling back when something isn't available or the person declines a prompt:
 *
 * <ul>
 *   <li>API 21+ (Lollipop and up): {@link MediaProjection} feeding this app's own H.264 encoder, the
 *       same way {@link VideoCapture} already encodes the camera - no root needed, only a one-time
 *       system "Start recording or casting?" prompt per session (that prompt is requested by the
 *       activity, not here; this class is handed an already-approved {@link MediaProjection}).
 *   <li>API 19-20, or API 21+ when the person declines the prompt: falls back to streaming real
 *       H.264 from the `screenrecord` shell tool (root required), parsed from its raw Annex-B
 *       elementary stream into access units compatible with {@link VideoDisplay}.
 *       `screenrecord` stops on its own after a time limit, so a session longer than that is
 *       continued by starting a fresh process; the decoder just gets a new format() call.
 *   <li>API 16-18: `screenrecord` does not exist yet, so this instead loops on `screencap -p`
 *       (which has existed since very early Android), decodes the PNG, scales it down, and sends it
 *       as a JPEG "frame" at a modest rate (not a real video stream).
 * </ul>
 *
 * Not tested on a real device or emulator (this environment has neither); the H.264 parsing in
 * particular is new code exercising an interface (`screenrecord`'s raw stdout) that was only read
 * from documentation, not observed.
 */
public final class ScreenCapture {
  public interface Sink {
    /** config is the SPS/PPS bytes for h264=true, or null for h264=false (JPEG frames). */
    void format(boolean h264, int w, int h, byte[] config);

    /** bytes is one H.264 access unit (h264=true) or one full JPEG image (h264=false). */
    void frame(boolean keyFrame, long ptsMicros, byte[] bytes);

    void status(String text);

    void error(Exception e);
  }

  private final Sink sink;
  private volatile boolean stop;
  private volatile Process process;
  private final Thread thread;

  // MediaProjection-path state, released by stop().
  private MediaCodec encoder;
  private VirtualDisplay virtualDisplay;
  private MediaProjection projection;

  /** Root fallback: screenrecord (API 19-20) or periodic screenshots (API 16-18). */
  public ScreenCapture(final Context context, final Sink sink) {
    this.sink = sink;
    final boolean stream = Build.VERSION.SDK_INT >= 19;
    thread =
        new Thread(
            new Runnable() {
              public void run() {
                try {
                  sink.status("Waiting for root access (check your root manager app)...");
                  RootShell.Result root = RootShell.probe();
                  if (!root.granted) throw new IOException("Root access not available: " + root.detail);
                  int[] size = targetSize(context, stream ? 1280 : 720);
                  if (stream) streamLoop(size[0], size[1]);
                  else snapshotLoop(size[0], size[1]);
                } catch (Exception e) {
                  if (!stop) sink.error(e);
                }
              }
            },
            "screen-capture");
    thread.start();
  }

  /** Preferred path on API 21+: no root, uses an already-approved MediaProjection. */
  public ScreenCapture(final Context context, final MediaProjection projection, final Sink sink) {
    this.sink = sink;
    this.projection = projection;
    thread =
        new Thread(
            new Runnable() {
              public void run() {
                try {
                  int[] size = targetSize(context, 1280);
                  projectionLoop(context, projection, size[0], size[1]);
                } catch (Exception e) {
                  if (!stop) sink.error(e);
                }
              }
            },
            "screen-capture-projection");
    thread.start();
  }

  public void stop() {
    stop = true;
    final Process p = process;
    if (p != null) {
      p.destroy();
      // Belt and braces: some root shells don't forward the kill signal to the grandchild they
      // spawned (screenrecord), which would otherwise keep running on the device indefinitely.
      new Thread(
              new Runnable() {
                public void run() {
                  try {
                    RootShell.run("killall -2 screenrecord 2>/dev/null", 2000);
                  } catch (Exception ignored) {
                  }
                }
              },
              "screen-capture-cleanup")
          .start();
    }
    if (virtualDisplay != null) {
      try {
        virtualDisplay.release();
      } catch (Exception ignored) {
      }
    }
    if (encoder != null) {
      try {
        encoder.stop();
      } catch (Exception ignored) {
      }
      try {
        encoder.release();
      } catch (Exception ignored) {
      }
    }
    if (projection != null) {
      try {
        projection.stop();
      } catch (Exception ignored) {
      }
    }
    thread.interrupt();
  }

  // -------------------------------------------------------------- API 21+: MediaProjection / H.264

  /** Same encoder-draining pattern as {@link VideoCapture}, with the surface fed by VirtualDisplay
   *  instead of the camera, so the system composites frames straight into the encoder (no CPU copy). */
  private void projectionLoop(Context context, MediaProjection projection, int w, int h)
      throws Exception {
    sink.status("Starting screen share...");
    MediaCodec enc = null;
    ByteBuffer[] output = null;
    for (int i = 0; i < MediaCodecList.getCodecCount() && enc == null; i++) {
      MediaCodecInfo info = MediaCodecList.getCodecInfoAt(i);
      if (!info.isEncoder()) continue;
      boolean avc = false;
      for (String type : info.getSupportedTypes()) if (type.equalsIgnoreCase("video/avc")) avc = true;
      if (!avc) continue;
      try {
        MediaCodec candidate = MediaCodec.createByCodecName(info.getName());
        MediaFormat f = MediaFormat.createVideoFormat("video/avc", w, h);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        f.setInteger(MediaFormat.KEY_BIT_RATE, 6000000);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, 24);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        candidate.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        enc = candidate;
      } catch (Exception | OutOfMemoryError e) {
        DebugLog.error("SCREEN_ENCODER_TRY", e);
      }
    }
    if (enc == null) throw new IOException("No compatible H264 encoder for screen share");
    encoder = enc;
    Surface inputSurface = enc.createInputSurface();
    enc.start();
    output = enc.getOutputBuffers();
    DisplayMetrics dm = new DisplayMetrics();
    ((WindowManager) context.getSystemService(Context.WINDOW_SERVICE))
        .getDefaultDisplay()
        .getMetrics(dm);
    virtualDisplay =
        projection.createVirtualDisplay(
            "ALTEKLinkScreenShare",
            w,
            h,
            dm.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            inputSurface,
            null,
            null);
    MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
    byte[] config = null;
    // What this app asked the encoder for; corrected below to what the encoder actually reports
    // once its output format is known, in case the device rounds/pads a size that isn't already a
    // clean multiple of its macroblock size (it shouldn't be, now that targetSize() rounds to 16,
    // but devices vary and this makes sure the number sent to the decoder is never a guess).
    int outW = w, outH = h;
    while (!stop) {
      int n;
      try {
        n = enc.dequeueOutputBuffer(info, 100000);
      } catch (IllegalStateException giveUp) {
        // stop() may have already stopped/released this encoder from another thread.
        if (stop) return;
        throw giveUp;
      }
      if (n == MediaCodec.INFO_TRY_AGAIN_LATER) continue;
      if (n == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
        output = enc.getOutputBuffers();
        continue;
      }
      if (n == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
        MediaFormat f = enc.getOutputFormat();
        if (f.containsKey(MediaFormat.KEY_WIDTH)) outW = f.getInteger(MediaFormat.KEY_WIDTH);
        if (f.containsKey(MediaFormat.KEY_HEIGHT)) outH = f.getInteger(MediaFormat.KEY_HEIGHT);
        byte[] a = csd(f, "csd-0"), b = csd(f, "csd-1");
        config = new byte[a.length + b.length];
        System.arraycopy(a, 0, config, 0, a.length);
        System.arraycopy(b, 0, config, a.length, b.length);
        sink.format(true, outW, outH, config);
        continue;
      }
      if (n < 0) continue;
      try {
        if (info.size == 0) continue;
        ByteBuffer b = output[n].duplicate();
        ((Buffer) b).position(info.offset);
        ((Buffer) b).limit(info.offset + info.size);
        byte[] out = new byte[info.size];
        b.get(out);
        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
          config = out;
          sink.format(true, outW, outH, config);
        } else {
          boolean key = (info.flags & MediaCodec.BUFFER_FLAG_SYNC_FRAME) != 0;
          sink.frame(key, info.presentationTimeUs, out);
        }
      } finally {
        try {
          enc.releaseOutputBuffer(n, false);
        } catch (IllegalStateException ignored) {
          if (!stop) throw ignored;
        }
      }
    }
  }

  private static byte[] csd(MediaFormat f, String key) {
    if (!f.containsKey(key)) return new byte[0];
    ByteBuffer b = f.getByteBuffer(key).duplicate();
    byte[] x = new byte[b.remaining()];
    b.get(x);
    return x;
  }

  /** The device's real screen size, for mapping normalized touch coordinates back to pixels. */
  public static int[] realSize(Context context) {
    DisplayMetrics m = new DisplayMetrics();
    android.view.Display d =
        ((WindowManager) context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay();
    if (Build.VERSION.SDK_INT >= 17) {
      d.getRealMetrics(m);
    } else {
      // getRealMetrics() needs API 17; this excludes the nav/status bar on API 16, which is the
      // best this can do there (screenrecord does not exist yet at that level either way).
      d.getMetrics(m);
    }
    return new int[] {m.widthPixels, m.heightPixels};
  }

  /**
   * Scales the real display down to at most `maxSide` on the long edge, rounded to a multiple of 16
   * on both sides. Most H.264 hardware encoders work internally in 16x16 macroblocks and either
   * reject or silently re-pad/crop a size that isn't a multiple of 16 - which, left unhandled, means
   * the actual encoded frame can end up a different size than what was requested without saying so,
   * producing a corrupted-looking (diagonally torn) decoded picture. Requesting a 16-aligned size up
   * front avoids relying on that per-device fallback behavior at all.
   */
  private static int[] targetSize(Context context, int maxSide) {
    int[] real = realSize(context);
    float scale = Math.min(1f, maxSide / (float) Math.max(real[0], real[1]));
    int w = roundTo16(Math.round(real[0] * scale)), h = roundTo16(Math.round(real[1] * scale));
    return new int[] {Math.max(16, w), Math.max(16, h)};
  }

  private static int roundTo16(int v) {
    return ((v + 8) / 16) * 16;
  }

  // ---------------------------------------------------------------- API 19+: screenrecord / H.264

  private void streamLoop(int w, int h) throws Exception {
    sink.status("Starting screen stream...");
    int consecutiveFailures = 0;
    long lastStart = 0;
    while (!stop) {
      long now = SystemClock.elapsedRealtime();
      if (now - lastStart < 1500) {
        consecutiveFailures++;
        if (consecutiveFailures > 3)
          throw new IOException("screenrecord keeps failing to start; giving up");
      } else {
        consecutiveFailures = 0;
      }
      lastStart = now;
      runOneRecording(w, h);
      // screenrecord exited (time limit, or an error): loop again and start a fresh one, unless
      // we were asked to stop.
    }
  }

  private void runOneRecording(int w, int h) throws Exception {
    process =
        RootShell.startInteractive(
            "screenrecord --output-format=h264 --size " + w + "x" + h + " --bit-rate 2000000 -");
    try {
      InputStream in = process.getInputStream();
      ByteArrayOutputStream pending = new ByteArrayOutputStream();
      ByteArrayOutputStream config = new ByteArrayOutputStream();
      ByteArrayOutputStream prefix = new ByteArrayOutputStream();
      boolean sentFormat = false;
      boolean sawData = false;
      long started = System.nanoTime();
      byte[] buf = new byte[32768];
      int n;
      while (!stop && (n = in.read(buf)) != -1) {
        if (n == 0) continue;
        sawData = true;
        pending.write(buf, 0, n);
        byte[] all = pending.toByteArray();
        List<Integer> marks = startCodes(all);
        if (marks.size() < 2) {
          if (all.length > 4 * 1024 * 1024)
            throw new IOException("No H.264 start codes seen in screen stream");
          continue;
        }
        for (int i = 0; i + 1 < marks.size(); i++) {
          int from = marks.get(i), to = marks.get(i + 1);
          // If the next start code is the 4-byte form (00 00 00 01), its extra leading zero is
          // sitting right before `to`; trim it so this unit ends exactly at its own payload, and
          // just drop that one filler byte (framing only, never part of any NAL's payload).
          if (to > from && all[to - 1] == 0) to--;
          if (to - from < 4) continue; // degenerate/empty NAL; ignore defensively
          int type = all[from + 3] & 0x1F;
          if (type == 7 || type == 8) {
            (sentFormat ? prefix : config).write(all, from, to - from);
          } else if (type == 1 || type == 5) {
            if (!sentFormat) {
              sink.format(true, w, h, config.toByteArray());
              sentFormat = true;
              config.reset();
            }
            ByteArrayOutputStream unit = new ByteArrayOutputStream();
            prefix.writeTo(unit);
            unit.write(all, from, to - from);
            prefix.reset();
            sink.frame(type == 5, (System.nanoTime() - started) / 1000, unit.toByteArray());
          } else {
            prefix.write(all, from, to - from);
          }
        }
        int tailStart = marks.get(marks.size() - 1);
        pending.reset();
        pending.write(all, tailStart, all.length - tailStart);
        if (prefix.size() > 262144) prefix.reset(); // guard against a runaway non-slice run
      }
      if (!sawData)
        throw new IOException("No data from screenrecord (root access or SELinux may be blocking it)");
    } finally {
      Process p = process;
      process = null;
      if (p != null) {
        try {
          p.getOutputStream().close();
        } catch (Exception ignored) {
        }
        p.destroy();
      }
    }
  }

  /**
   * Positions of each Annex-B start code (00 00 01 or 00 00 00 01) in `data`, pointing at its first
   * byte. The caller treats consecutive marks as [from, to) ranges covering one NAL unit each,
   * except the very last mark, which may still be incomplete and is kept for the next read.
   */
  private static List<Integer> startCodes(byte[] data) {
    List<Integer> r = new ArrayList<Integer>();
    int limit = data.length - 2;
    for (int i = 0; i < limit; i++) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        r.add(i);
        i += 2;
      }
    }
    return r;
  }

  // -------------------------------------------------------------- API 16-18: screencap snapshots

  private void snapshotLoop(int w, int h) throws Exception {
    sink.status("Starting screen capture...");
    boolean sentFormat = false;
    long started = System.nanoTime();
    int consecutiveFailures = 0;
    while (!stop) {
      long t0 = SystemClock.elapsedRealtime();
      byte[] png;
      try {
        png = RootShell.runBytes("screencap -p", 4000);
        consecutiveFailures = 0;
      } catch (Exception e) {
        if (++consecutiveFailures > 5) throw e;
        Thread.sleep(500);
        continue;
      }
      Bitmap captured = BitmapFactory.decodeByteArray(png, 0, png.length);
      if (captured == null) {
        Thread.sleep(200);
        continue;
      }
      Bitmap scaled = captured;
      if (captured.getWidth() != w || captured.getHeight() != h) {
        scaled = Bitmap.createScaledBitmap(captured, w, h, true);
        captured.recycle();
      }
      ByteArrayOutputStream jpeg = new ByteArrayOutputStream();
      scaled.compress(Bitmap.CompressFormat.JPEG, 55, jpeg);
      scaled.recycle();
      if (!sentFormat) {
        sink.format(false, w, h, null);
        sentFormat = true;
      }
      sink.frame(true, (System.nanoTime() - started) / 1000, jpeg.toByteArray());
      long elapsed = SystemClock.elapsedRealtime() - t0;
      long sleep = 220 - elapsed;
      if (sleep > 0) Thread.sleep(sleep);
    }
  }
}
