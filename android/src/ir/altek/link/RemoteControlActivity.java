package ir.altek.link;

import android.app.Activity;
import android.content.*;
import android.graphics.*;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.*;
import android.view.*;
import android.widget.*;

/**
 * UI for the "screen" call kind.
 *
 * <p>The side that dialed (Call.outgoing) is the controller: this activity decodes and shows the
 * remote screen and sends taps/swipes. It owns that session itself and ends it on leaving, same as
 * an audio/video call - there is no reason for the controller to need this open in the background.
 *
 * <p>The side that answered shares its screen. That session is owned by {@link LinkService}, not by
 * this activity (see {@link LinkService#beginScreenShare}): leaving this screen - pressing Home, a
 * remote tap opening another app, anything - must not end it, since the whole point of remote
 * control is to let someone else drive the phone while its owner keeps using it. This activity just
 * kicks the session off (answering, and requesting the MediaProjection permission, both of which
 * need an activity) and otherwise shows a status line; a persistent notification (with its own Stop
 * action) is how the person gets back to it or ends it.
 */
public final class RemoteControlActivity extends Activity implements TextureView.SurfaceTextureListener {
  private TextureView surface;
  private Surface output;
  private ImageView image;
  private FrameLayout stage;
  private TextView info;
  private TextView controlHint;
  private final Handler handler = new Handler();

  // Viewer (controller) side only.
  private ScreenEngine engine;
  private ScreenClipboard clipboard;
  private int clipboardMode;
  // Format, surface and frame handoff are coordinated across the reader and UI threads.
  private final Object screenLock = new Object();
  private final ScreenStartupFrames startupFrames = new ScreenStartupFrames();
  private volatile VideoDisplay display;
  private volatile boolean h264Mode;
  private boolean surfaceReady, haveFormat, decoderReady;
  private android.graphics.SurfaceTexture surfaceTex;
  private int pendingW, pendingH, formatGeneration;
  private byte[] pendingConfig;
  private int lastW = 0, lastH = 0; // Last decoded frame size for re-layout on rotation
  private boolean fitMode = true; // true=fit (contain, letterbox), false=fill (cover, crop)
  private float downX, downY;
  private long downTime;
  private boolean dragging;

  private boolean zoomMode; // ON: local zoom/pan; only tap goes to remote
  private float zoom = 1f, panX = 0f, panY = 0f;
  private float panX0, panY0, downRawX, downRawY;
  private long lastTapTime;
  private float lastTapRawX, lastTapRawY;
  private boolean pinching;
  private float startDist, startZoom;
  private ImageButton zoomBtn;
  private final RemoteTap remoteTap = new RemoteTap();
  private boolean remotePinchActive;
  private boolean remotePinchHappened;
  private float remotePinchPrevDist;
  private long remotePinchLastSend;
  private boolean mouseDownSent;
  private int downPid; // pointer id that initiated the down/up mouse stream
  private boolean longPressFired;
  private boolean leftDownPending; // finger is down but left-down not sent yet (decide tap vs drag vs long-press first)
  private final Handler lpHandler = new Handler();
  private final Runnable longPressRunnable =
      new Runnable() {
        public void run() {
          if (engine == null || attached == null || !attached.outgoing) return;
          if (dragging || remotePinchActive || pinching) return;
          remoteTap.reset();
          if (zoomMode) {
            float[] pt = stageToRemote(downRawX, downRawY);
            if (pt != null) {
              engine.sendRightDown(pt[0], pt[1]);
              engine.sendRightUp(pt[0], pt[1]);
              longPressFired = true;
              DebugLog.event("SEND_RIGHT_CLICK", "zoom pt=" + pt[0] + "," + pt[1]);
            }
          } else if (leftDownPending || mouseDownSent) {
            // The gesture turned out to be a long-press: never let a left click happen.
            if (mouseDownSent) {
              engine.sendMouseUp(downX, downY);
              mouseDownSent = false;
            }
            leftDownPending = false;
            engine.sendRightDown(downX, downY);
            engine.sendRightUp(downX, downY);
            longPressFired = true;
            DebugLog.event("SEND_RIGHT_CLICK", "nx=" + downX + " ny=" + downY);
          }
        }
      };

  private LinkService.Call attached;
  private boolean answered, active, failureShown;
  private boolean captureRequested; // sharer side: avoids asking for MediaProjection twice
  private boolean navShown; // controller side: shows the Back/Home/Recents buttons once, not every poll
  private ImageButton backBtn, homeBtn, recentsBtn, fitBtn;
  private ImageButton keyboardBtn;
  private RemoteKeyboard keyboard;
  private boolean keyboardShown;
  private LinearLayout root; // inner layout: stage + buttons, orientation-driven
  private LinearLayout buttons;
  private static final int REQUEST_PROJECTION = 4821;

  public void onCreate(Bundle b) {
    super.onCreate(b);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    applyFullscreen();
    // Device rotation is disabled: only the on-screen rotate button changes orientation.
    setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);

    // Outer frame holds the (mainly text) info/controlHint overlay at top.
    FrameLayout outer = new FrameLayout(this);
    outer.setBackgroundColor(Color.BLACK);

    root = new LinearLayout(this);
    root.setOrientation(LinearLayout.VERTICAL);
    root.setBackgroundColor(Color.BLACK);
    outer.addView(root, new FrameLayout.LayoutParams(-1, -1));

    LinearLayout head = new LinearLayout(this);
    head.setOrientation(LinearLayout.VERTICAL);
    info = new TextView(this);
    info.setTextSize(10);
    info.setTextColor(0xffbbbbbb);
    info.setMaxLines(1);
    info.setEllipsize(android.text.TextUtils.TruncateAt.END);
    info.setPadding(dp(8), dp(3), dp(8), dp(3));
    info.setText("Connecting...");
    head.addView(info);

    controlHint = new TextView(this);
    controlHint.setTextSize(11);
    controlHint.setTextColor(0xff7fd0ff);
    controlHint.setPadding(dp(8), dp(1), dp(8), dp(5));
    controlHint.setVisibility(View.GONE);
    controlHint.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            ControlAccessibilityService.openSettings(RemoteControlActivity.this);
          }
        });
    head.addView(controlHint);
    outer.addView(head, new FrameLayout.LayoutParams(-1, -2, Gravity.TOP));

    stage =
        new FrameLayout(this) {
          @Override
          public boolean onInterceptTouchEvent(MotionEvent ev) {
            // In zoom mode, the stage itself consumes the touch from the start so the child
            // view (surface/image) never sees it - it would consume the event by itself
            // and swallow the pinch/pan before this handler could act on it.
            if (zoomMode) return true;
            return super.onInterceptTouchEvent(ev);
          }
        };
    stage.setBackgroundColor(Color.BLACK);
    stage.setClickable(true);
    surface = new TextureView(this);
    surface.setSurfaceTextureListener(this);
    surface.setVisibility(View.GONE);
    image = new ImageView(this);
    image.setScaleType(ImageView.ScaleType.FIT_XY);
    image.setVisibility(View.GONE);
    stage.addView(surface, new FrameLayout.LayoutParams(1, 1, Gravity.CENTER));
    stage.addView(image, new FrameLayout.LayoutParams(1, 1, Gravity.CENTER));
    View.OnTouchListener touch =
        new View.OnTouchListener() {
          public boolean onTouch(View v, MotionEvent e) {
            // In zoom mode the stage handles everything (see stageTouch below).
            if (zoomMode) return false;
            return handleTouch(v, e);
          }
        };
    surface.setOnTouchListener(touch);
    image.setOnTouchListener(touch);
    stage.setOnTouchListener(
        new View.OnTouchListener() {
          public boolean onTouch(View v, MotionEvent e) {
            if (!zoomMode) return false;
            return handleTouch(v, e);
          }
        });
    root.addView(stage, new LinearLayout.LayoutParams(-1, 0, 1));

    keyboard = new RemoteKeyboard(this, new RemoteKeyboard.Target() {
      public void key(int vk, boolean down) {
        if (engine != null) engine.sendKeyCode(vk, down);
      }
      public boolean acceptsText() { return engine != null && engine.peerAcceptsText(); }
      public void text(String value) { if (engine != null) engine.sendText(value); }
      public void unsupported() {
        Toast.makeText(RemoteControlActivity.this,
            "This keyboard input is not supported by the current Windows version", Toast.LENGTH_SHORT).show();
      }
      public void hidden() { hideKeyboard(); }
    });
    FrameLayout.LayoutParams keyboardParams = new FrameLayout.LayoutParams(1, 1, Gravity.TOP | Gravity.LEFT);
    outer.addView(keyboard, keyboardParams);

    buttons = new LinearLayout(this);
    buttons.setBackgroundColor(0xff172a2b);
    // Back/Home/Recents on the remote device - only shown once this is known to be the
    // controller (see poll()), since the shared side has nothing to send these to.
    backBtn = navButton("back", 1);
    homeBtn = navButton("home", 2);
    recentsBtn = navButton("recents", 3);
    fitBtn = new ImageButton(this);
    fitBtn.setScaleType(ImageView.ScaleType.FIT_CENTER);
    fitBtn.setImageDrawable(new Icons("fit", Color.WHITE));
    fitBtn.setBackgroundColor(0xff1a3a3d);
    fitBtn.setPadding(dp(12), dp(12), dp(12), dp(12));
    fitBtn.setContentDescription("Toggle fit/fill");
    fitBtn.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            fitMode = !fitMode;
            // Re-layout the view with the current mode and last decoded size
            if (lastW > 0 && lastH > 0) {
              View target = h264Mode ? (View) surface : (View) image;
              fitWithMode(target, lastW, lastH);
            }
          }
        });
    ImageButton zoom = new ImageButton(this);
    zoom.setScaleType(ImageView.ScaleType.FIT_CENTER);
    zoom.setImageDrawable(new Icons("zoom", Color.WHITE));
    zoom.setBackgroundColor(0xff1a3a3d);
    zoom.setPadding(dp(12), dp(12), dp(12), dp(12));
    zoom.setContentDescription("Toggle zoom/pan mode");
    zoom.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            toggleZoom();
          }
        });
    zoomBtn = zoom;
    ImageButton rotate = new ImageButton(this);
    rotate.setScaleType(ImageView.ScaleType.FIT_CENTER);
    rotate.setImageDrawable(new Icons("rotate", Color.WHITE));
    rotate.setBackgroundColor(0xff1a3a3d);
    rotate.setPadding(dp(12), dp(12), dp(12), dp(12));
    rotate.setContentDescription("Toggle portrait/landscape");
    rotate.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            boolean land =
                getResources().getConfiguration().orientation
                    == android.content.res.Configuration.ORIENTATION_LANDSCAPE;
            setRequestedOrientation(
                land
                    ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                    : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
          }
        });
    ImageButton end = new ImageButton(this);
    end.setScaleType(ImageView.ScaleType.FIT_CENTER);
    end.setImageDrawable(new Icons("end", Color.WHITE));
    end.setBackgroundColor(0xffbc303b);
    end.setPadding(dp(12), dp(12), dp(12), dp(12));
    end.setContentDescription("Stop remote control");
    end.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            LinkService s = LinkService.current;
            if (s != null) s.hangup();
            finish();
          }
        });
    Button clip = new Button(this);
    clip.setText("Clip");
    clip.setContentDescription("Clipboard sharing");
    clip.setOnClickListener(new View.OnClickListener() { public void onClick(View v) {
      if (engine == null) return;
      final String[] labels = {"Disabled", "Controller to remote", "Remote to controller", "Both directions"};
      new android.app.AlertDialog.Builder(RemoteControlActivity.this).setTitle("Clipboard sharing")
          .setSingleChoiceItems(labels, clipboardMode, new DialogInterface.OnClickListener() {
            public void onClick(DialogInterface dialog, int which) {
              clipboardMode = which;
              engine.sendClipboardMode(which);
              clipboard.setMode(which);
              dialog.dismiss();
            }
          }).show();
    }});
    buttons.addView(clip);
    keyboardBtn = new ImageButton(this);
    keyboardBtn.setScaleType(ImageView.ScaleType.FIT_CENTER);
    keyboardBtn.setImageDrawable(new Icons("keyboard", Color.WHITE));
    keyboardBtn.setBackgroundColor(0xff1a3a3d);
    keyboardBtn.setPadding(dp(12), dp(12), dp(12), dp(12));
    keyboardBtn.setContentDescription("Show or hide keyboard");
    keyboardBtn.setVisibility(View.GONE);
    keyboardBtn.setOnClickListener(new View.OnClickListener() {
      public void onClick(View v) {
        if (keyboardShown) hideKeyboard();
        else if (engine != null) {
          keyboardShown = true;
          getWindow().getDecorView().setSystemUiVisibility(0);
          getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
          keyboard.requestFocus();
          keyboard.post(new Runnable() {
            public void run() {
              if (keyboardShown)
                ((android.view.inputmethod.InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
                    .showSoftInput(keyboard, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
            }
          });
        }
      }
    });
    buttons.addView(keyboardBtn);
    buttons.addView(backBtn);
    buttons.addView(homeBtn);
    buttons.addView(recentsBtn);
    buttons.addView(fitBtn);
    buttons.addView(zoom);
    buttons.addView(rotate);
    buttons.addView(end);
    root.addView(buttons, new LinearLayout.LayoutParams(-1, dp(56)));

    setContentView(outer);
    applyButtonsOrientation();
  }

  /** Portrait: stage above a horizontal button bar. Landscape: stage left of a vertical bar.
   *  The bar is a real layout child (not transparent overlay), but in both orientations the
   *  desktop video still fills the remaining area (it is width-limited in portrait,
   *  height-limited in landscape), so the bar costs little visible space. */
  private void applyButtonsOrientation() {
    boolean land =
        getResources().getConfiguration().orientation
            == android.content.res.Configuration.ORIENTATION_LANDSCAPE;
    root.setOrientation(land ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
    LinearLayout.LayoutParams slp = (LinearLayout.LayoutParams) stage.getLayoutParams();
    if (land) {
      slp.width = 0;
      slp.height = LinearLayout.LayoutParams.MATCH_PARENT;
      slp.weight = 1;
    } else {
      slp.width = LinearLayout.LayoutParams.MATCH_PARENT;
      slp.height = 0;
      slp.weight = 1;
    }
    stage.setLayoutParams(slp);
    LinearLayout.LayoutParams blp = (LinearLayout.LayoutParams) buttons.getLayoutParams();
    if (land) {
      blp.width = dp(56);
      blp.height = LinearLayout.LayoutParams.MATCH_PARENT;
      blp.weight = 0;
    } else {
      blp.width = LinearLayout.LayoutParams.MATCH_PARENT;
      blp.height = dp(56);
      blp.weight = 0;
    }
    buttons.setLayoutParams(blp);
    buttons.setOrientation(land ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
    for (int i = 0; i < buttons.getChildCount(); i++) {
      View v = buttons.getChildAt(i);
      LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) v.getLayoutParams();
      if (land) {
        lp.width = LinearLayout.LayoutParams.MATCH_PARENT;
        lp.height = 0;
        lp.weight = 1;
      } else {
        lp.width = 0;
        lp.height = LinearLayout.LayoutParams.MATCH_PARENT;
        lp.weight = 1;
      }
      v.setLayoutParams(lp);
    }
  }

  private int dp(int n) {
    return (int) (n * getResources().getDisplayMetrics().density + .5f);
  }

  private void applyFullscreen() {
    getWindow()
        .getDecorView()
        .setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
  }

  private void hideKeyboard() {
    keyboardShown = false;
    ((android.view.inputmethod.InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
        .hideSoftInputFromWindow(keyboard.getWindowToken(), 0);
    keyboard.clearFocus();
    applyFullscreen();
  }

  /** A Back/Home/Recents button sent to the remote device - not local navigation. */
  private ImageButton navButton(String icon, final int globalAction) {
    ImageButton b = new ImageButton(this);
    b.setScaleType(ImageView.ScaleType.FIT_CENTER);
    b.setImageDrawable(new Icons(icon, Color.WHITE));
    b.setBackgroundColor(0xff23383a);
    b.setPadding(dp(14), dp(14), dp(14), dp(14));
    b.setVisibility(View.GONE);
    b.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            if (engine != null) engine.sendKey(globalAction);
          }
        });
    return b;
  }

  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    if (clipboard != null) clipboard.focus(hasFocus);
    LinkService service = LinkService.current;
    if (service != null && service.screenClipboard != null) service.screenClipboard.focus(hasFocus);
  }

  protected void onResume() {
    super.onResume();
    active = true;
    applyFullscreen();
    LinkService s = LinkService.current;
    if (s == null || !s.ready) {
      finish();
      return;
    }
    handler.post(poll);
  }

  protected void onPause() {
    active = false;
    handler.removeCallbacks(poll);
    if (attached != null && !attached.outgoing) {
      // Sharing side: the session lives in LinkService now, not here. Leaving this screen (Home,
      // a remote tap that opens another app, ...) must not end it - that is the point of this
      // feature. Just stop updating this UI; the notification is how the person gets back to it.
      super.onPause();
      return;
    }
    teardown();
    LinkService s = LinkService.current;
    if (s != null) s.hangup();
    super.onPause();
    if (!isFinishing()) finish();
  }

  /** Viewer-side cleanup only; the sharing side has nothing local to tear down any more. */
  private void teardown() {
    if (keyboard != null) keyboard.close();
    if (clipboard != null) { clipboard.close(); clipboard = null; }
    remoteTap.reset();
    if (engine != null) {
      engine.stop();
      engine = null;
    }
    synchronized (screenLock) {
      decoderReady = false;
      haveFormat = false;
      startupFrames.clear();
      if (display != null) {
        display.stop();
        display = null;
      }
    }
  }

  protected void onActivityResult(int requestCode, int resultCode, Intent data) {
    super.onActivityResult(requestCode, resultCode, data);
    if (requestCode != REQUEST_PROJECTION) return;
    // Not gated on `active`: onActivityResult can be delivered before onResume() runs (the exact
    // order is not guaranteed the same way on every OEM build), and `active` is already false at
    // that point because onPause() sets it unconditionally before its sharing-side early return.
    // isFinishing() is the correct signal for "this activity is actually going away".
    LinkService s = LinkService.current;
    if (s == null || attached == null || isFinishing()) return;
    MediaProjection projection = null;
    boolean approved = resultCode == Activity.RESULT_OK && data != null;
    String projectionFailure = null;
    if (approved) {
      try {
        // Android Q+ requires getMediaProjection() to be called while the app has
        // a running foreground service whose type is mediaProjection.
        if (Build.VERSION.SDK_INT >= 29) {
          s.prepareMediaProjectionForeground();
        }

        MediaProjectionManager mgr =
            (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        projection = mgr.getMediaProjection(resultCode, data);
      } catch (Exception e) {
        // Seen on at least one Samsung device as a SecurityException wrapped in a RemoteException:
        // the system approved the request (resultCode/data looked fine) but creating the
        // MediaProjection from it still failed. Whatever the exact cause, this must not crash the
        // app - fall back to root exactly like an explicit decline.
        DebugLog.error("SCREEN_PROJECTION", e);
        projectionFailure = e.getClass().getSimpleName();
        approved = false;
      }
    }
    if (approved && projection != null) {
      s.beginScreenShare(attached, projection);
    } else if (Build.VERSION.SDK_INT >= 19) {
      info.setText(
          (projectionFailure != null
                  ? "Screen-share permission failed (" + projectionFailure + ")"
                  : "Screen-share permission declined")
              + "; trying root instead...");
      s.beginScreenShare(attached, null);
    } else {
      info.setText("Screen sharing permission was declined");
      s.hangup();
    }
  }

  public void onSurfaceTextureAvailable(android.graphics.SurfaceTexture texture, int w, int h) {
    synchronized (screenLock) {
      output = new Surface(texture);
      surfaceReady = true;
      surfaceTex = texture;
      configureScreenDecoder();
    }
  }

  // Called on the UI thread with screenLock held. Queue format before any saved frames.
  private void configureScreenDecoder() {
    if (!haveFormat || !h264Mode || !surfaceReady || output == null || decoderReady) return;
    try { surfaceTex.setDefaultBufferSize(pendingW, pendingH); } catch (Throwable ignored) {}
    if (display == null) {
      display = new VideoDisplay(output, new VideoDisplay.Listener() {
        public void size(int w, int h) {}
      });
    }
    display.format(pendingW, pendingH, pendingConfig);
    ScreenStartupFrames.Frame frame;
    while ((frame = startupFrames.take()) != null)
      display.frame(frame.pts, frame.key ? 1 : 0, frame.bytes);
    decoderReady = true;
  }

  public void onSurfaceTextureSizeChanged(android.graphics.SurfaceTexture texture, int w, int h) {
    // Surface size changed (device rotation, layout); re-fit for current mode
    if (lastW > 0 && lastH > 0) {
      View target = h264Mode ? (View) surface : (View) image;
      fitWithMode(target, lastW, lastH);
    }
  }

  @Override
  public void onConfigurationChanged(android.content.res.Configuration newConfig) {
    super.onConfigurationChanged(newConfig);
    applyButtonsOrientation();
    if (lastW > 0 && lastH > 0) {
      View target = h264Mode ? (View) surface : (View) image;
      fitWithMode(target, lastW, lastH);
    }
  }

  public void onSurfaceTextureUpdated(android.graphics.SurfaceTexture texture) {}

  public boolean onSurfaceTextureDestroyed(android.graphics.SurfaceTexture texture) {
    synchronized (screenLock) {
      surfaceReady = false;
      surfaceTex = null;
      decoderReady = false;
      startupFrames.clear();
      if (display != null) {
        display.stop();
        display = null;
      }
      if (output != null) {
        output.release();
        output = null;
      }
    }
    return true;
  }

  private static float clamp01(float v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
  }

  /** Only the controller (the side that dialed) sends gestures; the shared side ignores touches. */
  private boolean handleTouch(View v, MotionEvent e) {
    if (engine == null || attached == null || !attached.outgoing) return false;
    if (zoomMode) return handleZoomTouch(e);
    int w = v.getWidth(), h = v.getHeight();
    if (w <= 0 || h <= 0) return false;
    float nx = clamp01(e.getX() / w), ny = clamp01(e.getY() / h);
    final boolean twoFingers = e.getPointerCount() >= 2;
    switch (e.getActionMasked()) {
      case MotionEvent.ACTION_DOWN:
        downX = nx;
        downY = ny;
        downTime = SystemClock.uptimeMillis();
        dragging = false;
        remotePinchActive = false;
        remotePinchHappened = false;
        remotePinchPrevDist = 0;
        mouseDownSent = false;
        leftDownPending = true;
        DebugLog.event("TOUCH_DOWN", "nx=" + nx + " ny=" + ny);
        longPressFired = false;
        lpHandler.removeCallbacks(longPressRunnable);
        lpHandler.postDelayed(longPressRunnable, 500);
        return true;
      case MotionEvent.ACTION_POINTER_DOWN:
        if (twoFingers) {
          remoteTap.reset();
          remotePinchActive = true;
          remotePinchHappened = false;
          remotePinchPrevDist = pointerDistance(e);
          // A second finger hands the gesture to pinch: release the held
          // mouse-down so the PC doesn't see a dangling button, and don't
          // emit any further mouse events from this gesture.
          if (mouseDownSent) {
            engine.sendMouseUp(downX, downY);
            DebugLog.event("SEND_MOUSE_UP", "nx=" + downX + " ny=" + downY + " pinch-cancel");
            mouseDownSent = false;
          }
          leftDownPending = false;
          lpHandler.removeCallbacks(longPressRunnable);
        }
        return true;
      case MotionEvent.ACTION_POINTER_UP:
        if (remotePinchActive) {
          remotePinchActive = false;
          dragging = true;
        }
        return true;
      case MotionEvent.ACTION_MOVE:
      if (remotePinchActive && twoFingers) {
          float nd = pointerDistance(e);
          if (remotePinchPrevDist > 0 && nd > 0) {
            float scale = nd / remotePinchPrevDist;
            long nowMs = SystemClock.uptimeMillis();
            // Only emit a control packet when we have moved enough to engage the remote
            // gesture detector; sending one per frame overwhelms dispatchGesture and drops
            // or queues them, which is exactly the "sometimes nothing, sometimes a jump".
            boolean enoughChange = scale > 1.02f || scale < 0.98f;
            boolean enoughTime = (nowMs - remotePinchLastSend) >= 40;
            if (enoughChange && enoughTime) {
              float mx = 0.5f * (e.getX(0) + e.getX(1));
              float my = 0.5f * (e.getY(0) + e.getY(1));
              engine.sendPinch(clamp01(mx / w), clamp01(my / h), scale);
              DebugLog.event("SEND_PINCH", "cx=" + clamp01(mx / w) + " cy=" + clamp01(my / h) + " scale=" + scale);
              remotePinchHappened = true;
              remotePinchPrevDist = nd;
              remotePinchLastSend = nowMs;
            }
          }
          return true;
        }
        if (!dragging) {
          float dx = (nx - downX) * w, dy = (ny - downY) * h;
          if (Math.abs(dx) > dp(8) || Math.abs(dy) > dp(8)) {
            dragging = true;
            remoteTap.reset();
            lpHandler.removeCallbacks(longPressRunnable);
            if (leftDownPending) {
              // Drag confirmed: start the left-button session at the down point.
              engine.sendMouseDown(downX, downY);
              DebugLog.event("SEND_MOUSE_DOWN", "nx=" + downX + " ny=" + downY + " drag-start");
              mouseDownSent = true;
              leftDownPending = false;
            }
          }
        }
        if (mouseDownSent) {
          // Stream the drag live so the PC shows selection/icon-drag while the
          // finger is still moving. The buttons stay down the whole time.
          engine.sendMouseMove(nx, ny);
        }
        return true;
      case MotionEvent.ACTION_UP:
        lpHandler.removeCallbacks(longPressRunnable);
        if (remotePinchHappened) {
          remotePinchHappened = false;
          remotePinchActive = false;
          mouseDownSent = false;
          leftDownPending = false;
          return true;
        }
        if (leftDownPending) {
          // Keep two nearby taps at the same remote point despite normal finger jitter.
          float[] point = remoteTap.position(SystemClock.uptimeMillis(), downX, downY, w, h, dp(24));
          engine.sendMouseDown(point[0], point[1]);
          engine.sendMouseUp(point[0], point[1]);
          DebugLog.event("SEND_TAP_CLICK", "nx=" + point[0] + " ny=" + point[1]);
          leftDownPending = false;
          return true;
        }
        if (mouseDownSent) {
          engine.sendMouseUp(nx, ny);
          DebugLog.event("SEND_MOUSE_UP", "nx=" + nx + " ny=" + ny);
          mouseDownSent = false;
        }
        return true;
      case MotionEvent.ACTION_CANCEL:
        lpHandler.removeCallbacks(longPressRunnable);
        remoteTap.reset();
        leftDownPending = false;
        if (mouseDownSent) {
          engine.sendMouseUp(nx, ny);
          DebugLog.event("SEND_MOUSE_UP", "nx=" + nx + " ny=" + ny + " cancel");
          mouseDownSent = false;
        }
        remotePinchActive = false;
        remotePinchHappened = false;
        return true;
    }
    return true;
  }

  /**
   * ON zoom mode: two-finger pinch = zoom (0.25x..10x), one-finger drag = pan (local only,
   * nothing goes to the remote until a tap). A quick tap = single click. Double-tap toggles
   * fit/2x centered on the tap. Zoom/pan persist when the mode is toggled off.
   */
  private boolean handleZoomTouch(MotionEvent e) {
    switch (e.getActionMasked()) {
      case MotionEvent.ACTION_DOWN:
        pinching = false;
        panX0 = panX;
        panY0 = panY;
        downRawX = e.getX();
        downRawY = e.getY();
        downTime = SystemClock.uptimeMillis();
        dragging = false;
        longPressFired = false;
        lpHandler.removeCallbacks(longPressRunnable);
        lpHandler.postDelayed(longPressRunnable, 500);
        return true;
      case MotionEvent.ACTION_POINTER_DOWN:
        lpHandler.removeCallbacks(longPressRunnable);
        if (e.getPointerCount() == 2) {
          pinching = true;
          startDist = pointerDistance(e);
          startZoom = zoom;
        }
        return true;
      case MotionEvent.ACTION_POINTER_UP:
        pinching = false;
        return true;
      case MotionEvent.ACTION_MOVE:
        if (pinching && e.getPointerCount() >= 2) {
          float oldZoom = zoom;
          float newDist = pointerDistance(e);
          if (startDist > 0 && newDist > 0) {
            zoom = clamp(startZoom * newDist / startDist, 0.25f, 10.0f);
            float midX = (e.getX(0) + e.getX(1)) / 2f;
            float midY = (e.getY(0) + e.getY(1)) / 2f;
            float cx = stage.getWidth() / 2f, cy = stage.getHeight() / 2f;
            panX = midX - cx - (midX - cx - panX) * (zoom / oldZoom);
            panY = midY - cy - (midY - cy - panY) * (zoom / oldZoom);
            clampPan();
            applyZoom();
          }
        } else if (!pinching) {
          float dx = e.getX() - downRawX, dy = e.getY() - downRawY;
          if (!dragging && (Math.abs(dx) > dp(8) || Math.abs(dy) > dp(8))) {
            dragging = true;
            lpHandler.removeCallbacks(longPressRunnable);
          }
          if (dragging) {
            panX = panX0 + dx;
            panY = panY0 + dy;
            clampPan();
            applyZoom();
          }
        }
        return true;
      case MotionEvent.ACTION_UP:
        lpHandler.removeCallbacks(longPressRunnable);
        if (dragging || pinching) {
          dragging = false;
          pinching = false;
          return true;
        }
        if (longPressFired) {
          longPressFired = false;
          return true;
        }
        // Quick tap: send click. Also detect double-tap for 1x/4x toggle.
        long now = SystemClock.uptimeMillis();
        float[] pt = stageToRemote(e.getX(), e.getY());
        if (pt != null) {
          if (now - lastTapTime < 300
              && Math.hypot(e.getX() - lastTapRawX, e.getY() - lastTapRawY) < dp(24)) {
            // double-tap: toggle zoom around the tap point (fit <-> 2x)
            zoom = zoom > 1.5f ? 1f : 2.0f;
            View t = zoomTarget();
            float tw = t.getWidth(), th = t.getHeight();
            if (tw > 0 && th > 0) {
              panX = -(pt[0] - 0.5f) * zoom * tw;
              panY = -(pt[1] - 0.5f) * zoom * th;
              clampPan();
              applyZoom();
            }
            lastTapTime = 0;
          } else {
            engine.sendTap(pt[0], pt[1]);
            lastTapTime = now;
            lastTapRawX = e.getX();
            lastTapRawY = e.getY();
          }
        }
        return true;
      case MotionEvent.ACTION_CANCEL:
        lpHandler.removeCallbacks(longPressRunnable);
        dragging = false;
        pinching = false;
        return true;
    }
    return true;
  }

  private View zoomTarget() {
    return h264Mode ? (View) surface : (View) image;
  }

  private void applyZoom() {
    View t = zoomTarget();
    t.setScaleX(zoom);
    t.setScaleY(zoom);
    t.setTranslationX(panX);
    t.setTranslationY(panY);
  }

  private void resetTransforms() {
    surface.setScaleX(1f);
    surface.setScaleY(1f);
    surface.setTranslationX(0f);
    surface.setTranslationY(0f);
    image.setScaleX(1f);
    image.setScaleY(1f);
    image.setTranslationX(0f);
    image.setTranslationY(0f);
  }

  private void clampPan() {
    View t = zoomTarget();
    int sw = stage.getWidth(), sh = stage.getHeight();
    if (sw <= 0 || sh <= 0 || t == null) return;
    float maxX = Math.max(0f, (zoom * t.getWidth() - sw) / 2f);
    float maxY = Math.max(0f, (zoom * t.getHeight() - sh) / 2f);
    if (panX > maxX) panX = maxX;
    if (panX < -maxX) panX = -maxX;
    if (panY > maxY) panY = maxY;
    if (panY < -maxY) panY = -maxY;
  }

  private float[] stageToRemote(float px, float py) {
    View t = zoomTarget();
    float tw = t.getWidth(), th = t.getHeight();
    int sw = stage.getWidth(), sh = stage.getHeight();
    if (tw <= 0 || th <= 0 || sw <= 0 || sh <= 0) return null;
    return new float[] {
      clamp01(0.5f + (px - sw / 2f - panX) / (zoom * tw)),
      clamp01(0.5f + (py - sh / 2f - panY) / (zoom * th))
    };
  }

  private static float clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
  }

  private float pointerDistance(MotionEvent e) {
    if (e.getPointerCount() < 2) return 0;
    float dx = e.getX(0) - e.getX(1), dy = e.getY(0) - e.getY(1);
    return (float) Math.sqrt(dx * dx + dy * dy);
  }

  private void toggleZoom() {
    remoteTap.reset();
    // Only flip the mode: the current zoom/pan must persist, otherwise the user would
    // have to "fall out" of zoom to make touches selectable and lose what they zoomed on.
    zoomMode = !zoomMode;
    if (zoomMode) {
      zoomBtn.setBackgroundColor(0xff2e7d32);
    } else {
      zoomBtn.setBackgroundColor(0xff1a3a3d);
    }
  }

  /**
   * Sizes `v` to the content's aspect ratio, centered in `stage`, so the view's own pixel bounds
   * exactly match the shown image and touches can be normalized with no separate letterbox math.
   */
  private void fitWithMode(View v, int cw, int ch) {
    int sw = stage.getWidth(), sh = stage.getHeight();
    if (sw <= 0 || sh <= 0 || cw <= 0 || ch <= 0) return;
    float scale =
        fitMode
            ? Math.min(sw / (float) cw, sh / (float) ch)
            : Math.max(sw / (float) cw, sh / (float) ch);
    int fw = Math.max(1, Math.round(cw * scale)), fh = Math.max(1, Math.round(ch * scale));
    FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) v.getLayoutParams();
    if (lp.width != fw || lp.height != fh) {
      lp.width = fw;
      lp.height = fh;
      lp.gravity = Gravity.CENTER;
      v.setLayoutParams(lp);
    }
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
              teardown();
              attached = null;
              answered = false;
              captureRequested = false;
            }
            if (s.lastCallFailure.length() > 0) {
              info.setText(s.lastCallFailure);
              if (!failureShown) {
                failureShown = true;
                new android.app.AlertDialog.Builder(RemoteControlActivity.this)
                    .setTitle("Remote control unavailable")
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
            info.setText("Session ended");
            handler.postDelayed(
                new Runnable() {
                  public void run() {
                    finish();
                  }
                },
                1200);
            return;
          }
          if (!c.outgoing && !c.accepted && !answered) {
            answered = true;
            s.answer(c);
          }
          attached = c;
          if (c.outgoing) {
            if (!navShown) {
              navShown = true;
              backBtn.setVisibility(View.VISIBLE);
              homeBtn.setVisibility(View.VISIBLE);
              recentsBtn.setVisibility(View.VISIBLE);
              keyboardBtn.setVisibility(View.VISIBLE);
            }
            if (c.socket != null && engine == null) {
              try {
                startViewer(s, c);
              } catch (Exception e) {
                DebugLog.error("SCREEN_UI", e);
                s.hangup();
                info.setText("Remote control initialization failed");
              }
            }
          } else {
            // Sharing side: LinkService owns the session; just reflect its status here.
            if (c.socket != null && !captureRequested) {
              captureRequested = true;
              beginSharing(s, c);
            }
            if (s.screenEngine != null) {
              if (s.screenClipboard != null) s.screenClipboard.focus(hasWindowFocus());
              info.setText(s.screenStatus.length() > 0 ? s.screenStatus : "Remote control active");
              RemoteControl r = s.screenRemote;
              if (Build.VERSION.SDK_INT >= 24 && r != null && !r.rootFree()) {
                controlHint.setText("Tap here to enable tap/swipe control without root (Accessibility)");
                controlHint.setVisibility(View.VISIBLE);
              } else {
                controlHint.setVisibility(View.GONE);
              }
            }
          }
          handler.postDelayed(this, 300);
        }
      };

  private void beginSharing(LinkService s, LinkService.Call c) {
    info.setText("Remote control active - this screen is being shared");
    if (Build.VERSION.SDK_INT >= 21) {
      // Preferred: no root, just a one-time system prompt. A decline falls back to root
      // automatically in onActivityResult (screenrecord also works at this API level).
      MediaProjectionManager mgr =
          (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
      startActivityForResult(mgr.createScreenCaptureIntent(), REQUEST_PROJECTION);
    } else {
      // API 16-20: MediaProjection does not exist yet; root is the only option.
      s.beginScreenShare(c, null);
    }
  }

  private void startViewer(final LinkService s, final LinkService.Call c) throws Exception {
    info.setText("Connecting to remote screen...");
    engine =
        new ScreenEngine(
            c.socket,
            new ScreenEngine.FrameListener() {
              public void format(final boolean h264, final int w, final int h, final byte[] config) {
                final int generation;
                synchronized (screenLock) {
                  if (haveFormat && h264Mode == h264 && pendingW == w && pendingH == h
                      && java.util.Arrays.equals(pendingConfig, config)) return;
                  haveFormat = true;
                  h264Mode = h264;
                  pendingW = w;
                  pendingH = h;
                  pendingConfig = config;
                  decoderReady = false;
                  startupFrames.clear();
                  generation = ++formatGeneration;
                }
                runOnUiThread(new Runnable() {
                  public void run() {
                    if (!active) return;
                    synchronized (screenLock) {
                      if (generation != formatGeneration) return;
                      surface.setVisibility(h264 ? View.VISIBLE : View.GONE);
                      image.setVisibility(h264 ? View.GONE : View.VISIBLE);
                      fitWithMode(h264 ? (View) surface : (View) image, w, h);
                      lastW = w;
                      lastH = h;
                      if (h264) configureScreenDecoder();
                    }
                  }
                });
              }

              public void frame(final boolean key, final long pts, final byte[] bytes) {
                synchronized (screenLock) {
                  if (!haveFormat) return;
                  if (h264Mode) {
                    if (decoderReady && display != null) display.frame(pts, key ? 1 : 0, bytes);
                    else startupFrames.offer(key, pts, bytes);
                    return;
                  }
                }
                final Bitmap bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
                if (bmp == null) return;
                runOnUiThread(new Runnable() {
                  public void run() {
                    if (active) image.setImageBitmap(bmp);
                    else bmp.recycle();
                  }
                });
              }
            },
            null,
            new ScreenEngine.Listener() {
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
    clipboard = new ScreenClipboard(this, engine, true);
    engine.attachClipboard(clipboard);
    clipboard.focus(hasWindowFocus());
  }
}
