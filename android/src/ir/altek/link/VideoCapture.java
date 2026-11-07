package ir.altek.link;

import android.graphics.*;
import android.hardware.Camera;
import android.media.*;
import android.os.*;
import java.nio.*;
import java.util.*;

/** Camera1 + MediaCodec path evolved from tested CamTest. Single owner thread, API16. */
public final class VideoCapture implements Camera.PreviewCallback {
  public interface Sink {
    void format(int w, int h, byte[] config);

    void frame(long pts, int flags, byte[] data);

    void error(Exception e);

    void status(String text);
  }

  private final Sink sink;
  private final android.content.Context context;
  private VideoOptions options;
  private long framePeriod = 100000000L;
  private long cameraFrames;
  private final HandlerThread thread = new HandlerThread("video-capture");
  private Handler h;
  private Camera camera;
  private SurfaceTexture texture;
  private final SurfaceTexture previewTexture;
  private MediaCodec codec;
  private ByteBuffer[] input, output;
  private int width, height, color, cameraId;
  private byte[] converted, config;
  private boolean active, frontCamera;
  private volatile boolean torchOn;
  private boolean defaultApplied;

  public boolean torchOn() {
    return torchOn;
  }

  private volatile boolean closed;
  private long next, origin, stats, frames, bytes;
  private final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();

  public VideoCapture(android.content.Context context, SurfaceTexture previewTexture, Sink sink) {
    this.previewTexture = previewTexture;
    this.context = context.getApplicationContext();
    this.sink = sink;
    thread.start();
    h = new Handler(thread.getLooper());
    h.post(
        new Runnable() {
          public void run() {
            open();
          }
        });
  }

  private final Runnable pump =
      new Runnable() {
        public void run() {
          if (!active || closed) return;
          try {
            drain();
            long now = SystemClock.elapsedRealtime();
            if (now - stats >= 2000) {
              DebugLog.event(
                  "VIDEO_TX",
                  "fps="
                      + (frames * 1000 / (now - stats))
                      + " cameraFPS="
                      + (cameraFrames * 1000 / (now - stats))
                      + " target="
                      + options.fps
                      + " payloadKBps="
                      + (bytes * 1000 / (now - stats) / 1024));
              stats = now;
              frames = bytes = cameraFrames = 0;
            }
            h.postDelayed(this, 10);
          } catch (Exception e) {
            fail(e);
          }
        }
      };

  private void open() {
    open(false);
  }

  private void open(boolean fallback) {
    // Apply front-camera default only once per session.
    // After that, cameraId is preserved from switchCamera() so the
    // button toggles correctly between front and rear.
    if (!defaultApplied && cameraId == 0) {
      int numCameras = Camera.getNumberOfCameras();
      for (int i = 0; i < numCameras; i++) {
        Camera.CameraInfo info = new Camera.CameraInfo();
        Camera.getCameraInfo(i, info);
        if (info.facing == Camera.CameraInfo.CAMERA_FACING_FRONT) {
          cameraId = i;
          defaultApplied = true;
          break;
        }
      }
    }
    options = fallback ? new VideoOptions() : VideoOptions.load(context, cameraId);
    framePeriod = VideoOptions.periodNs(options.fps);
    try {
      if (closed) return;
      camera = Camera.open(cameraId);
      Camera.CameraInfo details = new Camera.CameraInfo();
      Camera.getCameraInfo(cameraId, details);
      frontCamera = details.facing == Camera.CameraInfo.CAMERA_FACING_FRONT;
      DebugLog.event(
          "CAMERA_ORIENTATION",
          "id="
              + cameraId
              + " front="
              + frontCamera
              + " sensor="
              + details.orientation
              + " inputRotate="
              + (frontCamera ? 180 : 0));
      camera.setErrorCallback(
          new Camera.ErrorCallback() {
            public void onError(int err, Camera c) {
              fail(new Exception("Camera error " + err));
            }
          });
      Camera.Parameters p = camera.getParameters();
      Camera.Size best = null;
      long score = Long.MAX_VALUE;
      for (Camera.Size s : p.getSupportedPreviewSizes()) {
        if ((s.width & 1) != 0 || (s.height & 1) != 0) continue;
        long d =
            Math.abs((long) s.width * s.height - 320 * 240)
                + Math.abs(s.width * 3L - s.height * 4L) * 100;
        if (d < score) {
          score = d;
          best = s;
        }
      }
      if (options.size.length() > 0) {
        best = null;
        for (Camera.Size size : p.getSupportedPreviewSizes())
          if (options.size.equals(size.width + "x" + size.height)) {
            best = size;
            break;
          }
      }
      if (best == null) throw new Exception("No compatible preview");
      if (options.range.length() > 0) {
        boolean supported = false;
        List<int[]> ranges = p.getSupportedPreviewFpsRange();
        if (ranges != null)
          for (int[] range : ranges)
            if (options.range.equals(range[0] + "," + range[1])) {
              p.setPreviewFpsRange(range[0], range[1]);
              supported = true;
              break;
            }
        if (!supported) throw new Exception("Unsupported FPS range");
      }
      if (options.focus.length() > 0) {
        List<String> modes = p.getSupportedFocusModes();
        if (modes == null || !modes.contains(options.focus))
          throw new Exception("Unsupported focus");
        p.setFocusMode(options.focus);
      }
      p.setPreviewSize(best.width, best.height);
      p.setPreviewFormat(ImageFormat.NV21);
      camera.setParameters(p);
      Camera.Size previewSize = camera.getParameters().getPreviewSize();
      width = previewSize.width;
      height = previewSize.height;
      if (width > 4096
          || height > 4096
          || (width & 1) != 0
          || (height & 1) != 0
          || (long) width * height * 6 > Runtime.getRuntime().maxMemory() / 2)
        throw new Exception("Video memory limit");
      converted = new byte[width * height * 3 / 2];
      encoder();
      texture = previewTexture != null ? previewTexture : new SurfaceTexture(10);
      if (previewTexture != null) {
        int rotation = ((android.view.WindowManager) context.getSystemService(android.content.Context.WINDOW_SERVICE)).getDefaultDisplay().getRotation();
        int degrees = rotation == 1 ? 90 : rotation == 2 ? 180 : rotation == 3 ? 270 : 0;
        int orientation = frontCamera ? (360 - (details.orientation + degrees) % 360) % 360 : (details.orientation - degrees + 360) % 360;
        camera.setDisplayOrientation(orientation);
      }
      camera.setPreviewTexture(texture);
      for (int i = 0; i < 3; i++) camera.addCallbackBuffer(new byte[converted.length]);
      camera.setPreviewCallbackWithBuffer(this);
      origin = next = System.nanoTime();
      stats = SystemClock.elapsedRealtime();
      active = true;
      camera.startPreview();
      if (Camera.Parameters.FOCUS_MODE_AUTO.equals(options.focus))
        try {
          camera.autoFocus(null);
        } catch (Exception ignored) {
        }
      frames = bytes = cameraFrames = 0;
      h.post(pump);
      int[] range = new int[2];
      Camera.Parameters applied = camera.getParameters();
      applied.getPreviewFpsRange(range);
      String actual =
          (frontCamera ? "Front" : "Rear")
              + " #"
              + cameraId
              + " "
              + width
              + "x"
              + height
              + " sendFPS="
              + options.fps
              + " cameraRange="
              + (range[0] / 1000f)
              + "-"
              + (range[1] / 1000f)
              + " requestedKbps="
              + (options.bitrate / 1000)
              + " key="
              + options.keySeconds
              + " focus="
              + applied.getFocusMode()
              + (fallback ? " [FALLBACK]" : "");
      context
          .getSharedPreferences("video_actual", 0)
          .edit()
          .putString("camera" + cameraId, actual)
          .apply();
      DebugLog.event("VIDEO_CAPTURE", actual + " color=" + color);
      sink.status(actual);
    } catch (Exception e) {
      startupFailure(e, fallback);
    } catch (OutOfMemoryError e) {
      startupFailure(new Exception("Camera memory exhausted"), fallback);
    }
  }

  private void startupFailure(Exception e, boolean fallback) {
    if (!closed && !fallback && !options.isDefault()) {
      DebugLog.error("VIDEO_CUSTOM", e);
      DebugLog.event("VIDEO_FALLBACK", "camera=" + cameraId + " retry tested defaults");
      cleanup();
      open(true);
    } else fail(e);
  }

  private void encoder() throws Exception {
    for (int i = 0; i < MediaCodecList.getCodecCount(); i++) {
      MediaCodecInfo c = MediaCodecList.getCodecInfoAt(i);
      if (!c.isEncoder()) continue;
      for (String type : c.getSupportedTypes())
        if (type.equalsIgnoreCase("video/avc")) {
          int[] supported = c.getCapabilitiesForType(type).colorFormats;
          for (int f : new int[] {21, 19}) {
            boolean yes = false;
            for (int value : supported) if (value == f) yes = true;
            if (!yes) continue;
            MediaCodec candidate = null;
            try {
              candidate = MediaCodec.createByCodecName(c.getName());
              MediaFormat m = MediaFormat.createVideoFormat(type, width, height);
              m.setInteger(MediaFormat.KEY_COLOR_FORMAT, f);
              m.setInteger(MediaFormat.KEY_BIT_RATE, options.bitrate);
              m.setInteger(MediaFormat.KEY_FRAME_RATE, options.fps);
              m.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, options.keySeconds);
              candidate.configure(m, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
              candidate.start();
              input = candidate.getInputBuffers();
              output = candidate.getOutputBuffers();
              codec = candidate;
              color = f;
              DebugLog.event("ENCODER", c.getName());
              return;
            } catch (Exception | OutOfMemoryError e) {
              DebugLog.error("ENCODER_TRY", e);
              if (candidate != null) {
                try {
                  candidate.stop();
                } catch (Exception ignored) {
                }
                try {
                  candidate.release();
                } catch (Exception ignored) {
                }
              }
            }
          }
        }
    }
    throw new Exception("No compatible H264 encoder");
  }

  public void onPreviewFrame(byte[] data, Camera source) {
    if (!active || closed) return;
    try {
      cameraFrames++;
      if (data == null || data.length < converted.length) throw new Exception("Camera buffer");
      drain();
      long now = System.nanoTime();
      if (now < next) return;
      int index = codec.dequeueInputBuffer(0);
      if (index < 0) return;
      ByteBuffer b = input[index];
      ((Buffer) b).clear();
      if (b.remaining() < converted.length) throw new Exception("Codec buffer too small");
      convert(data, converted, width, height, color == 19, frontCamera);
      b.put(converted);
      codec.queueInputBuffer(index, 0, converted.length, (now - origin) / 1000, 0);
      next += framePeriod;
      if (next <= now) next = now + framePeriod;
      drain();
    } catch (Exception e) {
      fail(e);
    } finally {
      if (active && !closed && camera == source && data != null)
        try {
          source.addCallbackBuffer(data);
        } catch (Exception e) {
          fail(e);
        }
    }
  }

  private void drain() throws Exception {
    for (int i = 0; i < 16 && !closed; i++) {
      int n = codec.dequeueOutputBuffer(info, 0);
      if (n == MediaCodec.INFO_TRY_AGAIN_LATER) return;
      if (n == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
        output = codec.getOutputBuffers();
        continue;
      }
      if (n == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
        MediaFormat f = codec.getOutputFormat();
        byte[] a = csd(f, "csd-0"), b = csd(f, "csd-1");
        if (a.length + b.length > 0) {
          config = new byte[a.length + b.length];
          System.arraycopy(a, 0, config, 0, a.length);
          System.arraycopy(b, 0, config, a.length, b.length);
          sink.format(width, height, config);
        }
        continue;
      }
      if (n < 0) return;
      try {
        if (info.size == 0) continue;
        ByteBuffer b = output[n].duplicate();
        ((Buffer) b).position(info.offset);
        ((Buffer) b).limit(info.offset + info.size);
        byte[] out = new byte[info.size];
        b.get(out);
        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
          config = out;
          sink.format(width, height, config);
        } else {
          sink.frame(info.presentationTimeUs, info.flags, out);
          frames++;
          bytes += out.length;
        }
      } finally {
        codec.releaseOutputBuffer(n, false);
      }
    }
  }

  private byte[] csd(MediaFormat f, String key) {
    if (!f.containsKey(key)) return new byte[0];
    ByteBuffer b = f.getByteBuffer(key).duplicate();
    byte[] x = new byte[b.remaining()];
    b.get(x);
    return x;
  }

  public void torch() {
    h.post(
        new Runnable() {
          public void run() {
            try {
              if (camera == null) return;
              Camera.Parameters p = camera.getParameters();
              List<String> supported = p.getSupportedFlashModes();
              if (supported == null || !supported.contains(Camera.Parameters.FLASH_MODE_TORCH))
                return;
              p.setFlashMode(
                  Camera.Parameters.FLASH_MODE_TORCH.equals(p.getFlashMode())
                      ? Camera.Parameters.FLASH_MODE_OFF
                      : Camera.Parameters.FLASH_MODE_TORCH);
              camera.setParameters(p);
              torchOn =
                  Camera.Parameters.FLASH_MODE_TORCH.equals(camera.getParameters().getFlashMode());
            } catch (Exception e) {
              DebugLog.error("TORCH", e);
            }
          }
        });
  }

  public void switchCamera() {
    h.post(
        new Runnable() {
          public void run() {
            if (closed) return;
            try {
              if (Camera.getNumberOfCameras() < 2) return;
              cleanup();
              cameraId = (cameraId + 1) % Camera.getNumberOfCameras();
              open();
            } catch (Exception e) {
              fail(e);
            }
          }
        });
  }

  private void fail(Exception e) {
    DebugLog.error("VIDEO_CAPTURE", e);
    cleanup();
    sink.error(e);
  }

  private void cleanup() {
    active = false;
    torchOn = false;
    h.removeCallbacks(pump);
    if (camera != null) {
      try {
        camera.setPreviewCallbackWithBuffer(null);
        camera.stopPreview();
      } catch (Exception ignored) {
      }
      try {
        camera.release();
      } catch (Exception ignored) {
      }
      camera = null;
    }
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
    if (texture != null && texture != previewTexture) {
      try {
        texture.release();
      } catch (Exception ignored) {
      }
    }
    texture = null;
    config = null;
    converted = null;
    input = null;
    output = null;
  }

  public void stop() {
    closed = true;
    h.post(
        new Runnable() {
          public void run() {
            cleanup();
            thread.quit();
          }
        });
  }

  public static void convert(byte[] src, byte[] dst, int w, int h, boolean planar) {
    convert(src, dst, w, h, planar, false);
  }

  /**
   * Front-camera half turn is fused into the existing color conversion; no per-frame allocation.
   */
  public static void convert(
      byte[] src, byte[] dst, int w, int h, boolean planar, boolean halfTurn) {
    int y = w * h, c = y / 4;
    if (halfTurn) for (int i = 0; i < y; i++) dst[i] = src[y - 1 - i];
    else System.arraycopy(src, 0, dst, 0, y);
    for (int i = 0; i < c; i++) {
      int sample = halfTurn ? c - 1 - i : i;
      if (planar) {
        dst[y + i] = src[y + 2 * sample + 1];
        dst[y + c + i] = src[y + 2 * sample];
      } else {
        dst[y + 2 * i] = src[y + 2 * sample + 1];
        dst[y + 2 * i + 1] = src[y + 2 * sample];
      }
    }
  }
}
