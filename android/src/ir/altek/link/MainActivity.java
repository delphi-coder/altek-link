package ir.altek.link;

import android.app.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.MediaRecorder;
import android.net.Uri;
import android.os.*;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.util.*;
import org.json.*;

/** Native recycled chat rows, Unicode text, stored contacts. No WebView or cloud SDK. */
public final class MainActivity extends Activity {
  private final Handler handler = new Handler();
  private LinearLayout root, body, toolbar;
  private TextView barTitle;
  private AvatarView barAvatar;
  private TextView subtitle;
  private ListView list;
  private Store db;
  private String selected, screen = "contacts", rendered = "", alerted = "";
  private EditText compose;
  private Runnable afterPermission;
  private final ArrayList<Store.Peer> peers = new ArrayList<Store.Peer>();
  private final ArrayList<Store.Msg> messages = new ArrayList<Store.Msg>();
  private String sharedText;
  private ArrayList<Uri> sharedFiles = new ArrayList<Uri>();
  private BaseAdapter adapter;
  private MediaRecorder recorder;
  private File voice;
  private Button voiceButton;
  private final BroadcastReceiver updates =
      new BroadcastReceiver() {
        public void onReceive(Context c, Intent i) {
          refresh();
        }
      };

  private int dp(int n) {
    return (int) (n * getResources().getDisplayMetrics().density + .5f);
  }

  private TextView label(String s, int size) {
    TextView t = new TextView(this);
    t.setText(s);
    t.setTextSize(size);
    t.setTextColor(Color.rgb(27, 45, 54));
    t.setPadding(dp(8), dp(6), dp(8), dp(6));
    return t;
  }

  private Button button(LinearLayout row, String text, View.OnClickListener click) {
    Button b = new Button(this);
    b.setText(text);
    b.setTextSize(12);
    b.setAllCaps(false);
    row.addView(b, new LinearLayout.LayoutParams(0, -2, 1));
    b.setOnClickListener(click);
    return b;
  }

  private LinearLayout row(LinearLayout parent) {
    LinearLayout l = new LinearLayout(this);
    l.setOrientation(LinearLayout.HORIZONTAL);
    parent.addView(l, new LinearLayout.LayoutParams(-1, -2));
    return l;
  }

  private void toast(String s) {
    Toast.makeText(this, s, Toast.LENGTH_LONG).show();
  }

  public void onCreate(Bundle b) {
    super.onCreate(b);
    db = new Store(this);
    getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
    startNetwork();
    root = new LinearLayout(this);
    root.setOrientation(1);
    root.setBackgroundColor(Color.rgb(243, 247, 248));
    toolbar = new LinearLayout(this);
    toolbar.setGravity(Gravity.CENTER_VERTICAL);
    toolbar.setBackgroundColor(Color.rgb(19, 111, 105));
    root.addView(toolbar, new LinearLayout.LayoutParams(-1, dp(56)));
    subtitle = new TextView(this); // Receiver status is shown in Settings, not above every chat.
    body = new LinearLayout(this);
    body.setOrientation(1);
    root.addView(body, new LinearLayout.LayoutParams(-1, 0, 1));
    setContentView(root);
    captureShare(getIntent());
    home();
    String p = getIntent().getStringExtra("peer");
    if (p != null) chat(p);
    if (Build.VERSION.SDK_INT >= 33)
      try {
        Activity.class
            .getMethod("requestPermissions", String[].class, int.class)
            .invoke(
                this,
                new Object[] {
                  new String[] {"android.permission.POST_NOTIFICATIONS"}, Integer.valueOf(55)
                });
      } catch (Exception e) {
        DebugLog.error("NOTIFICATION_PERMISSION", e);
      }
  }

  private void captureShare(Intent intent) {
    String action = intent.getAction();
    if (Intent.ACTION_SEND.equals(action) || Intent.ACTION_SEND_MULTIPLE.equals(action)) {
      CharSequence text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT);
      sharedText = text == null ? null : text.toString();
      sharedFiles.clear();
      if (Intent.ACTION_SEND_MULTIPLE.equals(action)) {
        ArrayList<Uri> uris = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
        if (uris != null) sharedFiles.addAll(uris);
      } else {
        Uri uri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
        if (uri != null) sharedFiles.add(uri);
      }
      if (sharedFiles.size() > 10) {
        sharedFiles = new ArrayList<Uri>(sharedFiles.subList(0, 10));
        toast("First 10 attachments selected");
      }
      toast("Choose a contact to share");
    }
  }

  private void deliverShare(final Store.Peer peer) {
    if (sharedText == null && sharedFiles.isEmpty()) return;
    if (!peer.approved) {
      toast("Unblock this contact to share");
      return;
    }
    new AlertDialog.Builder(this)
        .setTitle("Share with " + peer.name + "?")
        .setMessage(sharedFiles.size() + " attachments" + (sharedText == null ? "" : " and text"))
        .setPositiveButton(
            "Send",
            new DialogInterface.OnClickListener() {
              public void onClick(DialogInterface d, int n) {
                LinkService s = service();
                if (s == null) return;
                if (sharedText != null) {
                  if (sharedText.length() > 8000) {
                    toast("Shared text exceeds 8000 characters");
                    return;
                  }
                  s.text(peer.id, sharedText);
                }
                for (Uri uri : sharedFiles) s.offer(peer.id, uri, "file");
                sharedText = null;
                sharedFiles.clear();
              }
            })
        .setNegativeButton(
            "Cancel",
            new DialogInterface.OnClickListener() {
              public void onClick(DialogInterface d, int n) {
                sharedText = null;
                sharedFiles.clear();
              }
            })
        .show();
  }

  private void startNetwork() {
    Intent i = new Intent(this, LinkService.class);
    try {
      if (Build.VERSION.SDK_INT >= 26)
        Context.class.getMethod("startForegroundService", Intent.class).invoke(this, i);
      else startService(i);
    } catch (Exception e) {
      toast("Cannot start receiver");
    }
  }

  private LinkService service() {
    LinkService s = LinkService.current;
    if (s == null || !s.ready) {
      toast("Receiver is starting or stopped");
      return null;
    }
    return s;
  }

  protected void onNewIntent(Intent i) {
    super.onNewIntent(i);
    setIntent(i);
    captureShare(i);
    if (Intent.ACTION_SEND.equals(i.getAction())
        || Intent.ACTION_SEND_MULTIPLE.equals(i.getAction())) home();
    String p = i.getStringExtra("peer");
    if (p != null) chat(p);
    refresh();
  }

  protected void onResume() {
    super.onResume();
    registerReceiver(updates, new IntentFilter(LinkService.UPDATE));
    handler.post(tick);
  }

  protected void onPause() {
    handler.removeCallbacks(tick);
    unregisterReceiver(updates);
    cancelVoice();
    super.onPause();
  }

  private final Runnable tick =
      new Runnable() {
        public void run() {
          refresh();
          handler.postDelayed(this, 1500);
        }
      };

  private void home() {
    if (root.getWindowToken() != null)
      ((android.view.inputmethod.InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
          .hideSoftInputFromWindow(root.getWindowToken(), 0);
    cancelVoice();
    screen = "contacts";
    selected = null;
    body.removeAllViews();
    setBar("ALTEK Link", false, null);
    icon(
        toolbar,
        "menu",
        "Menu",
        true,
        new View.OnClickListener() {
          public void onClick(View v) {
            homeMenu();
          }
        });
    list = new ListView(this);
    list.setDivider(new android.graphics.drawable.ColorDrawable(Color.rgb(225, 234, 235)));
    list.setDividerHeight(dp(1));
    body.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
    TextView empty =
        label(
            "Looking for nearby phones...\n"
                + "Connect both phones to the same Wi-Fi or hotspot.\n"
                + "Use the menu for Add IP if needed.",
            15);
    empty.setGravity(Gravity.CENTER);
    body.addView(empty, new LinearLayout.LayoutParams(-1, 0, 1));
    list.setEmptyView(empty);
    adapter =
        new BaseAdapter() {
          public int getCount() {
            return peers.size();
          }

          public Object getItem(int n) {
            return peers.get(n);
          }

          public long getItemId(int n) {
            return n;
          }

          public View getView(int n, View recycled, android.view.ViewGroup parent) {
            LinearLayout line;
            if (recycled instanceof LinearLayout) line = (LinearLayout) recycled;
            else {
              line = new LinearLayout(MainActivity.this);
              line.setGravity(Gravity.CENTER_VERTICAL);
              line.setPadding(dp(12), dp(10), dp(12), dp(10));
              line.setMinimumHeight(dp(80));
              AvatarView avatar = new AvatarView(MainActivity.this);
              line.addView(avatar, new LinearLayout.LayoutParams(dp(48), dp(48)));
              LinearLayout texts = new LinearLayout(MainActivity.this);
              texts.setOrientation(1);
              TextView nameLabel = label("", 18), previewLabel = label("", 13);
              nameLabel.setPadding(dp(10), dp(3), dp(4), dp(3));
              previewLabel.setPadding(dp(10), dp(3), dp(4), dp(3));
              texts.addView(nameLabel, new LinearLayout.LayoutParams(-1, -2));
              texts.addView(previewLabel, new LinearLayout.LayoutParams(-1, -2));
              line.addView(texts, new LinearLayout.LayoutParams(0, -2, 1));
            }
            Store.Peer p = peers.get(n);
            ((AvatarView) line.getChildAt(0)).bind(p.id, p.name);
            LinearLayout texts = (LinearLayout) line.getChildAt(1);
            TextView title = (TextView) texts.getChildAt(0),
                detail = (TextView) texts.getChildAt(1);
            title.setSingleLine(true);
            title.setEllipsize(android.text.TextUtils.TruncateAt.END);
            title.setText(p.name);
            title.setTextColor(p.online() ? Color.BLACK : Color.rgb(140, 140, 140));
            detail.setTextColor(p.online() ? Color.BLACK : Color.rgb(150, 150, 150));
            detail.setSingleLine(true);
            detail.setEllipsize(android.text.TextUtils.TruncateAt.END);
            detail.setText(
                (!p.approved ? "Blocked" : p.online() ? "● Online" : "○ Offline")
                    + "  ·  "
                    + db.preview(p.id));
            return line;
          }
        };
    list.setAdapter(adapter);
    list.setOnItemClickListener(
        new AdapterView.OnItemClickListener() {
          public void onItemClick(AdapterView<?> a, View v, int n, long id) {
            chat(peers.get(n).id);
          }
        });
    refresh();
  }

  private void chat(String peer) {
    cancelVoice();
    selected = peer;
    screen = "chat";
    rendered = "";
    body.removeAllViews();
    final Store.Peer p = db.peer(peer);
    if (p == null) {
      home();
      return;
    }
    deliverShare(p);
    setBar(p.name, true, p.id);
    icon(
        toolbar,
        "audio",
        "Audio call",
        true,
        new View.OnClickListener() {
          public void onClick(View v) {
            dial("audio");
          }
        });
    icon(
        toolbar,
        "video",
        "Video call",
        true,
        new View.OnClickListener() {
          public void onClick(View v) {
            dial("video");
          }
        });
    icon(
        toolbar,
        "menu",
        "Contact menu",
        true,
        new View.OnClickListener() {
          public void onClick(View v) {
            chatMenu();
          }
        });
    list = new ListView(this);
    list.setDivider(null);
    list.setTranscriptMode(ListView.TRANSCRIPT_MODE_NORMAL);
    body.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
    adapter =
        new BaseAdapter() {
          public int getCount() {
            return messages.size();
          }

          public Object getItem(int n) {
            return messages.get(n);
          }

          public long getItemId(int n) {
            return n;
          }

          public View getView(int n, View recycled, android.view.ViewGroup parent) {
            LinearLayout container;
            if (recycled instanceof LinearLayout) container = (LinearLayout) recycled;
            else {
              container = new LinearLayout(MainActivity.this);
              container.setOrientation(1);
              container.setPadding(dp(8), dp(6), dp(8), dp(6));
              container.addView(label("", 16), new LinearLayout.LayoutParams(-2, -2));
              container.addView(label("", 10), new LinearLayout.LayoutParams(-2, -2));
            }
            Store.Msg m = messages.get(n);
            container.setGravity(m.mine ? Gravity.RIGHT : Gravity.LEFT);
            TextView text = (TextView) container.getChildAt(0),
                meta = (TextView) container.getChildAt(1);
            text.setTextColor(Color.rgb(20, 35, 45));
            android.graphics.drawable.GradientDrawable bubble =
                new android.graphics.drawable.GradientDrawable();
            bubble.setColor(m.mine ? Color.rgb(200, 238, 225) : Color.WHITE);
            bubble.setCornerRadius(dp(12));
            text.setBackgroundDrawable(bubble);
            text.setPadding(dp(12), dp(8), dp(12), dp(8));
            text.setMaxWidth(getResources().getDisplayMetrics().widthPixels - dp(55));
            String value = m.body;
            if (!"text".equals(m.kind))
              try {
                JSONObject j = new JSONObject(m.body);
                value =
                    ("voice".equals(m.kind) ? "Voice note: " : "File: ")
                        + j.getString("filename")
                        + "\n"
                        + (j.getLong("size") / 1024)
                        + " KB · Tap to "
                        + (m.path.length() > 0 ? "open" : "download");
              } catch (Exception ignored) {
              }
            text.setText(Emoji.render(MainActivity.this, value, text.getTextSize()));
            text.setGravity(Gravity.LEFT);
            meta.setText(android.text.format.DateFormat.format("HH:mm", m.time) + " · " + m.state);
            return container;
          }
        };
    list.setAdapter(adapter);
    list.setOnItemClickListener(
        new AdapterView.OnItemClickListener() {
          public void onItemClick(AdapterView<?> a, View v, int n, long id) {
            Store.Msg m = messages.get(n);
            if ("text".equals(m.kind)) {
              ((android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE))
                  .setPrimaryClip(ClipData.newPlainText("Message", m.body));
              toast("Copied");
            } else if (m.path.length() > 0) openFile(m);
            else {
              LinkService s = service();
              if (s != null) s.download(m);
            }
          }
        });
    LinearLayout input = row(body);
    input.setGravity(Gravity.BOTTOM);
    input.setBackgroundColor(Color.WHITE);
    icon(
        input,
        "emoji",
        "Emoji",
        false,
        new View.OnClickListener() {
          public void onClick(View v) {
            emojiPicker();
          }
        });
    compose = new EditText(this);
    compose.setTextSize(16);
    compose.setHint("Message / پیام");
    compose.setMaxLines(4);
    compose.setPadding(dp(4), dp(10), dp(4), dp(10));
    input.addView(compose, new LinearLayout.LayoutParams(0, -2, 1));
    icon(
        input,
        "attach",
        "Attach file",
        false,
        new View.OnClickListener() {
          public void onClick(View v) {
            pickFile();
          }
        });
    voiceButton = new Button(this);
    voiceButton.setMinWidth(0);
    voiceButton.setMinimumWidth(0);
    voiceButton.setPadding(dp(10), dp(10), dp(10), dp(10));
    voiceButton.setBackgroundColor(Color.TRANSPARENT);
    input.addView(voiceButton, new LinearLayout.LayoutParams(dp(44), dp(48)));
    voiceButton.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            Store.Peer target = db.peer(selected);
            if (target == null || !target.approved) {
              toast("Contact is blocked");
              return;
            }
            if (recorder != null) {
              finishVoice();
              return;
            }
            String text = compose.getText().toString();
            if (text.trim().length() > 0) {
              if (text.length() > 8000) {
                toast("Maximum 8000 characters");
                return;
              }
              LinkService s = service();
              if (s != null) {
                s.text(selected, text);
                compose.setText("");
                refresh();
              }
            } else
              permissions(
                  new String[] {"android.permission.RECORD_AUDIO"},
                  new Runnable() {
                    public void run() {
                      startVoice();
                    }
                  });
          }
        });
    compose.addTextChangedListener(
        new android.text.TextWatcher() {
          public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

          public void onTextChanged(CharSequence s, int start, int before, int count) {}

          public void afterTextChanged(android.text.Editable e) {
            Emoji.apply(MainActivity.this, e, compose.getTextSize());
            updateComposer();
          }
        });
    updateComposer();
    refresh();
  }

  private void trust(final Store.Peer p) {
    new AlertDialog.Builder(this)
        .setTitle("Contact info: " + p.name)
        .setMessage(
            "IP: "
                + p.ip
                + "\n\n"
                + "Optional identity check: compare this fingerprint with the other phone's"
                + " Settings.\n\n"
                + p.id
                + "\n\nLocal chat does not require manual verification. Camera access is separate.")
        .setPositiveButton("Close", null)
        .setNegativeButton(
            p.approved ? "Block" : "Unblock",
            new DialogInterface.OnClickListener() {
              public void onClick(DialogInterface d, int n) {
                db.trust(p.id, !p.approved);
                LinkService svc = LinkService.current;
                if (p.approved && svc != null && svc.call != null && p.id.equals(svc.call.peer))
                  svc.hangup();
                chat(p.id);
              }
            })
        .setNeutralButton("Access permissions", new DialogInterface.OnClickListener() {
          public void onClick(DialogInterface d, int n) {
            if (!p.approved) { toast("Unblock contact first"); return; }
            final int[] request = new int[] {0};
            final AlertDialog access = new AlertDialog.Builder(MainActivity.this)
                .setTitle("Access for " + p.name)
                .setMultiChoiceItems(new String[] {"Camera access", "Unattended remote control (root required)"},
                    new boolean[] {p.camera, p.remote}, new DialogInterface.OnMultiChoiceClickListener() {
                      public void onClick(DialogInterface dialog, int which, boolean checked) {
                        if (which == 0) { db.camera(p.id, checked); return; }
                        final AlertDialog box = (AlertDialog) dialog;
                        final int ticket = ++request[0];
                        if (!checked) { db.remote(p.id, false); return; }
                        // A tap alone never grants unattended access.
                        box.getListView().setItemChecked(1, false);
                        toast("Checking root access...");
                        new Thread(new Runnable() {
                          public void run() {
                            final RootShell.Result result = RootShell.probeFresh();
                            runOnUiThread(new Runnable() {
                              public void run() {
                                if (!box.isShowing() || request[0] != ticket) return;
                                Store.Peer current = db.peer(p.id);
                                if (result.granted && current != null && current.approved) {
                                  db.remote(p.id, true);
                                  box.getListView().setItemChecked(1, true);
                                  toast("Root access ready");
                                } else {
                                  db.remote(p.id, false);
                                  toast("Root access unavailable; remote calls will require an answer");
                                }
                              }
                            });
                          }
                        }, "remote-root-check").start();
                      }
                    })
                .setPositiveButton("Done", null).create();
            access.setOnDismissListener(new DialogInterface.OnDismissListener() {
              public void onDismiss(DialogInterface dialog) { request[0]++; }
            });
            access.show();
          }
        })
        .show();
  }

  private void setBar(String title, boolean back, final String peer) {
    toolbar.removeAllViews();
    barAvatar = null;
    if (back)
      icon(
          toolbar,
          "back",
          "Back",
          true,
          new View.OnClickListener() {
            public void onClick(View v) {
              home();
            }
          });
    if (peer != null) {
      barAvatar = new AvatarView(this);
      barAvatar.bind(peer, title);
      toolbar.addView(barAvatar, new LinearLayout.LayoutParams(dp(34), dp(34)));
    }
    barTitle = label(title, 18);
    barTitle.setTextColor(Color.WHITE);
    barTitle.setSingleLine(true);
    barTitle.setEllipsize(android.text.TextUtils.TruncateAt.END);
    toolbar.addView(barTitle, new LinearLayout.LayoutParams(0, -2, 1));
    if (peer != null) {
      View.OnClickListener open =
          new View.OnClickListener() {
            public void onClick(View v) {
              showProfile(peer);
            }
          };
      barTitle.setOnClickListener(open);
      barAvatar.setOnClickListener(open);
      barTitle.setContentDescription("View contact photo");
      barAvatar.setContentDescription("View contact photo");
    }
  }

  private void showProfile(String peer) {
    Store.Peer contact = db.peer(peer);
    if (contact == null) return;
    final Dialog dialog = new Dialog(this);
    dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
    FrameLayout backdrop = new FrameLayout(this);
    backdrop.setBackgroundColor(0xcc000000);
    int size =
        Math.min(
                getResources().getDisplayMetrics().widthPixels,
                getResources().getDisplayMetrics().heightPixels)
            - dp(48);
    android.graphics.Bitmap photo = Profile.bitmap(this, peer);
    View picture;
    if (photo != null) {
      ImageView image = new ImageView(this);
      image.setImageBitmap(photo);
      image.setScaleType(ImageView.ScaleType.FIT_CENTER);
      image.setContentDescription(contact.name);
      picture = image;
    } else {
      AvatarView avatar = new AvatarView(this);
      avatar.bind(peer, contact.name);
      picture = avatar;
    }
    FrameLayout.LayoutParams bounds = new FrameLayout.LayoutParams(size, size, Gravity.CENTER);
    backdrop.addView(picture, bounds);
    picture.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {}
        });
    backdrop.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            dialog.dismiss();
          }
        });
    dialog.setContentView(backdrop);
    dialog.setCancelable(true);
    dialog.setCanceledOnTouchOutside(true);
    Window window = dialog.getWindow();
    window.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
    window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
    window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
    dialog.show();
    window.setLayout(-1, -1);
  }

  private void icon(
      LinearLayout row,
      String kind,
      String description,
      boolean white,
      View.OnClickListener click) {
    ImageButton b = new ImageButton(this);
    b.setScaleType(ImageView.ScaleType.FIT_CENTER);
    b.setImageDrawable(new Icons(kind, white ? Color.WHITE : Color.rgb(19, 111, 105)));
    b.setContentDescription(description);
    b.setBackgroundColor(Color.TRANSPARENT);
    b.setPadding(dp(6), dp(9), dp(6), dp(9));
    row.addView(b, new LinearLayout.LayoutParams(dp(42), dp(48)));
    b.setOnClickListener(click);
    final String hint = description;
    b.setOnLongClickListener(
        new View.OnLongClickListener() {
          public boolean onLongClick(View v) {
            toast(hint);
            return true;
          }
        });
  }

  private void homeMenu() {
    new AlertDialog.Builder(this)
        .setTitle("ALTEK Link")
        .setItems(
            new String[] {"Settings / Profile", "Add IP", "Message all"},
            new DialogInterface.OnClickListener() {
              public void onClick(DialogInterface d, int n) {
                if (n == 0) settings();
                else if (n == 1) addIP();
                else broadcastText();
              }
            })
        .show();
  }

  private void addIP() {
    final EditText e = new EditText(this);
    e.setHint("192.168.100.12");
    new AlertDialog.Builder(this)
        .setTitle("Direct connection")
        .setView(e)
        .setPositiveButton(
            "Connect",
            new DialogInterface.OnClickListener() {
              public void onClick(DialogInterface d, int n) {
                LinkService s = service();
                if (s != null) s.probe(e.getText().toString().trim());
              }
            })
        .setNegativeButton("Cancel", null)
        .show();
  }

  private void chatMenu() {
    new AlertDialog.Builder(this)
        .setItems(
            new String[] {"View camera", "Request remote control", "Contact info / Camera access"},
            new DialogInterface.OnClickListener() {
              public void onClick(DialogInterface d, int n) {
                if (n == 2) {
                  Store.Peer p = db.peer(selected);
                  if (p != null) trust(p);
                } else if (n == 1) {
                  new AlertDialog.Builder(MainActivity.this)
                      .setTitle("Request remote control")
                      .setMessage(
                          "Asks the other phone for permission to see its screen and send taps and"
                              + " swipes to it. The other phone must be rooted. They can stop this at"
                              + " any time from their own screen.")
                      .setPositiveButton(
                          "Request",
                          new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface d, int n) {
                              dial("screen");
                            }
                          })
                      .setNegativeButton("Cancel", null)
                      .show();
                } else
                  new AlertDialog.Builder(MainActivity.this)
                      .setTitle("View other phone camera")
                      .setMessage(
                          "On the OTHER phone: open this contact's menu > Contact info > Allow"
                              + " camera access. Then Settings > Arm camera, and keep that screen"
                              + " open. Video only, no sound.")
                      .setPositiveButton(
                          "Connect",
                          new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface d, int n) {
                              dial("view");
                            }
                          })
                      .setNegativeButton("Cancel", null)
                      .show();
              }
            })
        .show();
  }

  private void pickFile() {
    Intent i = new Intent(Intent.ACTION_GET_CONTENT);
    i.setType("*/*");
    i.addCategory(Intent.CATEGORY_OPENABLE);
    try {
      startActivityForResult(Intent.createChooser(i, "Send file"), 10);
    } catch (Exception e) {
      toast("No file picker installed");
    }
  }

  private void emojiPicker() {
    final String[] emoji = Emoji.TEXT;
    CharSequence[] labels = new CharSequence[emoji.length];
    for (int i = 0; i < emoji.length; i++)
      labels[i] = Emoji.render(this, emoji[i] + "   " + Emoji.NAMES[i], dp(24));
    new AlertDialog.Builder(this)
        .setTitle("Emoji")
        .setItems(
            labels,
            new DialogInterface.OnClickListener() {
              public void onClick(DialogInterface d, int n) {
                int pos = Math.max(0, compose.getSelectionStart());
                compose.getText().insert(pos, emoji[n]);
              }
            })
        .show();
  }

  private void updateComposer() {
    if (voiceButton == null) return;
    String kind =
        recorder != null ? "stop" : compose != null && compose.length() > 0 ? "send" : "mic";
    Icons image = new Icons(kind, Color.rgb(19, 111, 105));
    image.setBounds(0, 0, dp(24), dp(24));
    voiceButton.setText("");
    voiceButton.setCompoundDrawables(image, null, null, null);
    voiceButton.setContentDescription(
        kind.equals("stop")
            ? "Stop and send voice note"
            : kind.equals("send") ? "Send message" : "Record voice note");
  }

  private void refresh() {
    LinkService s = LinkService.current;
    subtitle.setText(
        s != null && s.ready
            ? "Local receiver active · " + s.name()
            : "Receiver starting / stopped");
    if ("contacts".equals(screen)) {
      peers.clear();
      peers.addAll(db.peers());
      PeerOrder.sort(peers, System.currentTimeMillis());
      adapter.notifyDataSetChanged();
    } else if ("chat".equals(screen)) {
      Store.Peer contact = db.peer(selected);
      if (contact != null) {
        barTitle.setText(contact.name);
        if (barAvatar != null) barAvatar.bind(contact.id, contact.name);
      }
      ArrayList<Store.Msg> latest = db.messages(selected);
      StringBuilder stamp = new StringBuilder();
      for (Store.Msg m : latest) stamp.append(m.id).append(m.state).append(m.path);
      if (!rendered.equals(stamp.toString())) {
        boolean bottom = list.getLastVisiblePosition() >= messages.size() - 2;
        rendered = stamp.toString();
        messages.clear();
        messages.addAll(latest);
        adapter.notifyDataSetChanged();
        if (bottom) list.setSelection(messages.size() - 1);
      }
    }
    final LinkService.Call c = s == null ? null : s.call;
    if (c != null && !c.outgoing && !c.accepted && !c.id.equals(alerted)) {
      alerted = c.id;
      // The incoming request already launches IncomingCallActivity from LinkService;
      // this is a fallback in case that start was blocked.
      try {
        Intent inIntent = new Intent(this, IncomingCallActivity.class);
        inIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        startActivity(inIntent);
      } catch (Exception e) {
        DebugLog.error("INCOMING_UI", e);
      }
    }
  }

  private void broadcastText() {
    final EditText e = new EditText(this);
    new AlertDialog.Builder(this)
        .setTitle("Message online contacts")
        .setView(e)
        .setPositiveButton(
            "Send",
            new DialogInterface.OnClickListener() {
              public void onClick(DialogInterface d, int n) {
                LinkService s = service();
                String t = e.getText().toString();
                if (s == null || t.trim().isEmpty() || t.length() > 8000) return;
                int count = 0;
                for (Store.Peer p : db.peers())
                  if (p.approved && p.online()) {
                    s.text(p.id, t);
                    count++;
                  }
                toast("Queued for " + count + " contacts");
              }
            })
        .setNegativeButton("Cancel", null)
        .show();
  }

  private void dial(final String kind) {
    final String peer = selected;
    mediaPermissions(
        kind,
        new Runnable() {
          public void run() {
            try {
              LinkService s = service();
              if (s != null) {
                s.dial(peer, kind);
                startActivity(
                    new Intent(
                        MainActivity.this,
                        "screen".equals(kind) ? RemoteControlActivity.class : MediaActivity.class));
              }
            } catch (Exception e) {
              toast(e.getMessage());
            }
          }
        });
  }

  private void mediaPermissions(String kind, Runnable done) {
    if ("view".equals(kind) || "screen".equals(kind)) {
      done.run();
      return;
    }
    permissions(
        "video".equals(kind)
            ? new String[] {"android.permission.CAMERA", "android.permission.RECORD_AUDIO"}
            : new String[] {"android.permission.RECORD_AUDIO"},
        done);
  }

  private void permissions(String[] wanted, Runnable done) {
    try {
      if (Build.VERSION.SDK_INT < 23) {
        done.run();
        return;
      }
      ArrayList<String> missing = new ArrayList<String>();
      for (String p : wanted)
        if (((Integer)
                Activity.class.getMethod("checkSelfPermission", String.class).invoke(this, p))
            != PackageManager.PERMISSION_GRANTED) missing.add(p);
      if (missing.isEmpty()) {
        done.run();
        return;
      }
      afterPermission = done;
      Activity.class
          .getMethod("requestPermissions", String[].class, int.class)
          .invoke(
              this,
              new Object[] {missing.toArray(new String[missing.size()]), Integer.valueOf(44)});
    } catch (Exception e) {
      toast("Permission request failed");
      DebugLog.error("PERMISSION", e);
    }
  }

  public void onRequestPermissionsResult(int request, String[] p, int[] results) {
    if (request == 44) {
      Runnable r = afterPermission;
      afterPermission = null;
      boolean yes = results.length > 0;
      for (int n : results) yes &= n == PackageManager.PERMISSION_GRANTED;
      if (yes && r != null) r.run();
      else toast("Permission required for this feature");
    }
  }

  private void startVoice() {
    try {
      File folder = new File(getCacheDir(), "voice");
      folder.mkdirs();
      voice = new File(folder, Wire.uid() + ".3gp");
      recorder = new MediaRecorder();
      recorder.setAudioSource(android.media.MediaRecorder.AudioSource.MIC);
      recorder.setOutputFormat(MediaRecorder.OutputFormat.THREE_GPP);
      recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AMR_NB);
      recorder.setOutputFile(voice.getPath());
      recorder.setMaxDuration(120000);
      recorder.setOnInfoListener(
          new MediaRecorder.OnInfoListener() {
            public void onInfo(MediaRecorder m, int what, int extra) {
              if (what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_DURATION_REACHED) finishVoice();
            }
          });
      recorder.prepare();
      recorder.start();
      updateComposer();
      toast("Recording, tap the square to send (max 2 min)");
    } catch (Exception e) {
      cancelVoice();
      toast("Recording failed");
      DebugLog.error("VOICE", e);
    }
  }

  private void finishVoice() {
    if (recorder == null) return;
    File f = voice;
    try {
      recorder.stop();
      recorder.release();
      recorder = null;
      voice = null;
      updateComposer();
      LinkService s = service();
      if (s != null) s.offer(selected, Uri.fromFile(f), "voice");
    } catch (Exception e) {
      cancelVoice();
      toast("Recording too short or failed");
    }
  }

  private void cancelVoice() {
    if (recorder != null) {
      try {
        recorder.stop();
      } catch (Exception ignored) {
      }
      recorder.release();
      recorder = null;
      if (voice != null) voice.delete();
      voice = null;
      if (voiceButton != null) updateComposer();
    }
  }

  protected void onActivityResult(int request, int result, Intent data) {
    super.onActivityResult(request, result, data);
    if (request == 20 && result == RESULT_OK && data != null && data.getData() != null) {
      startActivityForResult(new Intent(this, CropActivity.class).setData(data.getData()), 21);
      return;
    }
    if (request == 21) {
      settings();
      return;
    }
    if (request == 10 && result == RESULT_OK && data != null && selected != null) {
      LinkService s = service();
      if (s != null) s.offer(selected, data.getData(), "file");
    }
  }

  private void openFile(Store.Msg m) {
    try {
      JSONObject j = new JSONObject(m.body);
      String mime =
          Mime.resolve(
              j.optString("mime", Mime.GENERIC), j.optString("filename", null), new File(m.path));
      Uri uri = Uri.parse("content://ir.altek.link.files/" + m.id);
      Intent i =
          new Intent(Intent.ACTION_VIEW)
              .setDataAndType(uri, mime)
              .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
      // ClipData makes sure the read grant is propagated through the chooser to the target app.
      i.setClipData(ClipData.newRawUri("attachment", uri));
      startActivity(Intent.createChooser(i, "Open attachment"));
    } catch (ActivityNotFoundException e) {
      toast("No app can open this file");
    } catch (Exception e) {
      DebugLog.error("OPEN_FILE", e);
      toast("Could not open this file");
    }
  }

  private EditText screenOption(LinearLayout panel, String title, int value) {
    panel.addView(label(title, 14));
    EditText edit = new EditText(this);
    edit.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
    edit.setSingleLine(true);
    edit.setText(Integer.toString(value));
    panel.addView(edit);
    return edit;
  }

  private void screenVideoSettings() {
    if (LinkService.current != null && LinkService.current.call != null) {
      toast("End the call before changing screen settings");
      return;
    }
    ScreenOptions current = ScreenOptions.load(this);
    LinearLayout panel = new LinearLayout(this);
    panel.setOrientation(LinearLayout.VERTICAL);
    panel.setPadding(dp(20), dp(4), dp(20), dp(4));
    final EditText size = screenOption(panel, "Maximum long side (320?1920 px)", current.maxSide);
    final EditText fps = screenOption(panel, "Encoder target FPS (1?60)", current.fps);
    final EditText bitrate = screenOption(panel, "Bitrate (100?20000 kbps)", current.bitrate / 1000);
    final EditText key = screenOption(panel, "Key-frame interval (1?10 seconds)", current.keySeconds);
    panel.addView(label("Applies to the next screen share. Actual FPS depends on the device. "
        + "Root screenrecord supports size and bitrate; its FPS and key-frame interval are system controlled.", 12));
    ScrollView scroll = new ScrollView(this);
    scroll.addView(panel);
    final AlertDialog dialog = new AlertDialog.Builder(this).setTitle("Screen share settings")
        .setView(scroll).setPositiveButton("Save", null).setNegativeButton("Cancel", null)
        .setNeutralButton("Defaults", null).create();
    dialog.setOnShowListener(new DialogInterface.OnShowListener() {
      public void onShow(DialogInterface ignored) {
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(new View.OnClickListener() {
          public void onClick(View v) {
            size.setText("1280"); fps.setText("24"); bitrate.setText("6000"); key.setText("2");
          }
        });
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(new View.OnClickListener() {
          public void onClick(View v) {
            try {
              int max = Integer.parseInt(size.getText().toString());
              int rate = Integer.parseInt(fps.getText().toString());
              int kbps = Integer.parseInt(bitrate.getText().toString());
              int seconds = Integer.parseInt(key.getText().toString());
              if (!ScreenOptions.valid(max, rate, kbps, seconds)) throw new IllegalArgumentException();
              ScreenOptions saved = new ScreenOptions();
              saved.maxSide = max; saved.fps = rate; saved.bitrate = kbps * 1000; saved.keySeconds = seconds;
              saved.save(MainActivity.this);
              dialog.dismiss();
              toast("Screen share settings saved");
            } catch (Exception e) {
              toast("Check the screen share setting ranges");
            }
          }
        });
      }
    });
    dialog.show();
  }

  private void settings() {
    cancelVoice();
    screen = "settings";
    setBar("Settings", true, null);
    body.removeAllViews();
    ScrollView scroll = new ScrollView(this);
    LinearLayout panel = new LinearLayout(this);
    panel.setOrientation(1);
    scroll.addView(panel);
    body.addView(scroll);
    LinearLayout videoRow = row(panel);
    button(
        videoRow,
        "Video settings / تنظیمات ویدیو",
        new View.OnClickListener() {
          public void onClick(View v) {
            if (LinkService.current != null && LinkService.current.call != null) {
              toast("End the call before changing video settings");
              return;
            }
            permissions(
                new String[] {"android.permission.CAMERA"},
                new Runnable() {
                  public void run() {
                    startActivity(new Intent(MainActivity.this, VideoSettingsActivity.class));
                  }
                });
          }
        });
    LinearLayout screenRow = row(panel);
    button(screenRow, "Screen share settings", new View.OnClickListener() {
      public void onClick(View v) { screenVideoSettings(); }
    });
    final SharedPreferences prefs = getSharedPreferences("settings", 0);
    final AvatarView ownAvatar = new AvatarView(this);
    ownAvatar.bind("self", prefs.getString("name", Build.MODEL));
    panel.addView(ownAvatar, new LinearLayout.LayoutParams(dp(80), dp(80)));
    LinearLayout photoRow = row(panel);
    button(
        photoRow,
        "Choose photo",
        new View.OnClickListener() {
          public void onClick(View v) {
            Intent i =
                new Intent(Intent.ACTION_GET_CONTENT)
                    .setType("image/*")
                    .addCategory(Intent.CATEGORY_OPENABLE);
            try {
              startActivityForResult(Intent.createChooser(i, "Profile photo"), 20);
            } catch (Exception e) {
              toast("No gallery installed");
            }
          }
        });
    button(
        photoRow,
        "Remove photo",
        new View.OnClickListener() {
          public void onClick(View v) {
            Profile.remove(MainActivity.this);
            ownAvatar.invalidate();
          }
        });
    panel.addView(label("Photo is visible to unblocked devices on this local network.", 12));
    panel.addView(
        label(
            LinkService.current != null && LinkService.current.ready
                ? "Local receiver active"
                : "Local receiver stopped / starting",
            12));
    panel.addView(label("Display name", 14));
    final EditText name = new EditText(this);
    name.setText(prefs.getString("name", Build.MODEL));
    panel.addView(name);
    LinearLayout controls = row(panel);
    button(
        controls,
        "Save",
        new View.OnClickListener() {
          public void onClick(View v) {
            String n = name.getText().toString().trim();
            if (n.length() == 0 || n.length() > 60) {
              toast("Name must be 1-60 characters");
              return;
            }
            prefs.edit().putString("name", n).apply();
            ownAvatar.bind("self", n);
            DebugLog.event("SETTINGS", "Updated");
            toast("Saved");
          }
        });
    button(
        controls,
        "Start receiver",
        new View.OnClickListener() {
          public void onClick(View v) {
            startNetwork();
          }
        });
    button(
        controls,
        "Stop receiver",
        new View.OnClickListener() {
          public void onClick(View v) {
            prefs.edit().putBoolean("enabled", false).apply();
            stopService(new Intent(MainActivity.this, LinkService.class));
            refresh();
          }
        });
    LinearLayout media = row(panel);
    button(
        media,
        "Arm camera",
        new View.OnClickListener() {
          public void onClick(View v) {
            permissions(
                new String[] {"android.permission.CAMERA"},
                new Runnable() {
                  public void run() {
                    LinkService s = service();
                    if (s != null) {
                      if (s.call != null) {
                        toast("End the current call first");
                        return;
                      }
                      startActivity(
                          new Intent(MainActivity.this, MediaActivity.class)
                              .putExtra("cameraMode", true));
                    }
                  }
                });
          }
        });
    button(
        media,
        "Battery settings",
        new View.OnClickListener() {
          public void onClick(View v) {
            try {
              startActivity(new Intent("android.settings.IGNORE_BATTERY_OPTIMIZATION_SETTINGS"));
            } catch (Exception e) {
              startActivity(new Intent(android.provider.Settings.ACTION_SETTINGS));
            }
          }
        });
    LinkService s = LinkService.current;
    if (s != null && s.identity != null) {
      TextView fingerprint =
          label("My fingerprint (compare on other phone):\n" + s.identity.id, 13);
      fingerprint.setTextIsSelectable(true);
      panel.addView(fingerprint);
    }
    panel.addView(
        label(
            "Local IPv4 addresses:\n"
                + addresses()
                + "\n\n"
                + "Receiver stays active with a notification after leaving the app. Force Stop"
                + " disables it until reopened. Camera mode must remain on its screen.\n\n"
                + "Version 0.1.17 · Android 4.1+\n"
                + "The 10 built-in emoji use Twemoji artwork (Twitter / contributors), CC-BY 4.0."
                + " github.com/jdecked/twemoji. Other keyboard emoji use device fonts.",
            13));
  }

  private String addresses() {
    StringBuilder s = new StringBuilder();
    try {
      Enumeration<java.net.NetworkInterface> n = java.net.NetworkInterface.getNetworkInterfaces();
      while (n.hasMoreElements()) {
        Enumeration<java.net.InetAddress> a = n.nextElement().getInetAddresses();
        while (a.hasMoreElements()) {
          java.net.InetAddress ip = a.nextElement();
          if (ip instanceof java.net.Inet4Address && !ip.isLoopbackAddress())
            s.append(ip.getHostAddress()).append('\n');
        }
      }
    } catch (Exception ignored) {
    }
    return s.toString();
  }

  public void onBackPressed() {
    if (!"contacts".equals(screen)) home();
    else super.onBackPressed();
  }
}
