package ir.altek.link;
import android.content.Context;
import android.net.Uri;
import java.io.*;
import java.net.*;
import java.security.SecureRandom;
import java.text.SimpleDateFormat;
import java.util.*;

/** Local browser bridge. Each session has an unguessable URL and explicit lifetime. */
final class WebBridge extends NanoHTTPD {
  private final Context context;
  private final Uri tree;
  private final File root;
  private final String token;
  private final String accessCode;
  private int failedAttempts;
  private long blockedUntil;
  private final Map<String, String> folders = new HashMap<String, String>();
  private volatile String sharedText = "";
  private static final String[] DISPLAY_KEYS = {"web_images", "web_video", "web_html"};

  boolean showInBrowser(int group) {
    return context.getSharedPreferences("settings", 0).getBoolean(DISPLAY_KEYS[group], group != 2);
  }

  void setShowInBrowser(int group, boolean enabled) {
    context.getSharedPreferences("settings", 0)
        .edit()
        .putBoolean(DISPLAY_KEYS[group], enabled)
        .apply();
  }

  private static int fileGroup(String name) {
    String lower = name.toLowerCase(Locale.US);
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png"))
      return 0;
    if (lower.endsWith(".mp4"))
      return 1;
    if (lower.endsWith(".html") || lower.endsWith(".htm"))
      return 2;
    return -1;
  }

  private static String contentType(String name) {
    String lower = name.toLowerCase(Locale.US);
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg"))
      return "image/jpeg";
    if (lower.endsWith(".png"))
      return "image/png";
    if (lower.endsWith(".mp4"))
      return "video/mp4";
    if (lower.endsWith(".html") || lower.endsWith(".htm"))
      return "text/html; charset=utf-8";
    return "application/octet-stream";
  }

  WebBridge(Context c, Uri t, File f) throws Exception {
    super(0);
    context = c.getApplicationContext();
    tree = t;
    root = f;
    SecureRandom random = new SecureRandom();
    byte[] b = new byte[16];
    random.nextBytes(b);
    token = Wire.hex(b);
    String alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    StringBuilder code = new StringBuilder();
    for (int i = 0; i < 4; i++)
      code.append(alphabet.charAt(random.nextInt(alphabet.length())));
    accessCode = code.toString();
    folders.put("root", tree == null ? root.getCanonicalPath() : WebDocuments.root(tree));
    setAsyncRunner(new LimitedClients());
  }
  private static final class LimitedClients implements NanoHTTPD.AsyncRunner {
    private final Set<NanoHTTPD.ClientHandler> clients = new HashSet<NanoHTTPD.ClientHandler>();
    public synchronized void exec(NanoHTTPD.ClientHandler client) {
      if (clients.size() >= 8) {
        client.close();
        return;
      }
      clients.add(client);
      Thread t = new Thread(client, "WebBridge client");
      t.setDaemon(true);
      t.start();
    }
    public synchronized void closed(NanoHTTPD.ClientHandler client) {
      clients.remove(client);
    }
    public synchronized void closeAll() {
      for (NanoHTTPD.ClientHandler client : new ArrayList<NanoHTTPD.ClientHandler>(clients))
        client.close();
      clients.clear();
    }
  }
  String address() throws Exception {
    Enumeration<NetworkInterface> all = NetworkInterface.getNetworkInterfaces();
    String fallback = null;
    while (all.hasMoreElements()) {
      NetworkInterface n = all.nextElement();
      if (!n.isUp() || n.isLoopback())
        continue;
      Enumeration<InetAddress> addrs = n.getInetAddresses();
      while (addrs.hasMoreElements()) {
        InetAddress a = addrs.nextElement();
        if (a instanceof Inet4Address && !a.isLoopbackAddress() && !a.isLinkLocalAddress()) {
          String ip = a.getHostAddress();
          if (n.getName().startsWith("wlan") || n.getName().startsWith("ap"))
            return "http://" + ip + ":" + getListeningPort() + "/" + token + "/";
          if (fallback == null)
            fallback = ip;
        }
      }
    }
    if (fallback == null)
      throw new IOException("No local network address");
    return "http://" + fallback + ":" + getListeningPort() + "/" + token + "/";
  }
  String shortAddress() throws Exception {
    String full = address();
    return full.substring(0, full.indexOf("/", 7) + 1);
  }
  String accessCode() {
    return accessCode;
  }
  private synchronized boolean checkCode(String candidate) {
    long now = System.currentTimeMillis();
    if (now < blockedUntil)
      return false;
    if (accessCode.equalsIgnoreCase(candidate)) {
      failedAttempts = 0;
      return true;
    }
    if (++failedAttempts >= 5) {
      failedAttempts = 0;
      blockedUntil = now + 30000;
    }
    return false;
  }
  String getText() {
    return sharedText;
  }
  void setText(String t) {
    sharedText = t == null ? "" : t;
  }
  private static String esc(String s) {
    if (s == null)
      return "";
    return s.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace("\"", "&quot;")
        .replace("'", "&#39;");
  }
  private static String enc(String s) {
    try {
      return java.net.URLEncoder.encode(s, "UTF-8");
    } catch (Exception e) {
      return "";
    }
  }
  private static Response text(Response.Status st, String s) {
    return newFixedLengthResponse(st, "text/plain; charset=utf-8", s);
  }
  private synchronized String register(String value) {
    for (Map.Entry<String, String> e : folders.entrySet())
      if (e.getValue().equals(value))
        return e.getKey();
    String id = UUID.randomUUID().toString();
    folders.put(id, value);
    return id;
  }
  private synchronized String folder(String id) {
    return folders.get(id);
  }
  private static String name(String s) throws IOException {
    if (s == null || s.length() == 0 || s.length() > 200 || s.equals(".") || s.equals("..")
        || s.indexOf('/') >= 0 || s.indexOf('\\') >= 0 || s.indexOf('\0') >= 0)
      throw new IOException("Invalid name");
    return s;
  }
  private File child(String parent, String n) throws IOException {
    File f = new File(parent, name(n)).getCanonicalFile();
    if (!f.getParentFile().getCanonicalPath().equals(parent))
      throw new IOException("Invalid path");
    return f;
  }
  private synchronized Object create(String parent, String requested, boolean dir)
      throws Exception {
    String n = name(requested);
    int dot = n.lastIndexOf('.');
    if (dot <= 0 || dir)
      dot = n.length();
    for (int i = 0; i < 10000; i++) {
      String candidate = i == 0 ? n : n.substring(0, dot) + " (" + i + ")" + n.substring(dot);
      if (tree == null) {
        File f = child(parent, candidate);
        if (dir ? f.mkdir() : f.createNewFile())
          return f;
        if (!f.exists())
          throw new IOException("Cannot create file");
      } else {
        boolean exists = false;
        for (WebDocuments.Entry e : WebDocuments.list(context, tree, parent))
          if (candidate.equals(e.name)) {
            exists = true;
            break;
          }
        if (exists)
          continue;
        Uri u = WebDocuments.create(context, tree, parent, candidate,
            dir ? "vnd.android.document/directory" : "application/octet-stream");
        if (u == null)
          throw new IOException("Cannot create file");
        return u;
      }
    }
    throw new IOException("Too many files");
  }
  private List<WebDocuments.Entry> list(String parent) throws IOException {
    if (tree != null)
      return WebDocuments.list(context, tree, parent);
    ArrayList<WebDocuments.Entry> result = new ArrayList<WebDocuments.Entry>();
    File[] files = new File(parent).listFiles();
    if (files == null)
      throw new IOException("Cannot read folder");
    for (File f : files) {
      try {
        if (f.getCanonicalFile().getParentFile().getCanonicalPath().equals(parent))
          result.add(new WebDocuments.Entry(
              f.getCanonicalPath(), f.getName(), f.length(), f.isDirectory(), f.lastModified()));
      } catch (IOException ignored) {
      }
    }
    return result;
  }
  private InputStream open(String id) throws IOException {
    if (tree == null)
      return new FileInputStream(id);
    InputStream in = context.getContentResolver().openInputStream(WebDocuments.uri(tree, id));
    if (in == null)
      throw new IOException("Cannot read file");
    return in;
  }
  private OutputStream output(Object target) throws IOException {
    if (target instanceof File)
      return new FileOutputStream((File) target);
    OutputStream out = context.getContentResolver().openOutputStream((Uri) target, "w");
    if (out == null)
      throw new IOException("Cannot write file");
    return out;
  }
  private void discard(Object target) {
    if (target instanceof File)
      ((File) target).delete();
    else if (target instanceof Uri)
      WebDocuments.delete(context, (Uri) target);
  }

  private static String formatSize(long size) {
    if (size < 0) return "-";
    String[] units = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = size;
    int unit = 0;
    while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
    return unit == 0 ? size + " B" : String.format(Locale.US, "%.1f %s", value, units[unit]);
  }

  private static String formatModified(long time) {
    if (time <= 0) return "-";
    return new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(new Date(time));
  }
  public Response serve(IHTTPSession session) {
    Response response = serveRequest(session);
    response.closeConnection(true);
    return response;
  }
  private Response serveRequest(IHTTPSession session) {
    try {
      String prefix = "/" + token + "/", path = session.getUri();
      if ("/".equals(path) && Method.GET.equals(session.getMethod())) {
        Response login = newFixedLengthResponse(Response.Status.OK, "text/html; charset=utf-8",
            "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>ALTEK Link</title></head><body style='font:16px sans-serif;max-width:400px;margin:3em auto;padding:1em'><h1>ALTEK Link</h1><p>Enter the four-letter code displayed on the sharing device.</p><form method=post action=/login><input name=code maxlength=4 autocomplete=off autocapitalize=characters required style='font-size:24px;text-transform:uppercase;width:5em'><button style='font-size:20px'>Connect</button></form></body></html>");
        login.addHeader("Cache-Control", "no-store");
        return login;
      }
      if ("/login".equals(path) && Method.POST.equals(session.getMethod())) {
        String length = session.getHeaders().get("content-length");
        long n = length == null ? -1 : Long.parseLong(length);
        if (n < 0 || n > 128)
          return text(Response.Status.BAD_REQUEST, "Invalid request");
        byte[] body = new byte[(int) n];
        new DataInputStream(session.getInputStream()).readFully(body);
        String form = new String(body, "UTF-8");
        String candidate =
            form.startsWith("code=") ? URLDecoder.decode(form.substring(5), "UTF-8") : "";
        if (!checkCode(candidate))
          return text(Response.Status.FORBIDDEN,
              "Incorrect code or too many attempts. Please wait and try again.");
        Response redirect = newFixedLengthResponse(
            Response.Status.REDIRECT_SEE_OTHER, "text/plain", "Connecting...");
        redirect.addHeader("Location", prefix);
        redirect.addHeader("Cache-Control", "no-store");
        return redirect;
      }
      if (!path.startsWith(prefix))
        return text(Response.Status.NOT_FOUND, "Not found");
      String op = path.substring(prefix.length());
      Map<String, String> q = session.getParms();
      String dir = folder(q.containsKey("d") ? q.get("d") : "root");
      if (dir == null)
        return text(Response.Status.NOT_FOUND, "Folder not found");
      if ("file".equals(op) && Method.GET.equals(session.getMethod())) {
        String id = q.get("id"), target = folder(id);
        if (target == null)
          return text(Response.Status.NOT_FOUND, "File not found");
        long size = -1;
        String filename = "download";
        boolean found = false;
        for (WebDocuments.Entry e : list(dir))
          if (e.id.equals(target) && !e.folder) {
            size = e.size;
            filename = e.name;
            found = true;
            break;
          }
        if (!found)
          return text(Response.Status.NOT_FOUND, "File unavailable");
        String type = contentType(filename);
        int group = fileGroup(filename);
        boolean inline = group >= 0 && showInBrowser(group);
        Response response = size < 0
            ? newChunkedResponse(Response.Status.OK, type, open(target))
            : newFixedLengthResponse(Response.Status.OK, type, open(target), size);
        response.addHeader("Content-Disposition",
            (inline ? "inline" : "attachment") + "; filename=\"download\"; filename*=UTF-8''"
                + enc(filename).replace("+", "%20"));
        response.addHeader("X-Content-Type-Options", "nosniff");
        response.addHeader("Content-Security-Policy", "sandbox");
        return response;
      }
      if ("upload".equals(op) && Method.POST.equals(session.getMethod())) {
        long left = Long.parseLong(session.getHeaders().get("content-length"));
        if (left < 0)
          throw new IOException("Invalid size");
        Object target = create(dir, q.get("name"), false);
        boolean ok = false;
        try {
          OutputStream out = output(target);
          try {
            byte[] b = new byte[65536];
            InputStream in = session.getInputStream();
            while (left > 0) {
              int n = in.read(b, 0, (int) Math.min(left, b.length));
              if (n < 0)
                throw new EOFException();
              out.write(b, 0, n);
              left -= n;
            }
          } finally {
            out.close();
          }
          ok = true;
        } finally {
          if (!ok)
            discard(target);
        }
        return text(Response.Status.OK, "Uploaded");
      }
      if ("mkdir".equals(op) && Method.POST.equals(session.getMethod())) {
        create(dir, q.get("name"), true);
        return text(Response.Status.OK, "Folder created");
      }
      if ("text".equals(op) && Method.POST.equals(session.getMethod())) {
        long n = Long.parseLong(session.getHeaders().get("content-length"));
        if (n < 0 || n > 65536)
          throw new IOException("Text too long");
        byte[] b = new byte[(int) n];
        new DataInputStream(session.getInputStream()).readFully(b);
        setText(new String(b, "UTF-8"));
        return text(Response.Status.OK, "Text saved");
      }
      if (op.length() != 0 || !Method.GET.equals(session.getMethod()))
        return text(Response.Status.NOT_FOUND, "Not found");
      StringBuilder html = new StringBuilder(
          "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
          + "<meta charset=utf-8><title>ALTEK Link</title><style>"
          + "*{box-sizing:border-box}body{margin:0;background:#f1f5f9;color:#263d51;font:16px Arial,sans-serif}"
          + "header{background:#193782;color:white;padding:22px 16px;text-align:center}"
          + "header h1{margin:0;font-size:26px}header p{margin:6px 0 0;opacity:.85}"
          + "main{max-width:780px;margin:auto;padding:22px 16px}section{background:white;border-radius:10px;"
          + "padding:18px;margin-bottom:16px;box-shadow:0 2px 10px #19378212}"
          + "h2{font-size:18px;margin:0 0 14px}a{color:#244d95;text-decoration:none}"
          + "a:hover{text-decoration:underline}ul{list-style:none;margin:0;padding:0}"
          + "li a,.file-head{display:grid;grid-template-columns:minmax(0,1fr) 95px 145px;gap:12px;"
          + "align-items:center;padding:12px;border-bottom:1px solid #edf1f5;overflow-wrap:anywhere}"
          + ".file-head{color:#64748b;font-size:13px;font-weight:bold}.meta{white-space:nowrap}"
          + "@media (orientation:portrait){li a,.file-head{grid-template-columns:minmax(0,1fr)}"
          + ".meta{display:none}}"
          + "li a:hover{background:#f5f8fd}input,textarea,button{font:inherit}"
          + "input[type=text],input[type=file],textarea{width:100%;padding:10px;margin:4px 0 10px;"
          + "border:1px solid #cbd5e1;border-radius:6px}button{background:#193782;color:white;"
          + "border:0;border-radius:6px;padding:10px 18px;cursor:pointer}"
          + "button:hover{background:#28499b}progress{display:block;width:100%;margin-top:10px}"
          + "textarea{min-height:110px;resize:vertical}</style></head><body>"
          + "<header><h1>ALTEK Link</h1><p>Local file sharing</p></header><main>"
          + "<section><h2>Shared files</h2>");
      if (!"root".equals(q.get("d")) && q.containsKey("d"))
        html.append("<p><a href='./'>Shared folder</a></p>");
      html.append("<div class=file-head><span>Name</span><span class=meta>Size</span>"
          + "<span class=meta>Last modified</span></div><ul>");
      for (WebDocuments.Entry e : list(dir)) {
        String id = register(e.id);
        html.append("<li><a href='");
        if (e.folder)
          html.append("?d=").append(enc(id));
        else
          html.append("file?d=")
              .append(enc(q.containsKey("d") ? q.get("d") : "root"))
              .append("&amp;id=")
              .append(enc(id));
        html.append("'><span>")
            .append(e.folder ? "&#128193; " : "&#128196; ")
            .append(esc(e.name))
            .append("</span><span class=meta>")
            .append(e.folder ? "-" : formatSize(e.size))
            .append("</span><span class=meta>")
            .append(formatModified(e.modified))
            .append("</span></a></li>");
      }
      html.append("</ul>");
      String d = enc(q.containsKey("d") ? q.get("d") : "root");
      html.append(
              "</section><section><h2>Upload files</h2><input id=f type=file multiple><button onclick=upload()>Upload</button><progress id=p max=100 value=0></progress><h2>Create folder</h2><input type=text id=folder placeholder='New folder'><button onclick=mkdir()>Create folder</button></section><section><h2>Shared text</h2><textarea id=t>")
          .append(esc(sharedText))
          .append(
              "</textarea><button onclick=sendtext()>Send text</button><p id=status></p></section></main>");
      html.append("<script>var d='").append(d).append("';\n")
          .append("function post(url, body, done) {\n")
          .append("  var request = new XMLHttpRequest();\n")
          .append("  request.open('POST', url, true);\n")
          .append("  request.onload = function() {\n")
          .append("    if (request.status == 200) { if (done) done(); }\n")
          .append("    else alert(request.responseText);\n")
          .append("  };\n")
          .append("  request.onerror = function() { alert('Connection failed'); };\n")
          .append("  request.send(body);\n")
          .append("}\n")
          .append("function upload() {\n")
          .append("  var files = document.getElementById('f').files, index = 0;\n")
          .append("  function next() {\n")
          .append("    if (index >= files.length) { location.reload(); return; }\n")
          .append("    var file = files[index++], request = new XMLHttpRequest();\n")
          .append("    request.open('POST', 'upload?d=' + d + '&name=' + encodeURIComponent(file.name), true);\n")
          .append("    request.upload.onprogress = function(event) {\n")
          .append("      if (event.lengthComputable) document.getElementById('p').value = event.loaded / event.total * 100;\n")
          .append("    };\n")
          .append("    request.onload = function() { if (request.status == 200) next(); else alert(request.responseText); };\n")
          .append("    request.onerror = function() { alert('Upload failed'); };\n")
          .append("    request.send(file);\n")
          .append("  }\n")
          .append("  if (files.length) next();\n")
          .append("}\n")
          .append("function mkdir() {\n")
          .append("  post('mkdir?d=' + d + '&name=' + encodeURIComponent(document.getElementById('folder').value), '', function() { location.reload(); });\n")
          .append("}\n")
          .append("function sendtext() {\n")
          .append("  post('text?d=' + d, document.getElementById('t').value, function() {\n")
          .append("    document.getElementById('status').textContent = 'Text sent';\n")
          .append("  });\n")
          .append("}\n")
          .append("</script></body></html>");
      Response response =
          newFixedLengthResponse(Response.Status.OK, "text/html; charset=utf-8", html.toString());
      response.addHeader("Cache-Control", "no-store");
      response.addHeader("Referrer-Policy", "no-referrer");
      return response;
    } catch (Exception e) {
      return text(Response.Status.INTERNAL_ERROR, "Operation failed: " + e.getMessage());
    }
  }
}
