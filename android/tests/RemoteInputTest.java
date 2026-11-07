package ir.altek.link;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class RemoteInputTest {
  private static void check(boolean ok, String message) {
    if (!ok) throw new AssertionError(message);
    System.out.println("PASS " + message);
  }
  public static void main(String[] args) throws Exception {
    RemoteTap tap = new RemoteTap();
    float[] first = tap.position(1000, .5f, .5f, 1000, 1000, 24);
    float[] second = tap.position(1350, .515f, .5f, 1000, 1000, 24);
    check(first[0] == second[0] && first[1] == second[1], "Nearby taps share remote coordinates");
    first = tap.position(1500, .2f, .2f, 1000, 1000, 24);
    second = tap.position(1600, .3f, .2f, 1000, 1000, 24);
    check(second[0] == .3f, "Distant taps remain separate");
    second = tap.position(2101, .31f, .2f, 1000, 1000, 24);
    check(second[0] == .31f, "Slow taps remain separate");
    tap.reset();
    second = tap.position(2200, .32f, .2f, 1000, 1000, 24);
    check(second[0] == .32f, "Other gestures reset tap pairing");

    check("a".equals(RemoteKeyText.character(0x41, false, false))
        && "A".equals(RemoteKeyText.character(0x41, true, false))
        && "A".equals(RemoteKeyText.character(0x41, false, true))
        && "a".equals(RemoteKeyText.character(0x41, true, true)), "Letters and modifiers");
    check("!".equals(RemoteKeyText.character(0x31, true, false))
        && "?".equals(RemoteKeyText.character(0xbf, true, false))
        && "\\".equals(RemoteKeyText.character(0xdc, false, false)), "Digits and punctuation");

    final ScreenPacketQueue queue = new ScreenPacketQueue();
    final CountDownLatch started = new CountDownLatch(1), received = new CountDownLatch(1);
    final byte[][] result = new byte[1][];
    Thread writer = new Thread(new Runnable() {
      public void run() {
        try { started.countDown(); result[0] = queue.take(2000); received.countDown(); }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); }
      }
    });
    writer.start();
    check(started.await(1, TimeUnit.SECONDS), "Writer started");
    Thread.sleep(100);
    check(queue.offerControl(new byte[] {7}), "Control queued");
    check(received.await(250, TimeUnit.MILLISECONDS) && result[0][0] == 7,
        "Idle writer wakes immediately for control");
    queue.offerFrame(new byte[] {1}, true);
    queue.offerFrame(new byte[] {2}, true);
    queue.offerFrame(new byte[] {3}, true);
    queue.offerControl(new byte[] {9});
    check(queue.take(10)[0] == 9 && queue.take(10)[0] == 2 && queue.take(10)[0] == 3,
        "Control priority and bounded frame queue");
    ScreenPacketQueue video = new ScreenPacketQueue();
    video.offerFormat(new byte[] {4}, true);
    video.offerFrame(new byte[] {5}, false);
    check(video.take(10)[0] == 4 && video.take(1) == null, "Format retained and delta waits for keyframe");
    video.offerFrame(new byte[] {6}, true);
    video.offerFrame(new byte[] {7}, false);
    check(video.offerFrame(new byte[] {8}, false), "H264 overflow requests recovery");
    video.offerFrame(new byte[] {9}, false);
    check(video.take(1) == null, "H264 deltas discarded after overflow");
    video.offerFrame(new byte[] {10}, true);
    video.offerFrame(new byte[] {11}, false);
    check(video.take(10)[0] == 10 && video.take(10)[0] == 11, "H264 resumes from keyframe");
    check(ScreenOptions.valid(1280, 24, 6000, 2) && !ScreenOptions.valid(10, 24, 6000, 2)
        && !ScreenOptions.valid(1280, 24, 999999, 2), "Screen setting bounds");
    for (int i = 0; i < 32; i++) check(queue.offerControl(new byte[] {1}), "Control capacity " + i);
    check(!queue.offerControl(new byte[] {1}), "Control overflow is rejected");
    queue.close();
    check(queue.take(10) == null, "Closed writer stops");
    writer.join(1000);
  }
}
