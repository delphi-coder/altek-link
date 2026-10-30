package ir.altek.link;

import android.content.*;
import android.database.Cursor;
import android.database.sqlite.*;
import java.util.*;

public final class Store extends SQLiteOpenHelper {
  public Store(Context c) {
    super(c, "link.db", null, 2);
  }

  public void onCreate(SQLiteDatabase d) {
    d.execSQL(
        "CREATE TABLE peers(id TEXT PRIMARY KEY,name TEXT,ip TEXT,seen INTEGER,approved INTEGER"
            + " DEFAULT 1,camera INTEGER DEFAULT 0)");
    d.execSQL(
        "CREATE TABLE messages(id TEXT PRIMARY KEY,peer TEXT,kind TEXT,body TEXT,path TEXT,mine"
            + " INTEGER,state TEXT,time INTEGER)");
    d.execSQL("CREATE INDEX history ON messages(peer,time)");
  }

  public void onUpgrade(SQLiteDatabase d, int a, int b) {
    if (a < 2) d.execSQL("UPDATE peers SET approved=1");
  }

  public static class Peer {
    public String id, name, ip;
    public long seen;
    public boolean approved, camera;

    public boolean online() {
      return System.currentTimeMillis() - seen < 16000;
    }
  }

  public static class Msg {
    public String id, peer, kind, body, path, state;
    public boolean mine;
    public long time;
  }

  public synchronized void seen(String id, String name, String ip, boolean live) {
    if (id == null || !id.matches("[0-9a-f]{64}")) return;
    ContentValues v = new ContentValues();
    v.put("name", name.length() > 60 ? name.substring(0, 60) : name);
    v.put("ip", ip);
    if (live) v.put("seen", System.currentTimeMillis());
    if (getWritableDatabase().update("peers", v, "id=?", new String[] {id}) == 0) {
      v.put("id", id);
      v.put("approved", 1);
      if (!live) v.put("seen", 0);
      getWritableDatabase().insert("peers", null, v);
    }
  }

  public synchronized void trust(String id, boolean yes) {
    ContentValues v = new ContentValues();
    v.put("approved", yes ? 1 : 0);
    if (!yes) v.put("camera", 0);
    getWritableDatabase().update("peers", v, "id=?", new String[] {id});
  }

  public synchronized void camera(String id, boolean yes) {
    ContentValues v = new ContentValues();
    v.put("camera", yes ? 1 : 0);
    getWritableDatabase().update("peers", v, "id=?", new String[] {id});
  }

  public synchronized ArrayList<Peer> peers() {
    ArrayList<Peer> r = new ArrayList<Peer>();
    Cursor c =
        getReadableDatabase()
            .rawQuery("SELECT * FROM peers ORDER BY approved DESC,name COLLATE NOCASE", null);
    try {
      while (c.moveToNext()) {
        Peer p = new Peer();
        p.id = c.getString(0);
        p.name = c.getString(1);
        p.ip = c.getString(2);
        p.seen = c.getLong(3);
        p.approved = c.getInt(4) != 0;
        p.camera = c.getInt(5) != 0;
        r.add(p);
      }
    } finally {
      c.close();
    }
    return r;
  }

  public synchronized String preview(String peer) {
    Cursor c =
        getReadableDatabase()
            .rawQuery(
                "SELECT kind,body FROM messages WHERE peer=? ORDER BY time DESC LIMIT 1",
                new String[] {peer});
    try {
      if (!c.moveToFirst()) return "Tap to chat";
      String kind = c.getString(0);
      return "text".equals(kind)
          ? c.getString(1)
          : "voice".equals(kind) ? "Voice note" : "Attachment";
    } finally {
      c.close();
    }
  }

  public synchronized Peer peer(String id) {
    for (Peer p : peers()) if (p.id.equals(id)) return p;
    return null;
  }

  public synchronized boolean add(
      String id, String peer, String kind, String body, String path, boolean mine, String state) {
    Msg existing = message(id);
    if (existing != null) {
      if (!existing.peer.equals(peer)
          || existing.mine != mine
          || !existing.kind.equals(kind)
          || !existing.body.equals(body)) throw new SQLiteException("Message identity collision");
      return false;
    }
    ContentValues v = new ContentValues();
    v.put("id", id);
    v.put("peer", peer);
    v.put("kind", kind);
    v.put("body", body);
    v.put("path", path);
    v.put("mine", mine ? 1 : 0);
    v.put("state", state);
    v.put("time", System.currentTimeMillis());
    if (getWritableDatabase()
            .insertWithOnConflict("messages", null, v, SQLiteDatabase.CONFLICT_IGNORE)
        == -1) throw new SQLiteException("Message was not persisted");
    return true;
  }

  public synchronized void state(String id, String state) {
    ContentValues v = new ContentValues();
    v.put("state", state);
    getWritableDatabase().update("messages", v, "id=?", new String[] {id});
  }

  public synchronized void path(String id, String path, String state) {
    ContentValues v = new ContentValues();
    v.put("path", path);
    v.put("state", state);
    getWritableDatabase().update("messages", v, "id=?", new String[] {id});
  }

  public synchronized ArrayList<Msg> messages(String peer) {
    return query("peer=?", new String[] {peer}, "time DESC", "200", true);
  }

  public synchronized ArrayList<Msg> pending() {
    return query(
        "mine=1 AND state='queued' AND peer IN (SELECT id FROM peers WHERE approved=1 AND seen>=?)",
        new String[] {String.valueOf(System.currentTimeMillis() - 16000)},
        "time ASC, rowid ASC",
        "50",
        false);
  }

  public synchronized Msg message(String id) {
    ArrayList<Msg> r = query("id=?", new String[] {id}, "time ASC", "1", false);
    return r.isEmpty() ? null : r.get(0);
  }

  private ArrayList<Msg> query(
      String where, String[] args, String order, String limit, boolean reverse) {
    ArrayList<Msg> r = new ArrayList<Msg>();
    Cursor c = getReadableDatabase().query("messages", null, where, args, null, null, order, limit);
    try {
      while (c.moveToNext()) {
        Msg m = new Msg();
        m.id = c.getString(0);
        m.peer = c.getString(1);
        m.kind = c.getString(2);
        m.body = c.getString(3);
        m.path = c.getString(4);
        m.mine = c.getInt(5) != 0;
        m.state = c.getString(6);
        m.time = c.getLong(7);
        r.add(m);
      }
    } finally {
      c.close();
    }
    if (reverse) Collections.reverse(r);
    return r;
  }

  public synchronized void offline() {
    getWritableDatabase().execSQL("UPDATE peers SET seen=0");
  }
}
