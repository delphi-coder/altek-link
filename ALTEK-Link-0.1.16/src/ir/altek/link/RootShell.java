package ir.altek.link;

import java.io.*;

/**
 * Root-shell helper. Two things make this more finicky than it looks:
 *
 * <p>1. On most root setups (Magisk, SuperSU, ...), the very FIRST time this app asks for root, the
 * root manager shows an interactive "Grant?" prompt that the person has to notice and tap - that can
 * take much longer than a normal command. So the first probe uses a long timeout; once it succeeds,
 * root managers remember the grant for this app and later calls are fast, so those keep a short one.
 * A timed-out probe is NOT cached as "not rooted": that would be a false negative (root is there,
 * the person just hadn't answered the prompt yet), and would wrongly stick until the app restarts.
 *
 * <p>2. Not every `su` accepts `su -c "command"`. Some (older or vendor-specific builds) only work
 * in the classic interactive style: start `su` with no arguments, write the command to its stdin,
 * then close stdin. This is the more universally compatible form, so it is what this class uses.
 *
 * <p>3. `su` is not always resolved by a plain exec("su") call - the environment an app's own child
 * processes run in does not always include the directory holding `su`, unlike an interactive adb
 * shell. A short list of common install locations is tried as a fallback.
 */
public final class RootShell {
  private static final String[] CANDIDATES = {
    "su", "/system/xbin/su", "/system/bin/su", "/sbin/su", "/su/bin/su"
  };
  private static volatile String working; // candidate confirmed to at least start a process
  private static volatile boolean grantedOnce;

  private RootShell() {}

  public static final class Result {
    public final boolean granted;
    public final String detail; // reason when not granted: which candidates failed and how

    Result(boolean granted, String detail) {
      this.granted = granted;
      this.detail = detail;
    }
  }

  /**
   * Checks root access, waiting long enough for a root manager's one-time grant prompt. Call this
   * once before relying on root; after a true result, later run()/runBytes() calls are expected to
   * be fast (already granted) and use their own shorter timeout.
   */
  public static Result probe() {
    if (grantedOnce) return new Result(true, "already granted this session");
    StringBuilder detail = new StringBuilder();
    for (String candidate : orderedCandidates()) {
      try {
        Exec e = spawn(candidate);
        byte[] out = finish(e, "id\nexit\n".getBytes("UTF-8"), 20000);
        String text = new String(out, "UTF-8");
        if (text.contains("uid=0")) {
          working = candidate;
          grantedOnce = true;
          return new Result(true, candidate);
        }
        detail
            .append(candidate)
            .append(": denied or not root (")
            .append(firstLine(text))
            .append("); ");
      } catch (TimeoutException e) {
        // su exists and started, it just never answered - almost certainly an unanswered grant
        // prompt. Don't try other candidates (this is the real one) and don't cache this as a
        // permanent "no root": the person may still grant it and the next attempt should re-check.
        return new Result(
            false,
            candidate
                + " did not respond within 20s - check your root manager app for a pending"
                + " permission request");
      } catch (IOException e) {
        detail.append(candidate).append(": ").append(e.getMessage()).append("; ");
      } catch (Exception e) {
        detail.append(candidate).append(": ").append(e).append("; ");
      }
    }
    return new Result(false, detail.length() == 0 ? "no su binary found" : detail.toString());
  }

  /** Runs `cmd` as root; returns combined stdout+stderr as text, trimmed. For text output only. */
  public static String run(String cmd, long timeoutMs) throws Exception {
    byte[] out = runBytes(cmd, timeoutMs);
    return new String(out, "UTF-8").trim();
  }

  /** Runs `cmd` as root; returns raw stdout+stderr bytes. Binary-safe (PNG screenshots, ...). */
  public static byte[] runBytes(String cmd, long timeoutMs) throws Exception {
    String candidate = working != null ? working : "su";
    try {
      Exec e = spawn(candidate);
      return finish(e, (cmd + "\nexit\n").getBytes("UTF-8"), timeoutMs);
    } catch (IOException firstFailure) {
      if (working != null) throw firstFailure; // a previously-working su stopped working
      for (String c : CANDIDATES) {
        if (c.equals(candidate)) continue;
        try {
          Exec e = spawn(c);
          byte[] out = finish(e, (cmd + "\nexit\n").getBytes("UTF-8"), timeoutMs);
          working = c;
          return out;
        } catch (Exception ignored) {
        }
      }
      throw firstFailure;
    }
  }

  /**
   * Starts a long-running root command (its own process tree): spawns a root shell, writes `cmd` to
   * its stdin, then leaves stdin open so the shell stays attached to it as a foreground job. Caller
   * reads getInputStream() for as long as the command runs, and must destroy() the returned process
   * to stop it - on devices whose root shell does not forward that signal to the grandchild it
   * spawned, the caller should also kill the command by name via {@link #run} as a backstop.
   */
  public static Process startInteractive(String cmd) throws IOException {
    String candidate = working != null ? working : "su";
    Process p;
    try {
      p = new ProcessBuilder(candidate).redirectErrorStream(false).start();
    } catch (IOException first) {
      Process found = null;
      for (String c : CANDIDATES) {
        if (c.equals(candidate)) continue;
        try {
          found = new ProcessBuilder(c).redirectErrorStream(false).start();
          working = c;
          break;
        } catch (Exception ignored) {
        }
      }
      if (found == null) throw first;
      p = found;
    }
    OutputStream os = p.getOutputStream();
    os.write((cmd + "\n").getBytes("UTF-8"));
    os.flush();
    return p;
  }

  private static String[] orderedCandidates() {
    if (working == null) return CANDIDATES;
    String[] r = new String[CANDIDATES.length];
    r[0] = working;
    int i = 1;
    for (String c : CANDIDATES) if (!c.equals(working)) r[i++] = c;
    return r;
  }

  private static String firstLine(String s) {
    int i = s.indexOf('\n');
    String line = (i < 0 ? s : s.substring(0, i)).trim();
    return line.length() == 0 ? "no output" : line;
  }

  // ------------------------------------------------------------------------------ process plumbing

  private static final class Exec {
    final Process process;

    Exec(Process p) {
      process = p;
    }
  }

  private static final class TimeoutException extends IOException {
    TimeoutException(String m) {
      super(m);
    }
  }

  private static Exec spawn(String suPath) throws IOException {
    return new Exec(new ProcessBuilder(suPath).redirectErrorStream(true).start());
  }

  /** Writes stdin, closes it, waits up to timeoutMs for exit, and returns everything read. */
  private static byte[] finish(Exec e, byte[] stdin, long timeoutMs) throws Exception {
    final Process p = e.process;
    final ByteArrayOutputStream out = new ByteArrayOutputStream();
    final IOException[] readFailure = new IOException[1];
    Thread reader =
        new Thread(
            new Runnable() {
              public void run() {
                try {
                  InputStream in = p.getInputStream();
                  byte[] buf = new byte[4096];
                  int n;
                  while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
                } catch (IOException ex) {
                  readFailure[0] = ex;
                }
              }
            },
            "su-stdout");
    reader.start();
    try {
      OutputStream os = p.getOutputStream();
      os.write(stdin);
      os.flush();
      os.close();
    } catch (IOException ignored) {
      // su may have already exited (e.g. rejected the request immediately); the exit-code / output
      // check below still applies.
    }
    boolean finished;
    try {
      finished = waitFor(p, timeoutMs);
    } finally {
      reader.join(500);
    }
    if (!finished) {
      try {
        p.destroy();
      } catch (Exception ignored) {
      }
      throw new TimeoutException("su timed out");
    }
    if (readFailure[0] != null) throw readFailure[0];
    return out.toByteArray();
  }

  /** Process.waitFor(timeout) needs API 26; polled manually so this keeps working on API 16. */
  private static boolean waitFor(Process p, long timeoutMs) throws InterruptedException {
    long deadline = System.currentTimeMillis() + timeoutMs;
    while (true) {
      try {
        p.exitValue();
        return true;
      } catch (IllegalThreadStateException stillRunning) {
        if (System.currentTimeMillis() >= deadline) return false;
        Thread.sleep(20);
      }
    }
  }
}
