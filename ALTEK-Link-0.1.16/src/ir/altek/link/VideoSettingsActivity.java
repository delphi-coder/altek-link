package ir.altek.link;

import android.app.*;
import android.content.*;
import android.graphics.ImageFormat;
import android.hardware.Camera;
import android.os.*;
import android.view.*;
import android.widget.*;
import java.util.*;

/** Camera1 discovery on a worker. Does not assume front and rear capabilities match. */
public final class VideoSettingsActivity extends Activity {
  private LinearLayout panel;
  private volatile boolean closed;
  private final ArrayList<Caps> cameras = new ArrayList<Caps>();
  private Spinner cameraSelect;

  private static class Caps {
    int id, maxFps = 10;
    String label, formats = "", error = "";
    final ArrayList<String> sizes = new ArrayList<String>(),
        ranges = new ArrayList<String>(),
        focus = new ArrayList<String>();
  }

  public void onCreate(Bundle state) {
    super.onCreate(state);
    panel = new LinearLayout(this);
    panel.setOrientation(1);
    panel.setPadding(16, 8, 16, 8);
    ScrollView scroll = new ScrollView(this);
    scroll.addView(panel);
    setContentView(scroll);
    text("Reading cameras...", 18);
    new Thread(
            new Runnable() {
              public void run() {
                scan();
              }
            },
            "camera-capabilities")
        .start();
  }

  protected void onDestroy() {
    closed = true;
    super.onDestroy();
  }

  private void scan() {
    try {
      LinkService service = LinkService.current;
      if (service != null && service.call != null) throw new Exception("End the call first");
      int count = Camera.getNumberOfCameras();
      for (int id = 0; id < count && !closed; id++) {
        final Caps caps = new Caps();
        caps.id = id;
        Camera camera = null;
        try {
          Camera.CameraInfo info = new Camera.CameraInfo();
          Camera.getCameraInfo(id, info);
          caps.label =
              (info.facing == Camera.CameraInfo.CAMERA_FACING_FRONT ? "Front / سلفی" : "Rear / پشت")
                  + " #"
                  + id;
          camera = Camera.open(id);
          Camera.Parameters p = camera.getParameters();
          ArrayList<Camera.Size> sizes = new ArrayList<Camera.Size>(p.getSupportedPreviewSizes());
          Collections.sort(
              sizes,
              new Comparator<Camera.Size>() {
                public int compare(Camera.Size a, Camera.Size b) {
                  return Long.valueOf((long) a.width * a.height)
                      .compareTo((long) b.width * b.height);
                }
              });
          for (Camera.Size size : sizes)
            if ((size.width & 1) == 0
                && (size.height & 1) == 0
                && size.width <= 4096
                && size.height <= 4096) caps.sizes.add(size.width + "x" + size.height);
          List<int[]> ranges = p.getSupportedPreviewFpsRange();
          int max = 0;
          if (ranges != null)
            for (int[] range : ranges) {
              caps.ranges.add(range[0] + "," + range[1]);
              max = Math.max(max, range[1]);
            }
          if (max == 0) {
            List<Integer> legacy = p.getSupportedPreviewFrameRates();
            if (legacy != null) for (int rate : legacy) max = Math.max(max, rate * 1000);
          }
          if (max > 0) caps.maxFps = Math.max(1, Math.min(120, max / 1000));
          List<String> focus = p.getSupportedFocusModes();
          if (focus != null) caps.focus.addAll(focus);
          List<Integer> formats = p.getSupportedPreviewFormats();
          if (formats != null)
            for (int format : formats)
              caps.formats += (caps.formats.length() == 0 ? "" : ", ") + formatName(format);
          int[] actual = new int[2];
          p.getPreviewFpsRange(actual);
          DebugLog.event(
              "CAMERA_CAPS",
              caps.label
                  + " sizes="
                  + caps.sizes
                  + " ranges="
                  + caps.ranges
                  + " formats="
                  + caps.formats);
        } catch (Exception e) {
          caps.error = "Camera busy / unavailable. Close camera apps and reopen this page.";
          DebugLog.error("CAMERA_CAPS", e);
        } finally {
          if (camera != null) camera.release();
        }
        cameras.add(caps);
      }
      runOnUiThread(
          new Runnable() {
            public void run() {
              if (closed) return;
              panel.removeAllViews();
              if (cameras.isEmpty()) {
                text("No camera found", 18);
                return;
              }
              text("Video settings / تنظیمات ویدیو", 20);
              text(
                  "Separate settings for each camera. Save before switching camera here. Applied on"
                      + " the next call or Flip.",
                  13);
              ArrayList<String> labels = new ArrayList<String>();
              for (Caps c : cameras) labels.add(c.label == null ? "Camera #" + c.id : c.label);
              cameraSelect = spinner(labels, 0);
              cameraSelect.setOnItemSelectedListener(
                  new AdapterView.OnItemSelectedListener() {
                    public void onItemSelected(AdapterView<?> a, View v, int n, long id) {
                      editor(cameras.get(n));
                    }

                    public void onNothingSelected(AdapterView<?> a) {}
                  });
            }
          });
    } catch (Exception e) {
      DebugLog.error("CAMERA_CAPS", e);
      runOnUiThread(
          new Runnable() {
            public void run() {
              if (!closed) {
                panel.removeAllViews();
                text("Cannot read cameras. End any active call and try again.", 16);
              }
            }
          });
    }
  }

  private String formatName(int f) {
    return f == ImageFormat.NV21
        ? "NV21"
        : f == ImageFormat.YV12
            ? "YV12"
            : f == ImageFormat.YUY2 ? "YUY2" : f == ImageFormat.RGB_565 ? "RGB565" : "format " + f;
  }

  private TextView text(String value, int size) {
    TextView t = new TextView(this);
    t.setText(value);
    t.setTextSize(size);
    t.setPadding(4, 8, 4, 4);
    panel.addView(t);
    return t;
  }

  private Spinner spinner(List<String> values, int index) {
    Spinner s = new Spinner(this);
    ArrayAdapter<String> a =
        new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, values);
    a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
    s.setAdapter(a);
    s.setSelection(Math.max(0, index));
    panel.addView(s);
    return s;
  }

  private ArrayList<String> choices(String first, List<String> rest) {
    ArrayList<String> list = new ArrayList<String>();
    list.add(first);
    list.addAll(rest);
    return list;
  }

  private int index(List<String> list, String value) {
    return value.length() == 0 ? 0 : Math.max(0, list.indexOf(value));
  }

  private void editor(final Caps caps) {
    while (panel.getChildCount() > 3) panel.removeViewAt(3);
    if (caps.error.length() > 0) {
      text(caps.error, 16);
      return;
    }
    final VideoOptions old = VideoOptions.load(this, caps.id);
    text("Resolution (camera preview sizes)", 15);
    final ArrayList<String> sizes = choices("Default: nearest 320x240", caps.sizes);
    final Spinner size = spinner(sizes, index(sizes, old.size));
    text("Camera FPS range (driver-supported)", 15);
    final ArrayList<String> ranges = choices("", caps.ranges),
        rangeLabels = new ArrayList<String>();
    rangeLabels.add("Default: keep driver setting");
    for (String r : caps.ranges) {
      String[] split = r.split(",");
      rangeLabels.add(
          (Integer.parseInt(split[0]) / 1000f)
              + " - "
              + (Integer.parseInt(split[1]) / 1000f)
              + " fps");
    }
    final Spinner range = spinner(rangeLabels, index(ranges, old.range));
    text("Send FPS (frame limit, may be below camera FPS)", 15);
    final ArrayList<String> rates = new ArrayList<String>();
    for (int f : new int[] {1, 5, 10, 15, 20, 24, 25, 30, 50, 60, 90, 120})
      if (f <= caps.maxFps) rates.add("" + f);
    if (!rates.contains("10")) rates.add(0, "10");
    final Spinner fps = spinner(rates, Math.max(0, rates.indexOf("" + old.fps)));
    text("Bitrate, kbps (requested H.264 bitrate)", 15);
    final EditText bitrate = new EditText(this);
    bitrate.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
    bitrate.setSingleLine(true);
    bitrate.setText("" + (old.bitrate / 1000));
    panel.addView(bitrate);
    text(
        "32–20000 kbps. Default 250. Actual bitrate depends on the encoder. Camera modes do not"
            + " guarantee encoder support.",
        12);
    text("Key-frame interval, seconds", 15);
    final ArrayList<String> keys = new ArrayList<String>(Arrays.asList("1", "2", "3", "5", "10"));
    final Spinner key = spinner(keys, Math.max(0, keys.indexOf("" + old.keySeconds)));
    text("Focus mode", 15);
    final ArrayList<String> focuses = choices("Default: keep driver setting", caps.focus);
    final Spinner focus = spinner(focuses, index(focuses, old.focus));
    text(
        "Supported raw camera formats: "
            + caps.formats
            + "\n"
            + "This build uses NV21 -> H.264. Other raw formats are listed for diagnostics, not"
            + " selectable. Sizes above4096 or odd dimensions are excluded.",
        12);
    Button save = new Button(this);
    save.setText("Save this camera");
    panel.addView(save);
    save.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            try {
              int rate = Integer.parseInt((String) fps.getSelectedItem()),
                  kbps = Integer.parseInt(bitrate.getText().toString()),
                  seconds = Integer.parseInt((String) key.getSelectedItem());
              if (!VideoOptions.valid(rate, kbps, seconds)) throw new Exception();
              String selectedRange = ranges.get(range.getSelectedItemPosition());
              if (selectedRange.length() > 0
                  && rate * 1000 > Integer.parseInt(selectedRange.split(",")[1])) {
                Toast.makeText(
                        VideoSettingsActivity.this,
                        "Send FPS exceeds selected camera range",
                        Toast.LENGTH_LONG)
                    .show();
                return;
              }
              VideoOptions o = new VideoOptions();
              o.size =
                  size.getSelectedItemPosition() == 0
                      ? ""
                      : sizes.get(size.getSelectedItemPosition());
              o.range = selectedRange;
              o.focus =
                  focus.getSelectedItemPosition() == 0
                      ? ""
                      : focuses.get(focus.getSelectedItemPosition());
              o.fps = rate;
              o.bitrate = kbps * 1000;
              o.keySeconds = seconds;
              o.save(VideoSettingsActivity.this, caps.id);
              DebugLog.event(
                  "VIDEO_PREFS",
                  "camera="
                      + caps.id
                      + " size="
                      + o.size
                      + " fps="
                      + rate
                      + " bitrate="
                      + o.bitrate
                      + " key="
                      + seconds
                      + " range="
                      + o.range
                      + " focus="
                      + o.focus);
              Toast.makeText(
                      VideoSettingsActivity.this, "Saved for " + caps.label, Toast.LENGTH_SHORT)
                  .show();
            } catch (Exception e) {
              Toast.makeText(
                      VideoSettingsActivity.this, "Enter bitrate32–20000 kbps", Toast.LENGTH_LONG)
                  .show();
            }
          }
        });
    Button reset = new Button(this);
    reset.setText("Restore tested defaults for this camera");
    panel.addView(reset);
    reset.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            new VideoOptions().save(VideoSettingsActivity.this, caps.id);
            editor(caps);
          }
        });
    String actual = getSharedPreferences("video_actual", 0).getString("camera" + caps.id, "");
    if (actual.length() > 0) text("Last actual configuration:\n" + actual, 13);
    text(
        "If custom settings fail to start, the app retries this camera using the tested defaults"
            + " and logs VIDEO_FALLBACK. Saved choices remain for editing.",
        12);
  }
}
