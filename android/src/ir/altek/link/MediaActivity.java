package ir.altek.link;

import android.app.Activity;
import android.content.*;
import android.os.*;
import android.view.*;
import android.widget.*;

/** Calls require a visible media screen. Camera mode intentionally keeps screen on. */
public final class MediaActivity extends Activity implements TextureView.SurfaceTextureListener {
  private TextureView surface, preview;
  private FrameLayout frame;
  private boolean videoCall, swapped, smallVisible = true, moved;
  private float touchX, touchY, startLeft, startTop;
  private int smallLeft = -1, smallTop = -1;
  private long lastSmallTap;

  private Surface output;
  private int videoWidth = 320, videoHeight = 240;
  private TextView info;
  private ImageButton muteButton, speakerButton, flipButton, torchButton;
  private final Handler handler = new Handler();
  private MediaEngine engine;
  private LinkService.Call attached;
  private boolean cameraMode, surfaceReady, answered, active, failureShown;

  public void onCreate(Bundle b) {
    super.onCreate(b);
    cameraMode = getIntent().getBooleanExtra("cameraMode", false);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    LinearLayout root = new LinearLayout(this);
    root.setOrientation(1);
    info = new TextView(this);
    root.setBackgroundColor(android.graphics.Color.BLACK);
    info.setTextSize(10);
    info.setTextColor(0xffbbbbbb);
    info.setMaxLines(1);
    info.setEllipsize(android.text.TextUtils.TruncateAt.END);
    info.setPadding(dp(8), dp(3), dp(8), dp(3));
    info.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            Toast.makeText(MediaActivity.this, info.getText(), Toast.LENGTH_LONG).show();
          }
        });
    info.setText(
        cameraMode
            ? "Camera armed. Only individually allowed contacts can view. Keep this screen open."
            : "Calling / waiting for connection...");
    root.addView(info);
    surface = new TextureView(this);
    surface.setOpaque(false);
    surface.setSurfaceTextureListener(this);
    frame = new FrameLayout(this);
    frame.setBackgroundColor(android.graphics.Color.BLACK);
    frame.addView(surface, new FrameLayout.LayoutParams(-1, -1));
    preview = new TextureView(this);
    preview.setOpaque(false);
    frame.addView(preview, new FrameLayout.LayoutParams(dp(120), dp(160), Gravity.TOP | Gravity.LEFT));
    preview.setAlpha(.01f);
    View.OnTouchListener videoTouch = new View.OnTouchListener() {
      public boolean onTouch(View v, MotionEvent e) { return touchVideo(v, e); }
    };
    surface.setOnTouchListener(videoTouch);
    preview.setOnTouchListener(videoTouch);

    root.addView(frame, new LinearLayout.LayoutParams(-1, 0, 1));
    LinearLayout buttons = new LinearLayout(this);
    buttons.setBackgroundColor(0xff172a2b);
    root.addView(buttons, new LinearLayout.LayoutParams(-1, dp(56)));
    muteButton =
        add(
            buttons,
            "mic",
            "Mute microphone",
            new View.OnClickListener() {
              public void onClick(View v) {
                if (engine != null) {
                  engine.mute();
                  updateControls();
                }
              }
            });
    speakerButton =
        add(
            buttons,
            "speaker",
            "Speaker",
            new View.OnClickListener() {
              public void onClick(View v) {
                if (engine != null) {
                  engine.speaker();
                  updateControls();
                }
              }
            });
    flipButton =
        add(
            buttons,
            "flip",
            "Switch camera",
            new View.OnClickListener() {
              public void onClick(View v) {
                if (engine != null) engine.switchCamera();
              }
            });
    torchButton =
        add(
            buttons,
            "torch",
            "Torch",
            new View.OnClickListener() {
              public void onClick(View v) {
                if (engine != null) engine.torch();
              }
            });
    ImageButton end =
        add(
            buttons,
            "end",
            cameraMode ? "Disarm camera" : "End call",
            new View.OnClickListener() {
              public void onClick(View v) {
                finish();
              }
            });
    end.setBackgroundColor(0xffbc303b);
    setContentView(root);
    updateControls();
  }

  private void layoutVideo() {
    if (!videoCall || frame.getWidth() == 0 || frame.getHeight() == 0) return;
    int w = Math.min(dp(120), frame.getWidth()), h = Math.min(dp(160), frame.getHeight());
    if (smallLeft < 0) { smallLeft = Math.max(0, frame.getWidth() - w - dp(12)); smallTop = dp(12); }
    smallLeft = Math.max(0, Math.min(smallLeft, frame.getWidth() - w));
    smallTop = Math.max(0, Math.min(smallTop, frame.getHeight() - h));
    TextureView big = swapped ? preview : surface;
    TextureView small = swapped ? surface : preview;
    big.setLayoutParams(new FrameLayout.LayoutParams(-1, -1));
    FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w, h, Gravity.TOP | Gravity.LEFT);
    lp.leftMargin = smallLeft; lp.topMargin = smallTop;
    small.setLayoutParams(lp);
    big.setAlpha(1f); small.setAlpha(smallVisible ? 1f : 0f);
    big.bringToFront(); small.bringToFront();
    transformVideo();
  }

  private boolean touchVideo(View v, MotionEvent e) {
    if (!videoCall) return false;
    boolean small = v == (swapped ? surface : preview) && smallVisible;
    switch (e.getActionMasked()) {
      case MotionEvent.ACTION_DOWN:
        if (!small && v != (swapped ? preview : surface)) return false;
        touchX = e.getRawX(); touchY = e.getRawY(); startLeft = smallLeft; startTop = smallTop; moved = false;
        return true;
      case MotionEvent.ACTION_MOVE:
        if (small) {
          float dx = e.getRawX() - touchX, dy = e.getRawY() - touchY;
          if (Math.abs(dx) > dp(5) || Math.abs(dy) > dp(5)) moved = true;
          if (moved) { smallLeft = (int)(startLeft + dx); smallTop = (int)(startTop + dy); layoutVideo(); }
        }
        return true;
      case MotionEvent.ACTION_UP:
        if (!moved) {
          if (small) {
            long now = android.os.SystemClock.uptimeMillis();
            if (now - lastSmallTap < ViewConfiguration.getDoubleTapTimeout()) {
              swapped = !swapped; lastSmallTap = 0; layoutVideo();
            } else lastSmallTap = now;
          } else { smallVisible = !smallVisible; layoutVideo(); }
        }
        return true;
      case MotionEvent.ACTION_CANCEL: return true;
    }
    return true;
  }

  private int dp(int n) {
    return (int) (n * getResources().getDisplayMetrics().density + .5f);
  }

  private ImageButton add(
      LinearLayout row, String kind, final String label, View.OnClickListener click) {
    ImageButton b = new ImageButton(this);
    b.setScaleType(ImageView.ScaleType.FIT_CENTER);
    b.setImageDrawable(new Icons(kind, android.graphics.Color.WHITE));
    b.setBackgroundColor(android.graphics.Color.TRANSPARENT);
    b.setPadding(dp(12), dp(12), dp(12), dp(12));
    b.setContentDescription(label);
    row.addView(b, new LinearLayout.LayoutParams(0, -1, 1));
    b.setOnClickListener(click);
    b.setOnLongClickListener(
        new View.OnLongClickListener() {
          public boolean onLongClick(View v) {
            Toast.makeText(MediaActivity.this, label, Toast.LENGTH_SHORT).show();
            return true;
          }
        });
    return b;
  }

  private void updateControls() {
    boolean audio = engine != null && engine.hasAudio(),
        camera = engine != null && engine.hasCamera();
    muteButton.setEnabled(audio);
    speakerButton.setEnabled(audio);
    flipButton.setEnabled(camera);
    torchButton.setEnabled(camera);
    muteButton.setAlpha(audio ? 1f : .3f);
    speakerButton.setAlpha(audio ? 1f : .3f);
    flipButton.setAlpha(camera ? 1f : .3f);
    torchButton.setAlpha(camera ? 1f : .3f);
    boolean muted = audio && engine.isMuted();
    muteButton.setImageDrawable(new Icons(muted ? "muted" : "mic", android.graphics.Color.WHITE));
    muteButton.setContentDescription(muted ? "Unmute microphone" : "Mute microphone");
    muteButton.setBackgroundColor(muted ? 0xff136f69 : android.graphics.Color.TRANSPARENT);
    speakerButton.setBackgroundColor(
        audio && engine.isSpeakerOn() ? 0xff136f69 : android.graphics.Color.TRANSPARENT);
    torchButton.setBackgroundColor(
        camera && engine.isTorchOn() ? 0xff136f69 : android.graphics.Color.TRANSPARENT);
  }

  protected void onResume() {
    super.onResume();
    active = true;
    LinkService s = LinkService.current;
    if (s == null || !s.ready) {
      finish();
      return;
    }
    if (cameraMode) s.cameraArmed = true;
    handler.post(poll);
  }

  protected void onPause() {
    active = false;
    handler.removeCallbacks(poll);
    if (engine != null) {
      engine.stop();
      engine = null;
    }
    LinkService s = LinkService.current;
    if (s != null) {
      if (cameraMode) s.cameraArmed = false;
      s.hangup();
    }
    super.onPause();
    if (!isFinishing()) finish();
  }

  public void onSurfaceTextureAvailable(android.graphics.SurfaceTexture texture, int w, int h) {
    output = new Surface(texture);
    surfaceReady = true;
    transformVideo();
    DebugLog.event(
        "VIDEO_VIEW", "TextureView rotation=90cw hardware=" + surface.isHardwareAccelerated());
  }

  public void onSurfaceTextureSizeChanged(android.graphics.SurfaceTexture texture, int w, int h) {
    transformVideo();
  }

  public void onSurfaceTextureUpdated(android.graphics.SurfaceTexture texture) {}

  public boolean onSurfaceTextureDestroyed(android.graphics.SurfaceTexture texture) {
    surfaceReady = false;
    if (engine != null) {
      engine.stop();
      engine = null;
    }
    if (output != null) {
      output.release();
      output = null;
    }
    return true;
  }

  private void transformVideo() {
    if (surface.getWidth() <= 0 || surface.getHeight() <= 0) return;
    android.graphics.Matrix matrix = new android.graphics.Matrix();
    matrix.setValues(
        VideoFit.clockwise(surface.getWidth(), surface.getHeight(), videoWidth, videoHeight));
    surface.setTransform(matrix);
  }

  private final Runnable poll =
      new Runnable() {
        public void run() {
          if (!active) return;
          final LinkService s = LinkService.current;
          if (s == null) {
            finish();
            return;
          }
          final LinkService.Call c = s.call;
          if (c == null) {
            if (engine != null) {
              engine.stop();
              engine = null;
              attached = null;
              answered = false;
              videoCall = false; preview.setAlpha(0f);
            }
            if (cameraMode) info.setText("Camera armed · waiting for an allowed viewer");
            else {
              if (s.lastCallFailure.length() > 0) {
                info.setText(s.lastCallFailure);
                if (!failureShown) {
                  failureShown = true;
                  new android.app.AlertDialog.Builder(MediaActivity.this)
                      .setTitle("Camera / call unavailable")
                      .setMessage(s.lastCallFailure)
                      .setPositiveButton(
                          "OK",
                          new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface d, int n) {
                              finish();
                            }
                          })
                      .setOnCancelListener(
                          new DialogInterface.OnCancelListener() {
                            public void onCancel(DialogInterface d) {
                              finish();
                            }
                          })
                      .show();
                }
                return;
              }
              info.setText("Call ended");
              handler.postDelayed(
                  new Runnable() {
                    public void run() {
                      finish();
                    }
                  },
                  1200);
              return;
            }
          } else if (surfaceReady && (!"video".equals(c.kind) || preview.isAvailable())) {
            if ("video".equals(c.kind) && !videoCall) {
              videoCall = true;
              frame.post(new Runnable() { public void run() { layoutVideo(); } });
            }
            if (!c.outgoing && !c.accepted && !answered) {
              answered = true;
              s.answer(c);
            }
            if (c.socket != null && engine == null) {
              try {
                attached = c;
                boolean view = "view".equals(c.kind), video = "video".equals(c.kind);
                engine =
                    new MediaEngine(
                        MediaActivity.this,
                        c.socket,
                        output,
                        video ? preview.getSurfaceTexture() : null,
                        video || (view && !c.outgoing),
                        video || (view && c.outgoing),
                        !view,
                        new MediaEngine.Listener() {
                          public void videoSize(final int w, final int h) {
                            runOnUiThread(
                                new Runnable() {
                                  public void run() {
                                    if (!active) return;
                                    videoWidth = w;
                                    videoHeight = h;
                                    transformVideo();
                                  }
                                });
                          }

                          public void status(final String text) {
                            runOnUiThread(
                                new Runnable() {
                                  public void run() {
                                    info.setText(text);
                                  }
                                });
                          }

                          public void ended() {
                            runOnUiThread(
                                new Runnable() {
                                  public void run() {
                                    if (s.call == c) s.hangup();
                                  }
                                });
                          }
                        });
              } catch (Exception e) {
                DebugLog.error("MEDIA_UI", e);
                s.hangup();
                info.setText("Media initialization failed");
              }
            }
          }
          updateControls();
          handler.postDelayed(this, 300);
        }
      };
}
