package psx.exporter;

/** Fixed primitive extension collected inside the original coherent environment copy. */
public final class EnvironmentContextObserver {
 static volatile boolean qualified;
 private static volatile boolean ready,failed;
 private static int identity,active,planet,zone,valid;
 private static long station,at,until;
 private EnvironmentContextObserver() {}
 static void prepare(){if(!qualified)return;try{if(EnvironmentContextNative.abiVersion()!=1)throw new LinkageError("environment context ABI");EnvironmentContextNative.capabilities(true);ready=true;}catch(Throwable failure){ready=false;}}
 public static void disable(){failed=true;ready=false;try{EnvironmentContextNative.capabilities(false);}catch(Throwable ignored){}}
 static void reset(){valid=0;station=at=until=0;active=-1;}
 public static void begin(Object current,Object basic,Object first){
  if(!ready||failed)return;at=BridgeNative.qpc();identity=current==basic?0:current==first?1:2;
 }
 public static void copy(int tab,int basicVisibility,int firstVisibility,String firstStation){
  if(!ready||failed)return;active=identity<2?identity:tab;planet=basicVisibility;zone=firstVisibility;valid=0;
  if(active>=0&&active<=7)valid|=1;else active=-1;
  if(planet>=0&&planet<=9999)valid|=2;else planet=0;
  if(zone>=0&&zone<=9999)valid|=4;else zone=0;
  station=0;
  if(firstStation!=null&&firstStation.length()<8){boolean okay=true;for(int i=0;i<firstStation.length();i++){char c=firstStation.charAt(i);if(c<32||c>126){okay=false;break;}station|=((long)c)<<(i*8);}if(okay)valid|=8;else station=0;}
  until=BridgeNative.qpc();
 }
 static void publish(long sequence,long lifecycle,long start,long version){
  if(!ready||failed||valid==0)return;
  try{EnvironmentContextNative.publish(sequence,lifecycle,start,version,at,until,valid,active,planet,zone,station);}catch(Throwable failure){disable();}
 }
}
