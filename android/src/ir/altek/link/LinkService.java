package ir.altek.link;

import android.app.*;
import android.content.*;
import android.content.pm.ServiceInfo;
import android.media.projection.MediaProjection;
import android.net.wifi.WifiManager;
import android.os.*;
import java.io.*;
import java.net.*;
import java.security.*;
import java.util.*;
import java.util.concurrent.*;
import javax.net.ssl.*;
import org.json.*;

/**
 * Foreground network service. No camera/mic access here; media starts only from a visible activity.
 */
public final class LinkService extends Service {
  public static final String UPDATE = "ir.altek.link.UPDATE";
  public static volatile LinkService current;
  public Store db;
  public Identity identity;
  public volatile boolean ready, cameraArmed;
  public volatile String lastCallFailure = "";

  // "screen" calls: owned here, not by an activity, so sharing survives the activity being
  // backgrounded (pressing Home, a remote tap opening another app, ...) - the whole point of
  // letting someone else drive the phone while its owner keeps using it is that leaving the app
  // must not end the session. Populated only on the side that is sharing its screen.
  public volatile ScreenEngine screenEngine;
  public ScreenClipboard screenClipboard;
  public volatile ScreenCapture screenCapture;
  public volatile RemoteControl screenRemote;
  public volatile String screenStatus = "";

  private static final class Refused extends IOException {
    final String reason;

    Refused(String message) {
      super("Peer refused request");
      reason = message;
    }
  }

  private static String safeReason(String reason) {
    if ("CONTACT_BLOCKED".equals(reason)) return "This contact is blocked on the other phone";
    if ("Busy".equals(reason)) return "The other phone is busy";
    if ("CAMERA_NOT_ARMED".equals(reason))
      return "On the camera phone: Settings > Arm camera, then keep that screen open.";
    if ("CAMERA_NOT_ALLOWED".equals(reason))
      return "On the camera phone: open your contact > menu > Contact info > Allow camera access.";
    if ("Camera not armed or contact not allowed".equals(reason))
      return "On the camera phone: allow this contact camera access, then Settings > Arm camera.";
    if ("Approve this contact on receiving phone first".equals(reason))
      return "Trust this contact on the receiving phone first.";
    return "Request refused by the other phone";
  }

  private volatile boolean live = true;
  private SSLServerSocket server;
  private DatagramSocket discovery;
  private ScheduledExecutorService clock;
  private ThreadPoolExecutor jobs;
  private IncomingRinger ringer;
  private PowerManager.WakeLock wake;
  private WifiManager.WifiLock wifi;
  public volatile Call call;

  public static final class Call {
    public String id, peer, ip, kind, token;
    public boolean outgoing, accepted, autoScreen;
    public long started;
    public volatile SSLSocket socket;
  }

  public String name() {
    return getSharedPreferences("settings", 0).getString("name", Build.MODEL);
  }

  public void change() {
    sendBroadcast(new Intent(UPDATE).setPackage(getPackageName()));
  }

  public boolean job(Runnable r) {
    if (!live || jobs == null) return false;
    try {
      jobs.execute(r);
      return true;
    } catch (RejectedExecutionException e) {
      DebugLog.event("BUSY", "Job queue full");
      return false;
    }
  }

  public IBinder onBind(Intent i) {
    return null;
  }

  public void onCreate() {
    super.onCreate();
    current = this;
    ringer = new IncomingRinger(this);
    db = new Store(this);
    db.offline();
    jobs = new ThreadPoolExecutor(8, 8, 30, TimeUnit.SECONDS, new ArrayBlockingQueue<Runnable>(64));
    clock = Executors.newSingleThreadScheduledExecutor();
    startForeground(1, notification("ALTEK Link active", "Receiving local messages", null, false));
    job(
        new Runnable() {
          public void run() {
            initialize();
          }
        });
  }

  public int onStartCommand(Intent i, int flags, int startId) {
    if (i != null && "stop".equals(i.getAction())) {
      getSharedPreferences("settings", 0).edit().putBoolean("enabled", false).apply();
      stopSelf();
      return START_NOT_STICKY;
    }
    if (i != null && "stop_screen_share".equals(i.getAction())) {
      hangup();
      return START_STICKY;
    }
    getSharedPreferences("settings", 0).edit().putBoolean("enabled", true).apply();
    return START_STICKY;
  }

  /**
   * Promote the existing foreground service to the mediaProjection type before calling
   * MediaProjectionManager.getMediaProjection(). Android Q+ requires this for apps targeting Q+.
   *
   * The service is already running in the foreground for normal network operation; calling
   * startForeground() again updates its active foreground-service type.
   */
  public void prepareMediaProjectionForeground() {
    if (Build.VERSION.SDK_INT >= 29) {
      startForeground(
          1,
          notification("ALTEK Link active", "Screen sharing active", null, false),
          ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
    }
  }

  private static void copyFile(File from, File to) throws java.io.IOException {
    java.io.InputStream in = new java.io.FileInputStream(from);
    try {
      java.io.OutputStream out = new java.io.FileOutputStream(to);
      try {
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
      } finally {
        out.close();
      }
    } finally {
      in.close();
    }
  }

  private void initialize() {
    try {
      File privateDir = new File(getFilesDir(), "identity");
      File publicDir =
          new File(android.os.Environment.getExternalStorageDirectory(), "ALTEK-Link/identity");
      /* The RSA identity must survive uninstall: files under getFilesDir() are wiped,
       * but a public-storage twin is not. Restore from the twin first, then back up
       * back to the twin after load (covers reinstalls and factory states). */
      try {
        privateDir.mkdirs();
        publicDir.mkdirs();
        File pk8 = new File(privateDir, "identity.pk8"), der = new File(privateDir, "identity.der");
        File pk8b = new File(publicDir, "identity.pk8"), derb = new File(publicDir, "identity.der");
        if ((!pk8.exists() || !der.exists()) && pk8b.exists() && derb.exists()) {
          copyFile(pk8b, pk8);
          copyFile(derb, der);
        }
        identity = new Identity(privateDir);
        if (pk8.exists() && der.exists()) {
          copyFile(pk8, pk8b);
          copyFile(der, derb);
        }
      } catch (Throwable t) {
        // External storage may be unavailable (scoped storage / no card): fall back to
        // the plain in-app path; the identity then simply won't survive uninstall.
        identity = new Identity(privateDir);
      }
      if (!live) return;
      DebugLog.init(this, identity.id);
      DebugLog.event("SERVICE", "Starting API=" + Build.VERSION.SDK_INT + " version=0.2.0");
      server = (SSLServerSocket) identity.tls.getServerSocketFactory().createServerSocket();
      server.setReuseAddress(true);
      server.bind(new InetSocketAddress(Wire.PORT));
      server.setNeedClientAuth(true);
      server.setEnabledProtocols(new String[] {"TLSv1.2"});
      discovery = new DatagramSocket(null);
      discovery.setReuseAddress(true);
      discovery.setBroadcast(true);
      discovery.bind(new InetSocketAddress(Wire.DISCOVERY));
      wake =
          ((PowerManager) getSystemService(POWER_SERVICE))
              .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ALTEK:receiver");
      wake.acquire();
      WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
      if (wm != null) {
        wifi = wm.createWifiLock(WifiManager.WIFI_MODE_FULL, "ALTEK:local");
        wifi.acquire();
      }
      if (!live) {
        Wire.close(server);
        if (discovery != null) discovery.close();
        if (wake != null && wake.isHeld()) wake.release();
        if (wifi != null && wifi.isHeld()) wifi.release();
        return;
      }
      ready = true;
      change();
      new Thread(
              new Runnable() {
                public void run() {
                  acceptLoop();
                }
              },
              "tls-listener")
          .start();
      new Thread(
              new Runnable() {
                public void run() {
                  discoverLoop();
                }
              },
              "discovery-listener")
          .start();
      clock.scheduleWithFixedDelay(
          new Runnable() {
            public void run() {
              try {
                announce();
                flush();
                expireCall();
                change();
              } catch (Exception e) {
                DebugLog.error("TICK", e);
              }
            }
          },
          0,
          4,
          TimeUnit.SECONDS);
    } catch (Exception e) {
      DebugLog.init(this, "00000000");
      DebugLog.error("SERVICE_START", e);
      notifyUser("Service failed", e.getClass().getSimpleName(), null);
      stopSelf();
    }
  }

  private void acceptLoop() {
    while (live)
      try {
        final SSLSocket socket = (SSLSocket) server.accept();
        socket.setSoTimeout(12000);
        socket.setTcpNoDelay(true);
        try {
          jobs.execute(
              new Runnable() {
                public void run() {
                  handle(socket);
                }
              });
        } catch (RejectedExecutionException e) {
          Wire.close(socket);
        }
      } catch (Exception e) {
        if (live) DebugLog.error("ACCEPT", e);
      }
  }

  private JSONObject hello(String type) throws Exception {
    return new JSONObject().put("v", 1).put("type", type).put("name", name());
  }

  public JSONObject request(Store.Peer peer, JSONObject req) throws Exception {
    if (peer == null || !peer.approved) throw new IOException("Contact missing or blocked");
    SSLSocket s = identity.connect(peer.ip);
    try {
      if (!Identity.peer(s).equals(peer.id)) throw new SSLException("Contact key changed");
      req.put("v", 1).put("name", name());
      Wire.text(new DataOutputStream(s.getOutputStream()), req.toString());
      JSONObject response = new JSONObject(Wire.readText(new DataInputStream(s.getInputStream())));
      if (!response.optBoolean("ok"))
        throw new Refused(response.optString("error", "Request refused"));
      return response;
    } finally {
      Wire.close(s);
    }
  }

  private void reply(DataOutputStream out, boolean ok, String message) throws Exception {
    Wire.text(out, new JSONObject().put("ok", ok).put("error", message).toString());
  }

  private void handle(SSLSocket s) {
    boolean handoff = false;
    try {
      s.startHandshake();
      String peerId = Identity.peer(s);
      String ip = s.getInetAddress().getHostAddress();
      DataInputStream in = new DataInputStream(s.getInputStream());
      DataOutputStream out = new DataOutputStream(s.getOutputStream());
      JSONObject j = new JSONObject(Wire.readText(in));
      if (j.optInt("v") != 1) throw new IOException("Protocol version");
      String type = j.optString("type"), display = j.optString("name", "Device");
      db.seen(peerId, display, ip, true);
      Store.Peer p = db.peer(peerId);
      change();
      if ("hello".equals(type)) {
        Wire.text(
            out,
            new JSONObject()
                .put("ok", true)
                .put("name", name())
                .put("id", identity.id)
                .put("avatar", Profile.hash(this, "self"))
                .put("file_stream", true)
                .toString());
        return;
      }
      if (p == null || !p.approved) {
        reply(out, false, "CONTACT_BLOCKED");
        return;
      }
      if ("profile".equals(type)) {
        Wire.text(out, Profile.response(this, j.optString("known")).toString());
        return;
      }
      if ("text".equals(type)) {
        String id = validId(j), body = j.getString("text");
        if (body.length() > 8000) throw new IOException("Text too long");
        boolean fresh = db.add(id, peerId, "text", body, "", false, "delivered");
        reply(out, true, "");
        if (fresh) notifyUser(display, "New message", peerId);
        DebugLog.event("TEXT_RX", "peer=" + shortId(peerId) + " new=" + fresh);
        change();
      } else if ("offer".equals(type) || "offer_stream".equals(type)) {
        String id = validId(j);
        long size = j.getLong("size");
        if (size < 0) throw new IOException("Invalid file size");
        String filename = Wire.safeName(j.getString("filename"));
        String hash = "offer_stream".equals(type) ? "" : j.getString("hash");
        if (!"offer_stream".equals(type) && !hash.matches("[0-9a-f]{64}")) throw new IOException("Bad digest");
        String kind = "voice".equals(j.optString("kind")) ? "voice" : "file";
        JSONObject metadata =
            new JSONObject()
                .put("filename", filename)
                .put("size", size)
                .put("hash", hash)
                .put("mime", j.optString("mime", "application/octet-stream"));
        boolean fresh = db.add(id, peerId, kind, metadata.toString(), "", false, "offered");
        reply(out, true, "");
        if (fresh) notifyUser(display, "File ready - tap to accept", peerId);
        change();
        DebugLog.event("OFFER_RX", "bytes=" + size);
      } else if ("download".equals(type)) {
        Store.Msg m = db.message(validId(j));
        if (m == null || !m.mine || !m.peer.equals(peerId) || "text".equals(m.kind))
          throw new IOException("File not authorized");
        JSONObject meta = new JSONObject(m.body);
        long total = meta.getLong("size"), offset = j.optLong("offset", 0);
        if (offset < 0 || offset > total) throw new IOException("Invalid offset");
        boolean streaming = meta.optString("hash").length() == 0;
        if (streaming && !j.optBoolean("sha256_trailer")) throw new IOException("Streaming digest required");
        MessageDigest fileHash = streaming ? MessageDigest.getInstance("SHA-256") : null;
        InputStream file;
        if (m.path.startsWith("content://")) file = getContentResolver().openInputStream(android.net.Uri.parse(m.path));
        else {
          File f = new File(m.path);
          if (!f.exists() || f.length() != total) throw new IOException("Source missing or changed");
          file = new FileInputStream(f);
        }
        if (file == null) throw new IOException("Source missing");
        try {
          Wire.text(out, new JSONObject().put("ok", true).put("offset", offset).toString());
          byte[] b = new byte[262144];
          long skip = offset;
          while (skip > 0) {
            if (streaming) {
              int n = file.read(b, 0, (int)Math.min(b.length, skip));
              if (n < 0) throw new EOFException();
              fileHash.update(b, 0, n); skip -= n;
            } else {
              long n = file.skip(skip);
              if (n <= 0) { if (file.read() < 0) throw new EOFException(); n = 1; }
              skip -= n;
            }
          }
          long remaining = total - offset;
          while (remaining > 0 && live) {
            int n = file.read(b, 0, (int)Math.min(b.length, remaining));
            if (n < 0) throw new EOFException();
            out.write(b, 0, n);
            if (streaming) fileHash.update(b, 0, n);
            remaining -= n;
          }
          if (remaining != 0) throw new EOFException();
          out.flush();
          if (streaming) Wire.text(out, new JSONObject().put("hash", Wire.hex(fileHash.digest())).toString());
        } finally { file.close(); }
        s.setSoTimeout(4 * 60 * 60 * 1000);
        JSONObject done = new JSONObject(Wire.readText(in));
        if (done.optBoolean("ok")) db.state(m.id, "transferred");
        change();
        DebugLog.event("FILE_TX", "bytes=" + total);
      } else if ("call".equals(type)) {
        Call c = new Call();
        c.id = validId(j);
        c.peer = peerId;
        c.ip = ip;
        c.kind = j.optString("kind");
        c.token = j.getString("token");
        c.started = System.currentTimeMillis();
        if (!Arrays.asList("audio", "video", "view", "screen").contains(c.kind)
            || !c.token.matches("[0-9a-f]{64}")) throw new IOException("Bad call");
        // Re-check the actual root grant on every automatic request: it may have been revoked
        // since permission was enabled. A failed check must leave the normal call flow intact.
        boolean remoteRootReady = "screen".equals(c.kind) && p.remote
            && RootShell.probeAvailable().granted;
        if ("screen".equals(c.kind) && p.remote && !remoteRootReady) {
          db.remote(peerId, false);
          change();
        }
        synchronized (this) {
          if (call != null) {
            reply(out, false, "Busy");
            return;
          }
          if ("view".equals(c.kind) && (!cameraArmed || !p.camera)) {
            String reason = !p.camera ? "CAMERA_NOT_ALLOWED" : "CAMERA_NOT_ARMED";
            DebugLog.event("CALL_REFUSED", reason + " peer=" + shortId(peerId));
            reply(out, false, reason);
            return;
          }
          lastCallFailure = "";
          call = c;
          Store.Peer latestPeer = db.peer(peerId);
          c.autoScreen = remoteRootReady && latestPeer != null && latestPeer.approved && latestPeer.remote;
          c.accepted = "view".equals(c.kind) || c.autoScreen;
        }
        reply(out, true, "");
        DebugLog.event("CALL_RX", "kind=" + c.kind + " peer=" + shortId(peerId));
        change();
        if (!c.accepted) {
          synchronized (this) {
            if (live && call == c && !c.accepted) {
              ((NotificationManager) getSystemService(NOTIFICATION_SERVICE))
                  .notify(
                      3,
                      notification(
                          display,
                          "Incoming " + c.kind + " call - tap to answer",
                          peerId,
                          true,
                          true));
              ringer.start();
            }
          }
          if (live && call == c && !c.accepted) {
            try {
              Intent inIntent = new Intent(this, IncomingCallActivity.class);
              inIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
              startActivity(inIntent);
            } catch (Exception e) {
              DebugLog.error("INCOMING_UI", e);
            }
          }
        } else sendAccept(c);
      } else if ("accept".equals(type)) {
        final Call c = call;
        if (c == null
            || !c.outgoing
            || !c.peer.equals(peerId)
            || !c.id.equals(j.optString("id"))
            || !c.token.equals(j.optString("token"))) throw new IOException("No matching call");
        c.accepted = true;
        reply(out, true, "");
        job(
            new Runnable() {
              public void run() {
                connectMedia(c);
              }
            });
        change();
      } else if ("hangup".equals(type)) {
        Call c = call;
        if (c != null && c.peer.equals(peerId) && c.id.equals(j.optString("id"))) {
          clearCall(c);
          change();
        }
        reply(out, true, "");
      } else if ("media".equals(type)) {
        Call c = call;
        if (c == null
            || c.outgoing
            || !c.accepted
            || !c.peer.equals(peerId)
            || !c.id.equals(j.optString("id"))
            || !c.token.equals(j.optString("token"))) throw new IOException("Media not authorized");
        synchronized (c) {
          if (c.socket != null) throw new IOException("Media already connected");
          reply(out, true, "");
          s.setSoTimeout(15000);
          c.socket = s;
          handoff = true;
        }
        change();
        if (c.autoScreen) beginScreenShare(c, null);
      } else reply(out, false, "Unknown operation");
    } catch (Exception e) {
      if (live) DebugLog.error("REQUEST", e);
    } finally {
      if (!handoff) Wire.close(s);
    }
  }

  static String shortId(String s) {
    return s.substring(0, 8);
  }

  private String validId(JSONObject j) throws Exception {
    String id = j.getString("id");
    if (!id.matches("[0-9a-fA-F-]{36}")) throw new IOException("Invalid id");
    return id;
  }

  public void probe(final String ip) {
    job(
        new Runnable() {
          public void run() {
            SSLSocket s = null;
            try {
              if (!ip.matches("[0-9.]{7,15}")) throw new IOException("IPv4 only");
              s = identity.connect(ip);
              String id = Identity.peer(s);
              Wire.text(new DataOutputStream(s.getOutputStream()), hello("hello").toString());
              JSONObject j = new JSONObject(Wire.readText(new DataInputStream(s.getInputStream())));
              if (!identity.id.equals(id)) {
                db.seen(id, j.optString("name", "Device"), ip, true);
                if (j.has("avatar")) syncProfile(id, j.optString("avatar"));
              }
              change();
              DebugLog.event("PROBE", "success peer=" + shortId(id));
            } catch (Exception e) {
              DebugLog.error("PROBE", e);
              notifyUser("Connection failed", "Check IP and network", null);
            } finally {
              Wire.close(s);
            }
          }
        });
  }

  private void discoverLoop() {
    byte[] buffer = new byte[2048];
    while (live)
      try {
        DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
        discovery.receive(packet);
        JSONObject j = new JSONObject(new String(packet.getData(), 0, packet.getLength(), "UTF-8"));
        if (!"ALTEK_LINK".equals(j.optString("app")) || j.optInt("v") != 1) continue;
        String id = j.optString("id");
        if (id.equals(identity.id) || !id.matches("[0-9a-f]{64}")) continue;
        if (db.peer(id) == null && db.peers().size() >= 200) continue;
        db.seen(id, j.optString("name", "Device"), packet.getAddress().getHostAddress(), true);
        if (j.has("avatar")) syncProfile(id, j.optString("avatar"));
        change();
      } catch (Exception e) {
        if (live) DebugLog.error("DISCOVERY", e);
      }
  }

  private void announce() throws Exception {
    byte[] b =
        new JSONObject()
            .put("avatar", Profile.hash(this, "self"))
            .put("app", "ALTEK_LINK")
            .put("v", 1)
            .put("id", identity.id)
            .put("name", name())
            .toString()
            .getBytes("UTF-8");
    Set<String> destinations = new HashSet<String>();
    destinations.add("255.255.255.255");
    Enumeration<NetworkInterface> all = NetworkInterface.getNetworkInterfaces();
    while (all.hasMoreElements())
      for (InterfaceAddress a : all.nextElement().getInterfaceAddresses())
        if (a.getBroadcast() != null) destinations.add(a.getBroadcast().getHostAddress());
    WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
    if (wm != null && wm.getDhcpInfo() != null) {
      int g = wm.getDhcpInfo().gateway;
      if (g != 0)
        destinations.add(
            (g & 255)
                + "."
                + ((g >>> 8) & 255)
                + "."
                + ((g >>> 16) & 255)
                + "."
                + ((g >>> 24) & 255));
    }
    for (Store.Peer p : db.peers()) if (p.approved) destinations.add(p.ip);
    for (String ip : destinations)
      try {
        discovery.send(new DatagramPacket(b, b.length, InetAddress.getByName(ip), Wire.DISCOVERY));
      } catch (Exception ignored) {
      }
  }

  private final Set<String> profileBusy = Collections.synchronizedSet(new HashSet<String>());
  private final java.util.concurrent.ConcurrentHashMap<String, Long> profileAttempt =
      new java.util.concurrent.ConcurrentHashMap<String, Long>();

  public void profileChanged() {
    job(
        new Runnable() {
          public void run() {
            try {
              announce();
            } catch (Exception e) {
              DebugLog.error("PROFILE_ANNOUNCE", e);
            }
          }
        });
  }

  private void syncProfile(final String peer, String advertised) {
    Store.Peer p = db.peer(peer);
    if (p == null
        || !p.approved
        || !(advertised.length() == 0 || advertised.matches("[0-9a-f]{64}"))) return;
    String stored = Profile.hash(this, peer);
    if (advertised.equals(stored) && Profile.has(this, stored)) return;
    Long previous = profileAttempt.get(peer);
    long now = android.os.SystemClock.elapsedRealtime();
    if (previous != null && now - previous < 15000) return;
    if (!profileBusy.add(peer)) return;
    profileAttempt.put(peer, now);
    boolean queued =
        job(
            new Runnable() {
              public void run() {
                try {
                  String known = Profile.hash(LinkService.this, peer);
                  if (!Profile.has(LinkService.this, known)) known = "";
                  JSONObject response =
                      request(db.peer(peer), hello("profile").put("known", known));
                  Store.Peer currentPeer = db.peer(peer);
                  if (currentPeer != null && currentPeer.approved) {
                    Profile.accept(LinkService.this, peer, response);
                    DebugLog.event(
                        "PROFILE_RX", "peer=" + shortId(peer) + " image=" + response.has("image"));
                    change();
                  }
                } catch (Exception e) {
                  DebugLog.error("PROFILE", e);
                } finally {
                  profileBusy.remove(peer);
                }
              }
            });
    if (!queued) profileBusy.remove(peer);
  }

  public void text(String peer, String text) {
    db.add(Wire.uid(), peer, "text", text, "", true, "queued");
    change();
    job(
        new Runnable() {
          public void run() {
            flush();
          }
        });
  }

  private final Set<String> sending = Collections.synchronizedSet(new HashSet<String>());
  private final Set<String> sendingPeers = Collections.synchronizedSet(new HashSet<String>());

  private void flush() {
    if (!ready) return;
    for (final Store.Msg m : db.pending()) {
      final Store.Peer p = db.peer(m.peer);
      if (p == null || !p.approved || !p.online() || !sendingPeers.add(p.id)) continue;
      boolean accepted =
          job(
              new Runnable() {
                public void run() {
                  boolean completed = false;
                  try {
                    JSONObject j = hello("text").put("id", m.id);
                    if ("text".equals(m.kind)) j.put("text", m.body);
                    else {
                      JSONObject meta = new JSONObject(m.body);
                      if (meta.optString("hash").length() == 0) {
                        JSONObject capabilities = request(p, hello("hello"));
                        if (!capabilities.optBoolean("file_stream")) {
                          MessageDigest legacy = MessageDigest.getInstance("SHA-256");
                          InputStream source = m.path.startsWith("content://")
                              ? getContentResolver().openInputStream(android.net.Uri.parse(m.path))
                              : new FileInputStream(m.path);
                          if (source == null) throw new IOException("Source missing");
                          long count = 0;
                          try {
                            byte[] buffer = new byte[262144]; int n;
                            while ((n = source.read(buffer)) != -1) { legacy.update(buffer, 0, n); count += n; }
                          } finally { source.close(); }
                          if (count != meta.getLong("size")) throw new IOException("Source changed");
                          meta.put("hash", Wire.hex(legacy.digest()));
                          if (!db.body(m.id, meta.toString())) throw new IOException("Message removed");
                        }
                      }
                      j =
                          hello(meta.optString("hash").length() == 0 ? "offer_stream" : "offer")
                              .put("id", m.id)
                              .put("kind", m.kind)
                              .put("filename", meta.getString("filename"))
                              .put("size", meta.getLong("size"))
                              .put("hash", meta.getString("hash"))
                              .put("mime", meta.getString("mime"));
                    }
                    request(p, j);
                    db.state(m.id, "text".equals(m.kind) ? "delivered" : "offered");
                    DebugLog.event("DELIVERY", "peer=" + shortId(p.id) + " kind=" + m.kind);
                    change();
                    completed = true;
                  } catch (Exception e) {
                    DebugLog.error("SEND_RETRY", e);
                  } finally {
                    sendingPeers.remove(p.id);
                    if (completed) flush();
                  }
                }
              });
      if (!accepted) sendingPeers.remove(p.id);
    }
  }

  public void offer(final String peer, final android.net.Uri uri, final String kind) {
    if (uri == null) return;
    job(
        new Runnable() {
          public void run() {
            File dest = null;
            try {
              String filename = "attachment", mime = getContentResolver().getType(uri);
              long knownSize = -1;
              android.database.Cursor c = getContentResolver().query(uri, null, null, null, null);
              if (c != null)
                try {
                  int col = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                  if (c.moveToFirst()) {
                    if (col >= 0) filename = c.getString(col);
                    int sizeCol = c.getColumnIndex(android.provider.OpenableColumns.SIZE);
                    if (sizeCol >= 0 && !c.isNull(sizeCol)) knownSize = c.getLong(sizeCol);
                  }
                } finally {
                  c.close();
                }
              if ("file".equals(uri.getScheme())) { filename = new File(uri.getPath()).getName(); knownSize = new File(uri.getPath()).length(); }
              filename = Wire.safeName(filename == null ? "attachment" : filename);
              String id = Wire.uid();
              boolean direct = "file".equals(kind) && "file".equals(uri.getScheme())
                  && uri.getPath() != null && new File(uri.getPath()).isFile();
              if (android.os.Build.VERSION.SDK_INT >= 19 && "content".equals(uri.getScheme())) {
                direct = PersistedAccess.hasRead(LinkService.this, uri);
              }
              if (knownSize < 0) direct = false;
              if (!direct) {
                File folder = new File(getFilesDir(), "sent");
                folder.mkdirs();
                dest = new File(folder, id);
              }
              long size = knownSize;
              if (!direct) {
                InputStream in = getContentResolver().openInputStream(uri);
                if (in == null) throw new IOException("Cannot open attachment");
                FileOutputStream out = new FileOutputStream(dest);
                size = 0;
                try {
                  byte[] b = new byte[262144]; int n;
                  while ((n = in.read(b)) != -1) { out.write(b, 0, n); size += n; }
                } finally { Wire.close(in); Wire.close(out); }
              }
              JSONObject meta =
                  new JSONObject()
                      .put("filename", filename)
                      .put("size", size)
                      .put("hash", "")
                      .put(
                          "mime",
                          Mime.isGeneric(mime)
                              ? ("voice".equals(kind) ? "audio/3gpp" : Mime.resolve(null, filename, dest))
                              : mime);
              db.add(id, peer, kind, meta.toString(), direct ? ("file".equals(uri.getScheme()) ? uri.getPath() : uri.toString()) : dest.getAbsolutePath(), true, "queued");
              change();
              DebugLog.event("FILE_STAGED", "bytes=" + size);
              flush();
            } catch (Exception e) {
              if (dest != null) dest.delete();
              DebugLog.error("FILE_STAGE", e);
              notifyUser("Attachment failed", "Check available storage and file access", peer);
            }
          }
        });
  }

  public void download(final Store.Msg m) {
    if (!sending.add(m.id)) return;
    job(
        new Runnable() {
          public void run() {
            SSLSocket s = null;
            try {
              Store.Peer p = db.peer(m.peer);
              if (p == null || !p.approved) throw new IOException("Untrusted");
              JSONObject meta = new JSONObject(m.body);
              long size = meta.getLong("size");
              ReceiveTarget target = new ReceiveTarget(LinkService.this, m.id, meta.getString("filename"));
              long offset = target.size();
              if (offset > size) offset = 0;
              target.checkSpace(size - offset);
              MessageDigest hash = MessageDigest.getInstance("SHA-256");
              target.digestPrefix(hash, offset);
              s = identity.connect(p.ip);
              if (!Identity.peer(s).equals(p.id)) throw new SSLException("Key changed");
              DataInputStream in = new DataInputStream(s.getInputStream());
              DataOutputStream out = new DataOutputStream(s.getOutputStream());
              boolean streaming = meta.optString("hash").length() == 0;
              s.setSoTimeout(4 * 60 * 60 * 1000);
              Wire.text(out, hello("download").put("id", m.id).put("offset", offset).put("sha256_trailer", streaming).toString());
              JSONObject reply = new JSONObject(Wire.readText(in));
              if (!reply.optBoolean("ok")) throw new IOException("Download refused");
              long accepted = reply.optLong("offset", 0);
              if (accepted != offset) {
                if (accepted != 0) throw new IOException("Invalid server offset");
                offset = 0;
                hash.reset();
                target.checkSpace(size);
              }
              OutputStream file = target.output(offset);
              long remaining = size - offset, last = 0;
              try {
                byte[] b = new byte[262144];
                while (remaining > 0) {
                  int n = in.read(b, 0, (int) Math.min(b.length, remaining));
                  if (n < 0) throw new EOFException();
                  file.write(b, 0, n);
                  hash.update(b, 0, n);
                  remaining -= n;
                  long now = System.currentTimeMillis();
                  if (now - last > 1000) {
                    db.state(m.id, "receiving " + (long) ((double)(size - remaining) * 100 / Math.max(1, size)) + "%");
                    change(); last = now;
                  }
                }
              } finally { file.close(); }
              String expectedHash = streaming ? new JSONObject(Wire.readText(in)).getString("hash") : meta.getString("hash");
              if (!expectedHash.matches("[0-9a-f]{64}") || !Wire.hex(hash.digest()).equals(expectedHash)) {
                target.discard();
                throw new IOException("Checksum mismatch; retry from beginning");
              }
              String finishedPath = target.finish();
              if (!db.path(m.id, finishedPath, "downloaded"))
                throw new IOException("Conversation removed during download");
              reply(out, true, "");
              change();
              DebugLog.event("FILE_RX", "bytes=" + size);
            } catch (Exception e) {
              db.state(m.id, "retry download");
              change();
              DebugLog.error("FILE_RX", e);
              notifyUser("Download failed", "Check destination access and available space, then retry", m.peer);
            } finally {
              Wire.close(s);
              sending.remove(m.id);
            }
          }
        });
  }

  public synchronized Call dial(String peer, String kind) throws Exception {
    if (call != null) throw new IOException("Another call is active");
    Store.Peer p = db.peer(peer);
    if (p == null || !p.approved || !p.online())
      throw new IOException("Contact must be unblocked and online");
    lastCallFailure = "";
    final Call c = new Call();
    c.id = Wire.uid();
    c.peer = peer;
    c.ip = p.ip;
    c.kind = kind;
    c.outgoing = true;
    c.started = System.currentTimeMillis();
    byte[] token = new byte[32];
    new SecureRandom().nextBytes(token);
    c.token = Wire.hex(token);
    call = c;
    job(
        new Runnable() {
          public void run() {
            try {
              request(
                  db.peer(c.peer),
                  hello("call").put("id", c.id).put("kind", c.kind).put("token", c.token));
            } catch (Exception e) {
              if (call != c) return;
              lastCallFailure =
                  e instanceof Refused
                      ? safeReason(((Refused) e).reason)
                      : "Connection failed. Check the other phone and network.";
              if (e instanceof Refused) DebugLog.event("CALL_REFUSED", lastCallFailure);
              else DebugLog.error("CALL", e);
              clearCall(c);
              notifyUser("Call unavailable", lastCallFailure, c.peer);
              change();
            }
          }
        });
    return c;
  }

  public synchronized void answer(final Call c) {
    if (c != call || c.outgoing) return;
    c.accepted = true;
    stopRinging();
    sendAccept(c);
  }

  private void sendAccept(final Call c) {
    job(
        new Runnable() {
          public void run() {
            try {
              request(db.peer(c.peer), hello("accept").put("id", c.id).put("token", c.token));
            } catch (Exception e) {
              DebugLog.error("ACCEPT_CALL", e);
              clearCall(c);
              change();
            }
          }
        });
  }

  private void connectMedia(Call c) {
    SSLSocket s = null;
    try {
      if (c != call) return;
      s = identity.connect(c.ip);
      if (!Identity.peer(s).equals(c.peer)) throw new SSLException("Key changed");
      Wire.text(
          new DataOutputStream(s.getOutputStream()),
          hello("media").put("id", c.id).put("token", c.token).toString());
      JSONObject j = new JSONObject(Wire.readText(new DataInputStream(s.getInputStream())));
      if (!j.optBoolean("ok")) throw new IOException("Media rejected");
      if (c != call) throw new IOException("Call cancelled");
      s.setSoTimeout(15000);
      c.socket = s;
      s = null;
      change();
      DebugLog.event("MEDIA", "TLS connected");
    } catch (Exception e) {
      DebugLog.error("MEDIA_CONNECT", e);
      clearCall(c);
      change();
    } finally {
      Wire.close(s);
    }
  }

  public void hangup() {
    final Call c = call;
    if (c == null) return;
    clearCall(c);
    change();
    job(
        new Runnable() {
          public void run() {
            try {
              request(db.peer(c.peer), hello("hangup").put("id", c.id));
            } catch (Exception ignored) {
            }
          }
        });
  }

  private synchronized void clearCall(Call c) {
    if (call != c) return;
    call = null;
    stopRinging();
    Wire.close(c.socket);
    endScreenShare();
  }

  /** Starts sharing this device's screen for an already-answered "screen" call. Call once, from
   *  whichever activity answered it; the session then lives here, independent of that activity.
   *  projection is the already-approved MediaProjection (API 21+, no root), or null to use the
   *  root-based fallback (screenrecord on API 19-20, periodic screenshots on API 16-18). */
  public synchronized void beginScreenShare(final Call c, MediaProjection projection) {
    if (call != c || c.socket == null || screenEngine != null) return;
    int[] real = ScreenCapture.realSize(this);
    final RemoteControl r = new RemoteControl(real[0], real[1]);
    screenRemote = r;
    screenEngine =
        new ScreenEngine(
            c.socket,
            null,
            new ScreenEngine.ControlListener() {
              public void tap(float x, float y) {
                r.tap(x, y);
              }

              public void swipe(float x1, float y1, float x2, float y2, int ms) {
                r.swipe(x1, y1, x2, y2, ms);
              }

              public void key(int action) {
                r.key(action);
              }

              public void pinch(float x, float y, float scale) {
                DebugLog.event("IN_PINCH", "x=" + x + " y=" + y + " scale=" + scale);
                r.pinch(x, y, scale);
              }

              public void mouseDown(float x, float y) {
                r.mouseDown(x, y);
              }

              public void mouseMove(float x, float y) {
                r.mouseMove(x, y);
              }

              public void mouseUp(float x, float y) {
                r.mouseUp(x, y);
              }

              public void mouseRightDown(float x, float y) {
                r.rightDown(x, y);
              }

              public void mouseRightUp(float x, float y) {
                r.rightUp(x, y);
              }

              public void keyEvent(int vk, int down) {
                r.keyEvent(vk, down);
              }

              public void textInput(String text) {
                r.textInput(text);
              }
            },
            new ScreenEngine.Listener() {
              public void status(String text) {
                screenStatus = text;
                change();
              }

              public void ended() {
                hangup();
              }
            });
    final ScreenEngine e = screenEngine;
    screenClipboard = new ScreenClipboard(this, e, false);
    e.attachClipboard(screenClipboard);
    if (Build.VERSION.SDK_INT >= 21 && ControlAccessibilityService.enabled()) e.sendTextCapability();
    ScreenCapture.Sink sink =
        new ScreenCapture.Sink() {
          public void format(boolean h264, int w, int h, byte[] config) {
            e.sendFormat(h264, w, h, config);
          }

          public void frame(boolean key, long pts, byte[] bytes) {
            if (e.sendFrame(key, pts, bytes)) {
              ScreenCapture capture = screenCapture;
              if (capture != null) capture.requestSyncFrame();
            }
          }

          public void status(String text) {
            screenStatus = text;
            change();
          }

          public void error(Exception ex) {
            DebugLog.error("SCREEN_CAPTURE", ex);
            screenStatus = "Screen capture failed: " + ex.getMessage();
            change();
            hangup();
          }
        };
    screenCapture = projection != null ? new ScreenCapture(this, projection, sink) : new ScreenCapture(this, sink);
    Intent stop = new Intent(this, LinkService.class).setAction("stop_screen_share");
    PendingIntent stopIntent =
        PendingIntent.getService(
            this,
            0,
            stop,
            Build.VERSION.SDK_INT >= 23
                ? PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
                : PendingIntent.FLAG_UPDATE_CURRENT);
    Notification n =
        notification(
            "Screen sharing active",
            "Tap to return - sharing keeps going if you leave this app",
            c.peer,
            false,
            false);
    if (Build.VERSION.SDK_INT >= 20) {
      // Notification.Action needs API 20; below that the notification still shows, just without
      // the one-tap Stop button - returning to the app and using its own Stop button still works.
      n.actions =
          new Notification.Action[] {
            new Notification.Action.Builder(
                    android.R.drawable.ic_menu_close_clear_cancel, "Stop", stopIntent)
                .build()
          };
    }
    ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).notify(4, n);
  }

  private void endScreenShare() {
    if (screenClipboard != null) { screenClipboard.close(); screenClipboard = null; }
    if (screenEngine != null) {
      screenEngine.stop();
      screenEngine = null;
    }
    if (screenCapture != null) {
      screenCapture.stop();
      screenCapture = null;
    }
    if (screenRemote != null) {
      screenRemote.stop();
      screenRemote = null;
    }
    screenStatus = "";
    ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).cancel(4);
  }

  private void expireCall() {
    Call c = call;
    if (c != null && c.socket == null && System.currentTimeMillis() - c.started > 45000) {
      DebugLog.event("CALL", "timeout");
      hangup();
    }
  }

  private void stopRinging() {
    if (ringer != null) ringer.stop();
    ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).cancel(3);
  }

  public Notification notification(String title, String text, String peer, boolean alert) {
    return notification(title, text, peer, alert, false);
  }

  private Notification notification(
      String title, String text, String peer, boolean alert, boolean incoming) {
    NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
    String channel = incoming ? "incoming_calls" : alert ? "events" : "receiver";
    if (Build.VERSION.SDK_INT >= 26)
      try {
        Class<?> cls = Class.forName("android.app.NotificationChannel");
        Object value =
            cls.getConstructor(String.class, CharSequence.class, int.class)
                .newInstance(
                    channel, alert ? "Messages and calls" : "Local receiver", alert ? 4 : 2);
        if (incoming) {
          cls.getMethod(
                  "setSound", android.net.Uri.class, Class.forName("android.media.AudioAttributes"))
              .invoke(value, new Object[] {null, null});
          cls.getMethod("enableVibration", boolean.class).invoke(value, false);
        }
        NotificationManager.class
            .getMethod("createNotificationChannel", cls)
            .invoke(manager, value);
      } catch (Exception e) {
        DebugLog.error("CHANNEL", e);
      }
    Intent intent =
        new Intent(this, incoming ? IncomingCallActivity.class : MainActivity.class);
    if (peer != null) intent.putExtra("peer", peer);
    intent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
    int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 23 ? 0x04000000 : 0);
    PendingIntent pending =
        PendingIntent.getActivity(this, peer == null ? 0 : peer.hashCode(), intent, flags);
    Notification.Builder b =
        new Notification.Builder(this)
            .setSmallIcon(android.R.drawable.stat_notify_chat)
            .setContentTitle(title)
            .setContentText(text)
            .setContentIntent(pending)
            .setOngoing(!alert || incoming)
            .setAutoCancel(alert && !incoming);
    if (!alert) {
      Intent stop = new Intent(this, LinkService.class).setAction("stop");
      PendingIntent stopIntent = PendingIntent.getService(this, 99, stop, flags);
      b.addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop receiver", stopIntent);
      b.setPriority(Notification.PRIORITY_LOW);
    }
    if (alert && !incoming)
      b.setDefaults(Notification.DEFAULT_SOUND | Notification.DEFAULT_VIBRATE);
    if (Build.VERSION.SDK_INT >= 26)
      try {
        Notification.Builder.class.getMethod("setChannelId", String.class).invoke(b, channel);
      } catch (Exception ignored) {
      }
    return b.build();
  }

  public void notifyUser(String title, String text, String peer) {
    ((NotificationManager) getSystemService(NOTIFICATION_SERVICE))
        .notify(2, notification(title, text, peer, true));
  }

  public void onDestroy() {
    live = false;
    stopRinging();
    stopForeground(true);
    ready = false;
    cameraArmed = false;
    Call c = call;
    if (c != null) Wire.close(c.socket);
    Wire.close(server);
    if (discovery != null) discovery.close();
    if (clock != null) clock.shutdownNow();
    if (jobs != null) jobs.shutdownNow();
    if (wake != null && wake.isHeld()) wake.release();
    if (wifi != null && wifi.isHeld()) wifi.release();
    if (db != null) db.offline();
    current = null;
    DebugLog.event("SERVICE", "Stopped");
    super.onDestroy();
  }
}
