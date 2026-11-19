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
import android.media.MediaExtractor;
import android.media.projection.MediaProjection;
import android.os.Build;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.Surface;
import android.view.Display;
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
 *   <li>API 21+ when the person declines the prompt: falls back to streaming real
 *       H.264 from the `screenrecord` shell tool (root required), parsed from its raw Annex-B
 *       elementary stream into access units compatible with {@link VideoDisplay}.
 *       `screenrecord` stops on its own after a time limit, so a session longer than that is
 *       continued by starting a fresh process; the decoder just gets a new format() call.
 *   <li>API 16-20: loops on `screencap -p` for compatibility with older screenrecord tools,
 *       decodes the PNG, scales it down, and sends it
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
  private final Context context;
  private volatile boolean stop;
  private volatile Process process;
  private final Thread thread;

  // MediaProjection-path state, released by stop().
  private MediaCodec encoder;
  private VirtualDisplay virtualDisplay;
  private MediaProjection projection;
  private ScreenOptions options;

  /** Root fallback: screenrecord (API 21+) or periodic screenshots (API 16-20). */
  public ScreenCapture(final Context context, final Sink sink) {
    this.sink = sink;
    this.context = context.getApplicationContext();
    this.options = ScreenOptions.load(context);
    // KitKat's screenrecord writes MP4 and does not support raw H.264 stdout.
    final boolean stream = Build.VERSION.SDK_INT >= 21;
    thread =
        new Thread(
            new Runnable() {
              public void run() {
                try {
                  sink.status("Waiting for root access (check your root manager app)...");
                  RootShell.Result root = RootShell.probe();
                  if (!root.granted) throw new IOException("Root access not available: " + root.detail);
                  int[] size = targetSize(context, stream ? options.maxSide : Math.min(options.maxSide, 720));
                  if (stream) streamLoop(size[0], size[1]);
                  else if (directRootCaptureDevice()) {
                    int[] directSize = samsungP1Device()
                        ? samsungP1TargetSize(context) : targetSize(context, options.maxSide);
                    try {
                      directRootLoop(directSize[0], directSize[1]);
                    } catch (Exception e) {
                      if (stop) return;
                      DebugLog.error("SCREEN_ROOT_DIRECT", e);
                      sink.status("Direct H.264 unavailable, using screenshots...");
                      snapshotLoop(size[0], size[1]);
                    }
                  } else snapshotLoop(size[0], size[1]);
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
    this.context = context.getApplicationContext();
    this.options = ScreenOptions.load(context);
    this.projection = projection;
    thread =
        new Thread(
            new Runnable() {
              public void run() {
                try {
                  int[] size = targetSize(context, options.maxSide);
                  projectionLoop(context, projection, size[0], size[1]);
                } catch (Exception e) {
                  if (!stop) sink.error(e);
                }
              }
            },
            "screen-capture-projection");
    thread.start();
  }

  /** Request an IDR after sender backpressure; root screenrecord has no direct control API. */
  public void requestSyncFrame() {
    MediaCodec current = encoder;
    if (Build.VERSION.SDK_INT < 19 || current == null || stop) return;
    try {
      android.os.Bundle params = new android.os.Bundle();
      params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
      current.setParameters(params);
    } catch (Exception e) {
      DebugLog.error("SCREEN_SYNC_REQUEST", e);
    }
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
        f.setInteger(MediaFormat.KEY_BIT_RATE, options.bitrate);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, options.fps);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, options.keySeconds);
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

  // --------------------------------------------------------- API 19 Y520: direct root Surface / H.264

  private boolean directRootCaptureDevice() {
    return (Build.VERSION.SDK_INT == 19 && "HUAWEI Y520-U22".equals(Build.MODEL))
        || (Build.VERSION.SDK_INT == 17 && "HUAWEI Y600-U20".equals(Build.MODEL))
        || samsungP1Device();
  }

  private static boolean samsungP1Device() {
    return Build.VERSION.SDK_INT == 19 && "SCH-I800".equals(Build.MODEL);
  }

  /** Encoder-safe, macroblock-aligned sizes for the SCH-I800's 800x600 AVC limit. */
  private static int[] samsungP1TargetSize(Context context) {
    int[] real = realSize(context);
    return real[0] <= real[1] ? new int[] {352, 592} : new int[] {800, 464};
  }

  private void installRootHelper() throws Exception {
    File jar = new File(context.getFilesDir(), "root-screen-helper.jar");
    InputStream input = context.getAssets().open("root-screen-helper.jar");
    FileOutputStream output = new FileOutputStream(jar, false);
    try {
      byte[] buffer = new byte[8192];
      int count;
      while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
      output.getFD().sync();
    } finally {
      try { output.close(); } finally { input.close(); }
    }
    File script = new File(context.getFilesDir(), "root-screen-helper.sh");
    output = new FileOutputStream(script, false);
    try {
      output.write(("#!/system/bin/sh\n"
          + "export CLASSPATH=/data/local/tmp/altek-root-screen.jar\n"
          // KitKat's app_process does not support the newer --nice-name option.
          + "exec /system/bin/app_process /system/bin "
          + "ir.altek.link.RootScreenHelper \"$@\"\n").getBytes("UTF-8"));
      output.getFD().sync();
    } finally {
      output.close();
    }
    String jarPath = jar.getAbsolutePath(), scriptPath = script.getAbsolutePath();
    if (jarPath.indexOf('\'') >= 0 || scriptPath.indexOf('\'') >= 0)
      throw new IOException("Invalid helper path");
    RootShell.run("cp '" + jarPath + "' /data/local/tmp/altek-root-screen.jar && cp '"
        + scriptPath + "' /data/local/tmp/altek-root-screen.sh && chmod 644 "
        + "/data/local/tmp/altek-root-screen.jar && chmod 755 /data/local/tmp/altek-root-screen.sh", 5000);
  }

  private void directRootLoop(int width, int height) throws Exception {
    installRootHelper();
    Display display = ((WindowManager) context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay();
    int rotation = display.getRotation();
    int[] current = realSize(context);
    int sourceWidth = (rotation == 1 || rotation == 3) ? current[1] : current[0];
    int sourceHeight = (rotation == 1 || rotation == 3) ? current[0] : current[1];
    String arguments = width + " " + height + " "
        + (samsungP1Device() ? Math.min(options.bitrate, 2000000) : options.bitrate) + " "
        + (samsungP1Device() ? Math.min(options.fps, 10) : options.fps) + " "
        + options.keySeconds + " " + sourceWidth + " " + sourceHeight + " " + rotation;
    // CyanogenMod's su on the SCH-I800 has no `su shell -c` form. Keep the proven Huawei
    // invocation byte-for-byte equivalent, and give only the Samsung branch its own codec/command.
    String command = samsungP1Device()
        ? "/system/bin/sh /data/local/tmp/altek-root-screen.sh " + arguments
            + " OMX.SEC.AVC.Encoder"
        : "su shell -c 'runcon u:r:shell:s0 /system/bin/sh "
            + "/data/local/tmp/altek-root-screen.sh " + arguments + "'";
    sink.status("Starting direct H.264 screen capture...");
    process = RootShell.startInteractive(command);
    DataInputStream input = new DataInputStream(new BufferedInputStream(process.getInputStream(), 65536));
    if (input.readInt() != 0x414C5331) throw new IOException("Root screen helper did not start");
    while (!stop) {
      int type = input.readUnsignedByte();
      if (type == 1) {
        int outWidth = input.readInt(), outHeight = input.readInt(), length = input.readInt();
        if (length <= 0 || length > 1048576) throw new IOException("Root screen format size");
        byte[] config = new byte[length];
        input.readFully(config);
        sink.format(true, outWidth, outHeight, config);
      } else if (type == 2) {
        boolean key = input.readUnsignedByte() != 0;
        long pts = input.readLong();
        int length = input.readInt();
        if (length <= 0 || length > 4194304) throw new IOException("Root screen frame size");
        byte[] bytes = new byte[length];
        input.readFully(bytes);
        sink.frame(key, pts, bytes);
      } else {
        throw new IOException("Root screen record type");
      }
    }
  }

  // ---------------------------------------------------------------- API 19+: screenrecord / H.264

  /** KitKat screenrecord only writes finalized MP4: send short recordings as H.264 segments. */
  private void legacyMp4Loop(Context context, int w, int h) throws Exception {
    File recording = new File(context.getCacheDir(), "screen-segment.mp4");
    if (!recording.exists() && !recording.createNewFile()) throw new IOException("Cannot create screen cache");
    String path = recording.getAbsolutePath();
    if (path.indexOf('\'') >= 0) throw new IOException("Invalid screen cache path");
    sink.status("Starting H.264 screen capture (short segments)...");
    long started = System.nanoTime();
    try {
      while (!stop) {
        new FileOutputStream(recording, false).close();
        RootShell.run("screenrecord --size " + w + "x" + h + " --bit-rate "
            + Math.min(options.bitrate, 2000000) + " --time-limit 2 '" + path + "'", 12000);
        if (stop) break;
        FileInputStream input = new FileInputStream(recording);
        MediaExtractor extractor = new MediaExtractor();
        try {
          extractor.setDataSource(input.getFD());
          int track = -1;
          MediaFormat format = null;
          for (int i = 0; i < extractor.getTrackCount(); i++) {
            MediaFormat f = extractor.getTrackFormat(i);
            if ("video/avc".equalsIgnoreCase(f.getString(MediaFormat.KEY_MIME))) {
              track = i;
              format = f;
              break;
            }
          }
          if (track < 0 || format == null) throw new IOException("No H.264 track in screen recording");
          byte[] sps = csd(format, "csd-0"), pps = csd(format, "csd-1");
          if (sps.length == 0 || pps.length == 0) throw new IOException("Missing H.264 configuration");
          DebugLog.event("SCREEN_LEGACY_H264", "track " + w + "x" + h + " sps=" + sps.length + " pps=" + pps.length);
          byte[] config = new byte[sps.length + pps.length];
          System.arraycopy(sps, 0, config, 0, sps.length);
          System.arraycopy(pps, 0, config, sps.length, pps.length);
          int fw = format.containsKey(MediaFormat.KEY_WIDTH) ? format.getInteger(MediaFormat.KEY_WIDTH) : w;
          int fh = format.containsKey(MediaFormat.KEY_HEIGHT) ? format.getInteger(MediaFormat.KEY_HEIGHT) : h;
          extractor.selectTrack(track);
          ByteBuffer sample = ByteBuffer.allocate(2 * 1024 * 1024);
          boolean sentFormat = false;
          long base = (System.nanoTime() - started) / 1000;
          long firstPts = -1, playbackStart = SystemClock.elapsedRealtime();
          while (!stop) {
            ((Buffer) sample).clear();
            int length = extractor.readSampleData(sample, 0);
            if (length < 0) break;
            if (length == 0) { extractor.advance(); continue; }
            long samplePts = Math.max(0, extractor.getSampleTime());
            if (firstPts < 0) firstPts = samplePts;
            long delay = Math.min(2000, Math.max(0, (samplePts - firstPts) / 1000))
                - (SystemClock.elapsedRealtime() - playbackStart);
            if (delay > 0) Thread.sleep(delay);
            if (stop) break;
            if (!sentFormat) {
              sink.format(true, fw, fh, config);
              sentFormat = true;
            }
            byte[] bytes = new byte[length];
            ((Buffer) sample).position(0);
            sample.get(bytes);
            boolean key = (extractor.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0;
            if (!sentFormat || key) DebugLog.event("SCREEN_LEGACY_H264", "frame bytes=" + length + " key=" + key);
            sink.frame(key, base + samplePts - firstPts, bytes);
            if (!extractor.advance()) break;
          }
          if (!sentFormat && !stop) throw new IOException("No H.264 frames in screen recording");
        } finally {
          extractor.release();
          input.close();
        }
      }
    } finally {
      recording.delete();
    }
  }

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
            "screenrecord --output-format=h264 --size " + w + "x" + h + " --bit-rate " + options.bitrate + " -");
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

  // -------------------------------------------------------------- API 16-20: screencap snapshots

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
