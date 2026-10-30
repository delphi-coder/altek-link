package ir.altek.link;

import java.util.*;

/** One time snapshot keeps comparisons consistent while a contact crosses the offline threshold. */
public final class PeerOrder {
  public static void sort(List<Store.Peer> peers, final long now) {
    Collections.sort(
        peers,
        new Comparator<Store.Peer>() {
          public int compare(Store.Peer a, Store.Peer b) {
            boolean onlineA = now - a.seen < 16000, onlineB = now - b.seen < 16000;
            if (onlineA != onlineB) return onlineA ? -1 : 1;
            int name = a.name.compareToIgnoreCase(b.name);
            return name != 0 ? name : a.id.compareTo(b.id);
          }
        });
  }
}
