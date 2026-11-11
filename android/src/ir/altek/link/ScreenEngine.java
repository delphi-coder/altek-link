package ir.altek.link;

import java.io.*;
import java.util.concurrent.*;
import javax.net.ssl.SSLSocket;

/**
 * Carries the "screen" call's two channels over one TLS socket: screen frames flow from the shared
 * device to the controller, and gestures flow the other way. A viewer passes a {@link FrameListener}
 * and null for {@link ControlListener}; the device being controlled passes the reverse. The read
 * loop always dispatches both packet kinds and simply ignores whichever listener is null, so one
 * class covers both ends.
 *
 * <p>Wire format: each packet (already length-framed by {@link Wire}) starts with a kind byte:
 * 0 = keepalive, 1 = format, 2 = frame, 3 = control. This is a private sub-protocol for this one
 * socket, independent of {@link MediaEngine}'s own packet kinds on audio/video call sockets.
 */
public final class ScreenEngine {
  public interface FrameListener {
    /** config is SPS/PPS bytes when h264 is true, otherwise null (JPEG frames). */
    void format(boolean h264, int w, int h, byte[] config);

    void frame(boolean keyFrame, long ptsMicros, byte[] bytes);
  }

  public interface ControlListener {
    void tap(float x, float y);

    void swipe(float x1, float y1, float x2, float y2, int durationMs);

    /** action is one of AccessibilityService.GLOBAL_ACTION_BACK/HOME/RECENTS. */
    void key(int action);

    /** scale>1 = zoom in, scale<1 = zoom out. cx/cy are normalized midpoint. */
    void pinch(float cx, float cy, float scale);

    /* Raw mouse event stream: op=5 down, op=6 move, op=7 up, op=8 right down, op=9 right up.
     * Coordinates are normalized 0..1 on the remote screen. */
    void mouseDown(float x, float y);

    void mouseMove(float x, float y);

    void mouseUp(float x, float y);

    void mouseRightDown(float x, float y);

    void mouseRightUp(float x, float y);

    /** Hardware key forwarding: PC virtual-key code and down(true)/up(false). */
    void keyEvent(int vk, int down);

    void textInput(String text);
  }

  public interface Listener {
    void status(String text);

    void ended();
  }

  private static final int KIND_FORMAT = 1, KIND_FRAME = 2, KIND_CONTROL = 3;

  private final SSLSocket socket;
  private final FrameListener frames;
  private final ControlListener control;
  private final Listener listener;
  private volatile boolean running = true;
  private volatile boolean peerAcceptsText;
  private volatile ScreenClipboard clipboard;
  private int pendingClipboardMode = -1;
  private String pendingClipboardText;
  // Controls have priority, and either queue wakes the writer immediately.
  private final ScreenPacketQueue packets = new ScreenPacketQueue();

  public ScreenEngine(
      SSLSocket socket, FrameListener frames, ControlListener control, Listener listener) {
    this.socket = socket;
    this.frames = frames;
    this.control = control;
    this.listener = listener;
    Thread writer =
        new Thread(
            new Runnable() {
              public void run() {
                writeLoop();
              }
            },
            "screen-tx");
    writer.start();
    new Thread(
            new Runnable() {
              public void run() {
                readLoop();
              }
            },
            "screen-rx")
        .start();
  }

  public synchronized void attachClipboard(ScreenClipboard value) {
    clipboard = value;
    if (pendingClipboardMode >= 0) value.peerMode(pendingClipboardMode);
    if (pendingClipboardText != null) value.receive(pendingClipboardText);
    pendingClipboardMode = -1;
    pendingClipboardText = null;
  }
  private synchronized void peerClipboardMode(int value) {
    if (clipboard == null) pendingClipboardMode = value;
    else clipboard.peerMode(value);
  }
  private synchronized void peerClipboardText(String text) {
    if (clipboard == null) pendingClipboardText = text;
    else clipboard.receive(text);
  }
  public void sendClipboardMode(int mode) { enqueueControl(new byte[] {3, 17, (byte) mode}); }
  public boolean sendClipboard(String text) {
    try {
      byte[] bytes = text.getBytes("UTF-8");
      if (bytes.length > 65536) return false;
      byte[] packet = new byte[bytes.length + 2];
      packet[0] = 3; packet[1] = 18;
      System.arraycopy(bytes, 0, packet, 2, bytes.length);
      enqueueControl(packet);
      return true;
    } catch (Exception e) { return false; }
  }

  public void sendTextCapability() {
    enqueueControl(new byte[] { KIND_CONTROL, 16, 1 });
  }

  public boolean peerAcceptsText() { return peerAcceptsText; }

  static byte[] textPacket(String text) throws UnsupportedEncodingException {
    byte[] bytes = text.getBytes("UTF-8");
    if (bytes.length == 0 || bytes.length > 64) return null;
    byte[] packet = new byte[bytes.length + 2];
    packet[0] = KIND_CONTROL;
    packet[1] = 15;
    System.arraycopy(bytes, 0, packet, 2, bytes.length);
    return packet;
  }

  public void sendText(String text) {
    try {
      byte[] packet = textPacket(text);
      if (packet != null) enqueueControl(packet);
    } catch (Exception e) { error(e); }
  }

  public void sendFormat(boolean h264, int w, int h, byte[] config) {
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      DataOutputStream o = new DataOutputStream(b);
      o.writeByte(KIND_FORMAT);
      o.writeByte(h264 ? 1 : 0);
      o.writeInt(w);
      o.writeInt(h);
      if (h264 && config != null) o.write(config);
      if (running) packets.offerFormat(b.toByteArray(), h264);
    } catch (Exception e) {
      error(e);
    }
  }

  public boolean sendFrame(boolean keyFrame, long ptsMicros, byte[] bytes) {
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      DataOutputStream o = new DataOutputStream(b);
      o.writeByte(KIND_FRAME);
      o.writeByte(keyFrame ? 1 : 0);
      o.writeLong(ptsMicros);
      o.write(bytes);
      return running && packets.offerFrame(b.toByteArray(), keyFrame);
    } catch (Exception e) {
      error(e);
      return false;
    }
  }

  public void sendTap(float x, float y) {
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      DataOutputStream o = new DataOutputStream(b);
      o.writeByte(KIND_CONTROL);
      o.writeByte(1);
      o.writeFloat(x);
      o.writeFloat(y);
      enqueueControl(b.toByteArray());
    } catch (Exception e) {
      error(e);
    }
  }

  public void sendKey(int action) {
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      DataOutputStream o = new DataOutputStream(b);
      o.writeByte(KIND_CONTROL);
      o.writeByte(3);
      o.writeInt(action);
      enqueueControl(b.toByteArray());
    } catch (Exception e) {
      error(e);
    }
  }

  public void sendKeyCode(int vk, boolean down) {
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      DataOutputStream o = new DataOutputStream(b);
      o.writeByte(KIND_CONTROL);
      o.writeByte(13);
      o.writeInt(vk);
      o.writeInt(down ? 1 : 0);
      enqueueControl(b.toByteArray());
    } catch (Exception e) {
      error(e);
    }
  }

  public void sendSwipe(float x1, float y1, float x2, float y2, int durationMs) {
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      DataOutputStream o = new DataOutputStream(b);
      o.writeByte(KIND_CONTROL);
      o.writeByte(2);
      o.writeFloat(x1);
      o.writeFloat(y1);
      o.writeFloat(x2);
      o.writeFloat(y2);
      o.writeInt(durationMs);
      enqueueControl(b.toByteArray());
    } catch (Exception e) {
      error(e);
    }
  }

  /** Multiplicative pinch zoom (like Gallery pinch). scale>1 = zoom in, scale<1 = zoom out. Coordinates
   * are the pinch midpoint, normalized 0..1 relative to the displayed view. */
  public void sendPinch(float cx, float cy, float scale) {
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      DataOutputStream o = new DataOutputStream(b);
      o.writeByte(KIND_CONTROL);
      o.writeByte(4);
      o.writeFloat(cx);
      o.writeFloat(cy);
      o.writeFloat(scale);
      enqueueControl(b.toByteArray());
    } catch (Exception e) {
      error(e);
    }
  }

  public void sendMouseDown(float x, float y) {
    sendMouse(5, x, y);
  }

  public void sendMouseMove(float x, float y) {
    sendMouse(6, x, y);
  }

  public void sendMouseUp(float x, float y) {
    sendMouse(7, x, y);
  }

  public void sendRightDown(float x, float y) {
    sendMouse(8, x, y);
  }

  public void sendRightUp(float x, float y) {
    sendMouse(9, x, y);
  }

  private void sendMouse(int op, float x, float y) {
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      DataOutputStream o = new DataOutputStream(b);
      o.writeByte(KIND_CONTROL);
      o.writeByte(op);
      o.writeFloat(x);
      o.writeFloat(y);
      enqueueControl(b.toByteArray());
    } catch (Exception e) {
      error(e);
    }
  }

  private void enqueueControl(byte[] b) {
    if (running && !packets.offerControl(b)) error(new IOException("Control channel too slow"));
  }

  private void writeLoop() {
    try {
      DataOutputStream out = new DataOutputStream(socket.getOutputStream());
      while (running) {
        byte[] b = packets.take(2000);
        if (!running) break;
        if (b == null) b = new byte[] {0};
        Wire.write(out, b);
      }
    } catch (Exception e) {
      if (running) error(e);
    }
  }

  private void readLoop() {
    try {
      DataInputStream in = new DataInputStream(socket.getInputStream());
      while (running) {
        byte[] b = Wire.read(in, Wire.MAX_MEDIA);
        if (b.length < 1) throw new IOException("Empty screen packet");
        DataInputStream packet = new DataInputStream(new ByteArrayInputStream(b));
        int kind = packet.readUnsignedByte();
        DebugLog.event("SCREEN_PACKET", "kind=" + kind + " len=" + b.length);
        if (kind == KIND_FORMAT) {
          boolean h264 = packet.readUnsignedByte() != 0;
          int w = packet.readInt(), h = packet.readInt();
          byte[] config = h264 ? new byte[b.length - 10] : null;
          if (config != null) packet.readFully(config);
          DebugLog.event("SCREEN_FORMAT", "h264=" + h264 + " " + w + "x" + h + " config=" + (config == null ? 0 : config.length));
          if (frames != null) frames.format(h264, w, h, config);
        } else if (kind == KIND_FRAME) {
          boolean key = packet.readUnsignedByte() != 0;
          long pts = packet.readLong();
          byte[] bytes = new byte[b.length - 10];
          packet.readFully(bytes);
          DebugLog.event("SCREEN_FRAME", "key=" + key + " pts=" + pts + " size=" + bytes.length);
          if (frames != null) frames.frame(key, pts, bytes);
        } else if (kind == KIND_CONTROL) {
          int op = packet.readUnsignedByte();
          if (op == 17 && b.length == 3) {
            peerClipboardMode(b[2]);
          } else if (op == 18 && b.length <= 65538) {
            peerClipboardText(new String(b, 2, b.length - 2, "UTF-8"));
          } else if (op == 16 && b.length == 3 && b[2] == 1) {
            peerAcceptsText = true;
          } else if (op == 1) {
            float x = packet.readFloat(), y = packet.readFloat();
            if (control != null) control.tap(x, y);
          } else if (op == 2) {
            float x1 = packet.readFloat(),
                y1 = packet.readFloat(),
                x2 = packet.readFloat(),
                y2 = packet.readFloat();
            int ms = packet.readInt();
            if (control != null) control.swipe(x1, y1, x2, y2, ms);
          } else if (op == 3) {
            int action = packet.readInt();
            if (control != null) control.key(action);
          } else if (op == 4) {
            float cx = packet.readFloat(), cy = packet.readFloat(), scale = packet.readFloat();
            if (control != null) control.pinch(cx, cy, scale);
          } else if (op == 5) {
            float x = packet.readFloat(), y = packet.readFloat();
            if (control != null) control.mouseDown(x, y);
          } else if (op == 6) {
            float x = packet.readFloat(), y = packet.readFloat();
            if (control != null) control.mouseMove(x, y);
          } else if (op == 7) {
            float x = packet.readFloat(), y = packet.readFloat();
            if (control != null) control.mouseUp(x, y);
          } else if (op == 8) {
            float x = packet.readFloat(), y = packet.readFloat();
            if (control != null) control.mouseRightDown(x, y);
          } else if (op == 9) {
            float x = packet.readFloat(), y = packet.readFloat();
            if (control != null) control.mouseRightUp(x, y);
          } else if (op == 15 && b.length > 2 && b.length <= 66) {
            byte[] text = new byte[b.length - 2];
            packet.readFully(text);
            if (control != null) control.textInput(new String(text, "UTF-8"));
          } else if (op == 13) {
            int vk = packet.readInt(), down = packet.readInt();
            if (control != null) control.keyEvent(vk, down);
          }
        } else if (kind != 0) throw new IOException("Screen packet type");
      }
    } catch (Exception e) {
      if (running) error(e);
    }
  }

  private void error(Exception e) {
    DebugLog.error("SCREEN_ENGINE", e);
    stop();
    listener.ended();
  }

  public void stop() {
    if (!running) return;
    running = false;
    packets.close();
    Wire.close(socket);
  }
}
