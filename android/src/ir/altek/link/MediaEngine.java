package ir.altek.link;

import android.content.Context;
import android.media.*;
import android.media.audiofx.*;
import android.view.Surface;
import java.io.*;
import java.util.concurrent.*;
import javax.net.ssl.SSLSocket;

/**
 * Version 1 media: framed H264 + 16 kHz mono PCM on a dedicated TLS socket. No RTP claims: reliable
 * TCP is an intentional LAN-first beta tradeoff.
 */
public final class MediaEngine {
  public interface Listener {
    void status(String text);

    void videoSize(int width, int height);

    void ended();
  }

  private final SSLSocket socket;
  private final Listener listener;
  private final Context context;
  private volatile boolean running = true, mute;
  private final BlockingQueue<byte[]> outgoing = new ArrayBlockingQueue<byte[]>(64),
      audioQueue = new ArrayBlockingQueue<byte[]>(16);
  private VideoCapture capture;
  private VideoDisplay display;
  private volatile AudioRecord record;
  private volatile AudioTrack playback;
  private AudioManager manager;
  private int oldMode;
  private boolean oldSpeaker;

  public MediaEngine(
      Context c,
      SSLSocket s,
      Surface surface,
      boolean sendVideo,
      boolean receiveVideo,
      boolean audio,
      Listener listener)
      throws Exception {
    this.context = c;
    this.socket = s;
    this.listener = listener;
    synchronized (this) {
      try {
        if (receiveVideo)
          display =
              new VideoDisplay(
                  surface,
                  new VideoDisplay.Listener() {
                    public void size(int w, int h) {
                      MediaEngine.this.listener.videoSize(w, h);
                    }
                  });
        Thread writer =
            new Thread(
                new Runnable() {
                  public void run() {
                    writeLoop();
                  }
                },
                "media-tx");
        writer.start();
        new Thread(
                new Runnable() {
                  public void run() {
                    readLoop();
                  }
                },
                "media-rx")
            .start();
        if (audio) {
          manager = (AudioManager) c.getSystemService(Context.AUDIO_SERVICE);
          oldMode = manager.getMode();
          oldSpeaker = manager.isSpeakerphoneOn();
          manager.setMode(AudioManager.MODE_IN_COMMUNICATION);
          manager.setSpeakerphoneOn(false);
          startAudio();
        }
        if (sendVideo)
          capture =
              new VideoCapture(
                  context,
                  new VideoCapture.Sink() {
                    public void format(int w, int h, byte[] config) {
                      try {
                        ByteArrayOutputStream b = new ByteArrayOutputStream();
                        DataOutputStream o = new DataOutputStream(b);
                        o.writeByte(1);
                        o.writeInt(w);
                        o.writeInt(h);
                        o.write(config);
                        enqueue(b.toByteArray());
                      } catch (Exception e) {
                        error(e);
                      }
                    }

                    public void frame(long pts, int flags, byte[] bytes) {
                      try {
                        ByteArrayOutputStream b = new ByteArrayOutputStream();
                        DataOutputStream o = new DataOutputStream(b);
                        o.writeByte(2);
                        o.writeLong(pts);
                        o.writeInt(flags);
                        o.write(bytes);
                        enqueue(b.toByteArray());
                      } catch (Exception e) {
                        error(e);
                      }
                    }

                    public void status(String text) {
                      MediaEngine.this.listener.status(text);
                    }

                    public void error(Exception e) {
                      MediaEngine.this.error(e);
                    }
                  });
        listener.status("Connected · encrypted TLS");
        DebugLog.event(
            "MEDIA_START", "videoTX=" + sendVideo + " videoRX=" + receiveVideo + " audio=" + audio);
      } catch (Exception e) {
        stop();
        throw e;
      }
    }
  }

  private void enqueue(byte[] b) {
    if (running && !outgoing.offer(b)) error(new IOException("Media link too slow"));
  }

  private void writeLoop() {
    try {
      DataOutputStream out = new DataOutputStream(socket.getOutputStream());
      while (running) {
        byte[] b = outgoing.poll(2, TimeUnit.SECONDS);
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
        if (b.length < 1) throw new IOException("Empty media packet");
        DataInputStream packet = new DataInputStream(new ByteArrayInputStream(b));
        int kind = packet.readUnsignedByte();
        if (kind == 1) {
          int w = packet.readInt(), h = packet.readInt();
          byte[] config = new byte[b.length - 9];
          packet.readFully(config);
          if (display != null) display.format(w, h, config);
        } else if (kind == 2) {
          long pts = packet.readLong();
          int flags = packet.readInt();
          byte[] frame = new byte[b.length - 13];
          packet.readFully(frame);
          if (display != null) display.frame(pts, flags, frame);
        } else if (kind == 3) {
          if (b.length > 4097 || (b.length - 1) % 2 != 0) throw new IOException("Audio packet");
          byte[] pcm = new byte[b.length - 1];
          packet.readFully(pcm);
          if (!audioQueue.offer(pcm)) {
            audioQueue.poll();
            audioQueue.offer(pcm);
          }
        } else if (kind != 0) throw new IOException("Media type");
      }
    } catch (Exception e) {
      if (running) error(e);
    }
  }

  private void startAudio() {
    new Thread(
            new Runnable() {
              public void run() {
                AudioRecord local = null;
                AcousticEchoCanceler echo = null;
                NoiseSuppressor noise = null;
                try {
                  int min =
                      AudioRecord.getMinBufferSize(
                          16000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
                  if (min < 0) throw new IOException("16kHz capture unsupported");
                  local =
                      new AudioRecord(
                          MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                          16000,
                          AudioFormat.CHANNEL_IN_MONO,
                          AudioFormat.ENCODING_PCM_16BIT,
                          Math.max(min, 6400));
                  if (local.getState() != AudioRecord.STATE_INITIALIZED) {
                    local.release();
                    local =
                        new AudioRecord(
                            MediaRecorder.AudioSource.MIC,
                            16000,
                            AudioFormat.CHANNEL_IN_MONO,
                            AudioFormat.ENCODING_PCM_16BIT,
                            Math.max(min, 6400));
                  }
                  record = local;
                  if (local.getState() != AudioRecord.STATE_INITIALIZED)
                    throw new IOException("Microphone unavailable");
                  try {
                    if (AcousticEchoCanceler.isAvailable()) {
                      echo = AcousticEchoCanceler.create(local.getAudioSessionId());
                      if (echo != null) echo.setEnabled(true);
                    }
                    if (NoiseSuppressor.isAvailable()) {
                      noise = NoiseSuppressor.create(local.getAudioSessionId());
                      if (noise != null) noise.setEnabled(true);
                    }
                  } catch (Exception optionalEffect) {
                    DebugLog.error("AUDIO_EFFECT_OPTIONAL", optionalEffect);
                  }
                  if (!running) return;
                  local.startRecording();
                  byte[] buffer = new byte[640];
                  while (running) {
                    int n = local.read(buffer, 0, buffer.length);
                    if (n < 0) throw new IOException("Audio read failed");
                    if (n == 0) continue;
                    byte[] packet = new byte[n + 1];
                    packet[0] = 3;
                    if (!mute) System.arraycopy(buffer, 0, packet, 1, n);
                    enqueue(packet);
                  }
                } catch (Exception e) {
                  if (running) error(e);
                } finally {
                  record = null;
                  if (echo != null) echo.release();
                  if (noise != null) noise.release();
                  if (local != null) {
                    try {
                      local.stop();
                    } catch (Exception ignored) {
                    }
                    local.release();
                  }
                }
              }
            },
            "audio-capture")
        .start();
    new Thread(
            new Runnable() {
              public void run() {
                AudioTrack local = null;
                try {
                  int min =
                      AudioTrack.getMinBufferSize(
                          16000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
                  if (min < 0) throw new IOException("16kHz playback unsupported");
                  local =
                      new AudioTrack(
                          AudioManager.STREAM_VOICE_CALL,
                          16000,
                          AudioFormat.CHANNEL_OUT_MONO,
                          AudioFormat.ENCODING_PCM_16BIT,
                          Math.max(min, 3200),
                          AudioTrack.MODE_STREAM);
                  playback = local;
                  if (local.getState() != AudioTrack.STATE_INITIALIZED)
                    throw new IOException("Audio playback unavailable");
                  local.play();
                  while (running) {
                    byte[] pcm = audioQueue.poll(1, TimeUnit.SECONDS);
                    if (pcm == null) continue;
                    int offset = 0;
                    while (offset < pcm.length && running) {
                      int n = local.write(pcm, offset, pcm.length - offset);
                      if (n <= 0) throw new IOException("Audio write failed");
                      offset += n;
                    }
                  }
                } catch (Exception e) {
                  if (running) error(e);
                } finally {
                  playback = null;
                  if (local != null) {
                    try {
                      local.stop();
                    } catch (Exception ignored) {
                    }
                    local.release();
                  }
                }
              }
            },
            "audio-playback")
        .start();
  }

  public boolean isMuted() {
    return mute;
  }

  public boolean isSpeakerOn() {
    return manager != null && manager.isSpeakerphoneOn();
  }

  public boolean isTorchOn() {
    return capture != null && capture.torchOn();
  }

  public boolean hasAudio() {
    return manager != null;
  }

  public boolean hasCamera() {
    return capture != null;
  }

  public void mute() {
    mute = !mute;
    listener.status(mute ? "Microphone muted" : "Microphone active");
  }

  public void speaker() {
    if (manager != null) {
      manager.setSpeakerphoneOn(!manager.isSpeakerphoneOn());
      listener.status(
          manager.isSpeakerphoneOn()
              ? "Speaker on (headphones reduce echo)"
              : "Earpiece / wired headset");
    }
  }

  public void torch() {
    if (capture != null) capture.torch();
  }

  public void switchCamera() {
    if (capture != null) capture.switchCamera();
  }

  private void error(Exception e) {
    DebugLog.error("MEDIA_ERROR", e);
    listener.status("Media stopped: " + e.getClass().getSimpleName());
    stop();
    listener.ended();
  }

  public synchronized void stop() {
    if (!running) return;
    running = false;
    Wire.close(socket);
    if (capture != null) capture.stop();
    if (display != null) display.stop();
    AudioRecord r = record;
    if (r != null)
      try {
        r.stop();
      } catch (Exception ignored) {
      }
    AudioTrack t = playback;
    if (t != null)
      try {
        t.pause();
      } catch (Exception ignored) {
      }
    if (manager != null) {
      manager.setSpeakerphoneOn(oldSpeaker);
      manager.setMode(oldMode);
    }
    DebugLog.event("MEDIA_STOP", "closed");
  }
}
