package ir.altek.link;

import android.graphics.Rect;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.IBinder;
import android.view.Surface;
import java.io.BufferedOutputStream;
import java.io.DataOutputStream;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.nio.Buffer;
import java.nio.ByteBuffer;

/** API-19/root helper. Loaded by app_process under the shell SELinux context, not by the app VM. */
public final class RootScreenHelper {
  private static final int MAGIC = 0x414C5331; // ALS1

  private static byte[] csd(MediaFormat format, String key) {
    if (!format.containsKey(key)) return new byte[0];
    ByteBuffer buffer = format.getByteBuffer(key).duplicate();
    byte[] bytes = new byte[buffer.remaining()];
    buffer.get(bytes);
    return bytes;
  }

  private static void format(DataOutputStream out, int width, int height, byte[] config)
      throws Exception {
    out.writeByte(1);
    out.writeInt(width);
    out.writeInt(height);
    out.writeInt(config.length);
    out.write(config);
    out.flush();
  }

  private static void frame(DataOutputStream out, boolean key, long pts, byte[] bytes)
      throws Exception {
    out.writeByte(2);
    out.writeByte(key ? 1 : 0);
    out.writeLong(pts);
    out.writeInt(bytes.length);
    out.write(bytes);
    out.flush();
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 8 && args.length != 9)
      throw new IllegalArgumentException(
          "width height bitrate fps key sourceW sourceH rotation [codec]");
    final int width = Integer.parseInt(args[0]), height = Integer.parseInt(args[1]);
    final int bitrate = Integer.parseInt(args[2]), fps = Integer.parseInt(args[3]);
    final int keySeconds = Integer.parseInt(args[4]);
    final int sourceWidth = Integer.parseInt(args[5]), sourceHeight = Integer.parseInt(args[6]);
    final int rotation = Integer.parseInt(args[7]);
    // The omitted/default value deliberately preserves the established Huawei Y520/Y600 path.
    final String codecName = args.length == 9 ? args[8] : "OMX.MTK.VIDEO.ENCODER.AVC";
    MediaCodec codec = null;
    Surface surface = null;
    IBinder display = null;
    Class<?> control = Class.forName("android.view.SurfaceControl");
    Method create = control.getMethod("createDisplay", String.class, Boolean.TYPE);
    Method destroy = control.getMethod("destroyDisplay", IBinder.class);
    Method open = control.getMethod("openTransaction");
    Method close = control.getMethod("closeTransaction");
    Method setSurface = control.getMethod("setDisplaySurface", IBinder.class, Surface.class);
    Method setProjection = control.getMethod(
        "setDisplayProjection", IBinder.class, Integer.TYPE, Rect.class, Rect.class);
    Method setStack = control.getMethod("setDisplayLayerStack", IBinder.class, Integer.TYPE);
    DataOutputStream out = new DataOutputStream(
        new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 65536));
    try {
      MediaFormat requested = MediaFormat.createVideoFormat("video/avc", width, height);
      requested.setInteger(
          MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
      requested.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
      requested.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
      requested.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, keySeconds);
      codec = MediaCodec.createByCodecName(codecName);
      codec.configure(requested, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
      surface = codec.createInputSurface();
      codec.start();
      display = (IBinder) create.invoke(null, "ALTEKLinkRootCapture", false);
      open.invoke(null);
      try {
        setProjection.invoke(null, display, rotation, new Rect(0, 0, sourceWidth, sourceHeight),
            new Rect(0, 0, width, height));
        setStack.invoke(null, display, 0);
      } finally {
        close.invoke(null);
      }

      out.writeInt(MAGIC);
      out.flush();
      ByteBuffer[] buffers = codec.getOutputBuffers();
      MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
      boolean sentFormat = false, attached = false;
      long nextFrame = 0;
      while (true) {
        long now = android.os.SystemClock.elapsedRealtime();
        if (!attached && now >= nextFrame) {
          open.invoke(null);
          try { setSurface.invoke(null, display, surface); } finally { close.invoke(null); }
          attached = true;
        }
        int index = codec.dequeueOutputBuffer(info, 10000);
        if (index == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
          buffers = codec.getOutputBuffers();
        } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
          MediaFormat actual = codec.getOutputFormat();
          byte[] a = csd(actual, "csd-0"), b = csd(actual, "csd-1");
          byte[] config = new byte[a.length + b.length];
          System.arraycopy(a, 0, config, 0, a.length);
          System.arraycopy(b, 0, config, a.length, b.length);
          if (config.length > 0) {
            format(out, width, height, config);
            sentFormat = true;
          }
        } else if (index >= 0) {
          try {
            if (info.size > 0) {
              ByteBuffer buffer = buffers[index].duplicate();
              ((Buffer) buffer).position(info.offset);
              ((Buffer) buffer).limit(info.offset + info.size);
              byte[] bytes = new byte[info.size];
              buffer.get(bytes);
              if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                format(out, width, height, bytes);
                sentFormat = true;
              } else {
                if (!sentFormat) throw new IllegalStateException("frame before codec config");
                frame(out, (info.flags & MediaCodec.BUFFER_FLAG_SYNC_FRAME) != 0,
                    info.presentationTimeUs, bytes);
                if (attached) {
                  open.invoke(null);
                  try { setSurface.invoke(null, new Object[] {display, null}); }
                  finally { close.invoke(null); }
                  attached = false;
                  nextFrame = android.os.SystemClock.elapsedRealtime() + Math.max(1, 1000 / fps);
                }
              }
            }
          } finally {
            codec.releaseOutputBuffer(index, false);
          }
        }
      }
    } finally {
      if (display != null) try { destroy.invoke(null, display); } catch (Throwable ignored) {}
      if (codec != null) {
        try { codec.stop(); } catch (Throwable ignored) {}
        try { codec.release(); } catch (Throwable ignored) {}
      }
      if (surface != null) try { surface.release(); } catch (Throwable ignored) {}
    }
  }
}
