package ir.altek.link;
import java.util.*;

public final class UiLogicTest {
  static void check(boolean b,String label){if(!b)throw new AssertionError(label);System.out.println("PASS "+label);}
  static boolean near(float a,float b){return Math.abs(a-b)<.01f;}
  static float[] point(float[] m,float x,float y){return new float[]{m[0]*x+m[1]*y+m[2],m[3]*x+m[4]*y+m[5]};}
  static void fit(int w,int h,int vw,int vh){
    float[] m=VideoFit.clockwise(w,h,vw,vh);
    float[] tl=point(m,0,0),tr=point(m,w,0),bl=point(m,0,h),br=point(m,w,h);
    check(near(tl[0],tr[0])&&tl[1]<tr[1]&&bl[0]<tl[0]&&near(bl[1],tl[1]),"90 CW corner direction "+w+"x"+h);
    float width=tl[0]-bl[0],height=tr[1]-tl[1];
    check(near(width/height,(float)vh/vw)&&near((bl[0]+tl[0])/2,w/2f)&&near((tl[1]+tr[1])/2,h/2f),"Aspect preserved and centered");
    check(bl[0]>=-.01&&tl[1]>=-.01&&br[0]<=w+.01&&br[1]<=h+.01&&(near(width,w)||near(height,h)),"Fits without clipping");
  }
  static Store.Peer peer(String id,String name,long seen){Store.Peer p=new Store.Peer();p.id=id;p.name=name;p.seen=seen;return p;}
  public static void main(String[] args){
    VideoOptions defaults=new VideoOptions();
    check(defaults.isDefault()&&defaults.fps==15&&defaults.bitrate==4000000&&defaults.keySeconds==1,"Video defaults preserve tested settings");
    check(VideoOptions.periodNs(10)==100000000L&&VideoOptions.periodNs(25)==40000000L,"Frame pacing follows configured FPS");
    check(VideoOptions.valid(30,800,2)&&!VideoOptions.valid(0,800,1)&&!VideoOptions.valid(10,20001,1),"Video setting bounds");
    fit(320,480,320,240);fit(640,240,320,240);fit(480,480,640,360);
    boolean rejected=false;try{VideoFit.clockwise(0,100,320,240);}catch(IllegalArgumentException e){rejected=true;}
    check(rejected,"Invalid video dimensions rejected");
    long now=100000;
    Store.Peer offline=peer("1","A",now-16000),online=peer("2","Z",now-15999),second=peer("3","B",now);
    ArrayList<Store.Peer> peers=new ArrayList<Store.Peer>(Arrays.asList(offline,online,second));
    PeerOrder.sort(peers,now);check(peers.get(0)==second&&peers.get(1)==online&&peers.get(2)==offline,"Online first, then names, exact expiry boundary");
    PeerOrder.sort(peers,now+16001);check(peers.get(0)==offline&&peers.get(1)==second&&peers.get(2)==online,"Expired peers reordered consistently");
  }
}
