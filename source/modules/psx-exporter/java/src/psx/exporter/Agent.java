package psx.exporter;

import java.lang.instrument.*;
import java.io.*;
import java.net.URI;
import java.nio.file.*;
import java.security.ProtectionDomain;
import java.util.*;

/** Startup loader: consumes prepared immutable class bytes, never performs live analysis. */
public final class Agent {
    static final int PREPARED_PROPERTIES_LIMIT=65536;
    private Agent() {}
    private static byte[] boundedRead(Path path, int maximum) throws IOException {
        try (InputStream in = Files.newInputStream(path)) {
            byte[] bytes = in.readNBytes(maximum + 1);
            if (bytes.length > maximum) throw new IOException("prepared artifact too large");
            return bytes;
        }
    }
    private static URI pathUri(String path) {
        return Path.of(path).toAbsolutePath().normalize().toUri();
    }
    private static boolean sameUri(URI first, URI second) {
        return first.normalize().toString().equalsIgnoreCase(second.normalize().toString());
    }
    static boolean sameInputSource(Class<?> type, Path input) throws Exception {
        return type.getProtectionDomain().getCodeSource() != null && sameUri(
            Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toAbsolutePath().normalize().toUri(),
            input.toAbsolutePath().normalize().toUri());
    }
    static Map<String,byte[]> readDisplayOverlay(Properties p,Path directory,Map<String,String> originals,int totalBytes)throws Exception {
        if(!"1".equals(p.getProperty("display.capabilities")))throw new IllegalArgumentException("display unavailable");
        Set<String> names=new HashSet<>(Arrays.asList(p.getProperty("display.classes","").split(",")));
        if(names.size()!=9)throw new IllegalArgumentException("display class count");
        Map<String,byte[]> overlay=new HashMap<>();int overlayBytes=0;
        for(String name:names){
            if(!name.matches("[A-Za-z0-9_$]+")||!Objects.equals(originals.get(name),p.getProperty(name+".display.original")))throw new IllegalArgumentException("display original identity");
            byte[] bytes=boundedRead(directory.resolve("display-classes").resolve(name+".class"),1024*1024);overlayBytes+=bytes.length;
            if(overlayBytes+totalBytes>8*1024*1024||!Identity.hash(bytes).equals(p.getProperty(name+".display.prepared")))throw new IllegalArgumentException("display prepared identity");
            overlay.put(name,bytes);
        }
        return overlay;
    }
    static final class PreparedTransformer implements ClassFileTransformer {
        private final Map<String, byte[]> prepared;
        private final Map<String, String> originals;
        private final ClassLoader loader;
        private final URI inputUri;
        private final Set<String> poseClasses;
        private final Map<String,Integer> audioClasses;
        private final Set<String> terrainClasses;
        private Set<String> displayClasses=Set.of(),baseClasses=Set.of();
        private Map<String,Integer> aircraftClasses=Map.of();private Set<String> aircraftBaseClasses=Set.of();
        private Set<String> recorderClasses=Set.of(),recorderBaseClasses=Set.of();
        private Set<String> instrumentClasses=Set.of(),instrumentBaseClasses=Set.of();
        private Set<String> environmentClasses=Set.of(),environmentBaseClasses=Set.of();
        private Set<String> statusClasses=Set.of(),statusBaseClasses=Set.of();
        private Set<String> trafficClasses=Set.of(),trafficBaseClasses=Set.of();
        private Set<String> statusControlsClasses=Set.of(),statusControlsBaseClasses=Set.of();
        private Set<String> weightClasses=Set.of(),weightBaseClasses=Set.of();
        private Set<String> situationClasses=Set.of(),situationBaseClasses=Set.of();
        private Set<String> fmcClasses=Set.of(),fmcBaseClasses=Set.of();
        private Set<String> nativeFmcClasses=Set.of(),nativeFmcBaseClasses=Set.of();
        PreparedTransformer(Map<String, byte[]> prepared, Map<String, String> originals, ClassLoader loader, URI inputUri) {
            this(prepared,originals,loader,inputUri,prepared.keySet(),Map.of(),Set.of());
        }
        PreparedTransformer(Map<String,byte[]> prepared,Map<String,String> originals,ClassLoader loader,URI inputUri,Set<String> poseClasses,Map<String,Integer> audioClasses){
            this(prepared,originals,loader,inputUri,poseClasses,audioClasses,Set.of());
        }
        PreparedTransformer(Map<String,byte[]> prepared,Map<String,String> originals,ClassLoader loader,URI inputUri,Set<String> poseClasses,Map<String,Integer> audioClasses,Set<String> terrainClasses){
            this.prepared=prepared;this.originals=originals;this.loader=loader;this.inputUri=inputUri;this.poseClasses=poseClasses;this.audioClasses=audioClasses;this.terrainClasses=terrainClasses;
        }
        PreparedTransformer display(Set<String> classes,Set<String> base){displayClasses=Set.copyOf(classes);baseClasses=Set.copyOf(base);return this;}
        PreparedTransformer aircraft(Map<String,Integer> classes,Set<String> base){aircraftClasses=Map.copyOf(classes);aircraftBaseClasses=Set.copyOf(base);return this;}
        PreparedTransformer recorder(Set<String> classes,Set<String> lower){recorderClasses=Set.copyOf(classes);recorderBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer instrument(Set<String> classes,Set<String> lower){instrumentClasses=Set.copyOf(classes);instrumentBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer environment(Set<String> classes,Set<String> lower){environmentClasses=Set.copyOf(classes);environmentBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer status(Set<String> classes,Set<String> lower){statusClasses=Set.copyOf(classes);statusBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer traffic(Set<String> classes,Set<String> lower){trafficClasses=Set.copyOf(classes);trafficBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer statusControls(Set<String> classes,Set<String> lower){statusControlsClasses=Set.copyOf(classes);statusControlsBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer weight(Set<String> classes,Set<String> lower){weightClasses=Set.copyOf(classes);weightBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer situation(Set<String> classes,Set<String> lower){situationClasses=Set.copyOf(classes);situationBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer fmc(Set<String> classes,Set<String> lower){fmcClasses=Set.copyOf(classes);fmcBaseClasses=Set.copyOf(lower);return this;}
        PreparedTransformer nativeFmc(Set<String> classes,Set<String> lower){nativeFmcClasses=Set.copyOf(classes);nativeFmcBaseClasses=Set.copyOf(lower);return this;}
        private void mismatch(String name){if(instrumentClasses.contains(name)){try{InstrumentRecorder.failed();}catch(Throwable ignored){}if(!instrumentBaseClasses.contains(name))return;}if(nativeFmcClasses.contains(name)){try{FmcNativeRecordsService.disable();}catch(Throwable ignored){}if(!nativeFmcBaseClasses.contains(name))return;}if(fmcClasses.contains(name)){try{FmcSourceBackend.disable();}catch(Throwable ignored){}if(!fmcBaseClasses.contains(name))return;}if(situationClasses.contains(name)){try{SituationService.disable();}catch(Throwable ignored){}if(!situationBaseClasses.contains(name))return;}if(weightClasses.contains(name)){try{WeightService.disable();}catch(Throwable ignored){}if(!weightBaseClasses.contains(name))return;}if(statusControlsClasses.contains(name)){try{StatusControlsObserver.disable(15);}catch(Throwable ignored){}if(!statusControlsBaseClasses.contains(name))return;}if(trafficClasses.contains(name)){try{TrafficObserver.disable();}catch(Throwable ignored){}if(!trafficBaseClasses.contains(name))return;}if(statusClasses.contains(name)){try{StatusObserver.disable();}catch(Throwable ignored){}if(!statusBaseClasses.contains(name))return;}if(environmentClasses.contains(name)){try{EnvironmentObserver.disable();}catch(Throwable ignored){}if(!environmentBaseClasses.contains(name))return;}if(recorderClasses.contains(name)){try{PfdRecorder.failed();}catch(Throwable ignored){}if(!recorderBaseClasses.contains(name))return;}if(aircraftClasses.containsKey(name)){try{AircraftObserver.disable(aircraftClasses.get(name));}catch(Throwable ignored){}if(!aircraftBaseClasses.contains(name))return;}if(displayClasses.contains(name)){try{DisplayProbe.failed();}catch(Throwable ignored){}if(!baseClasses.contains(name))return;}if(terrainClasses.contains(name))Terrain.failed();if("Sound".equals(name)&&!poseClasses.contains(name))AudioOutput.failed();else if(audioClasses.containsKey(name)&&!poseClasses.contains(name))AudioObserver.disable(audioClasses.get(name));else if(!terrainClasses.contains(name)||poseClasses.contains(name))Observer.disable();}
        @Override public byte[] transform(ClassLoader actualLoader, String name,
            Class<?> redefining, ProtectionDomain domain, byte[] original) {
            try {
                boolean psxSource = false;
                if (domain != null && domain.getCodeSource() != null) {
                    URI source = domain.getCodeSource().getLocation().toURI();
                    if ("file".equalsIgnoreCase(source.getScheme()))
                        psxSource = sameUri(inputUri, Path.of(source).toAbsolutePath().normalize().toUri());
                }
                String expected = originals.get(name);
                if (!psxSource) {
                    if (expected != null) mismatch(name);
                    return null;
                }
                if (actualLoader != loader || redefining != null || expected == null
                    || !Identity.hash(original).equals(expected)) {
                    mismatch(name); return null;
                }
                return prepared.get(name);
            } catch (Throwable ignored) { mismatch(name); return null; }
        }
    }
    public static void premain(String options, Instrumentation instrumentation) {
        try {
            Path config = Path.of(options).toAbsolutePath().normalize();
            Properties p = new Properties();
            p.load(new ByteArrayInputStream(boundedRead(config, PREPARED_PROPERTIES_LIMIT)));
            if (!"1".equals(p.getProperty("schema")) || !"2".equals(p.getProperty("native.abi")))
                throw new IllegalArgumentException("schema/ABI");
            URI inputUri = pathUri(p.getProperty("input"));
            Path nativePath = Path.of(p.getProperty("native")).toAbsolutePath().normalize();
            URI runtimeUri = pathUri(p.getProperty("runtime"));
            URI agentUri = pathUri(p.getProperty("agent"));
            URI actualAgentUri = Path.of(Agent.class.getProtectionDomain().getCodeSource().getLocation().toURI())
                .toAbsolutePath().normalize().toUri();
            if (!sameUri(agentUri, actualAgentUri)) throw new IllegalArgumentException("agent context");
            Path actualRuntime = ProcessHandle.current().info().command().map(Path::of)
                .orElseThrow().toAbsolutePath().normalize();
            if (!sameUri(runtimeUri, actualRuntime.toUri())) throw new IllegalArgumentException("runtime context");
            Map<String, byte[]> prepared = new HashMap<>();
            Map<String, String> originals = new HashMap<>();
            Set<String> poseClasses=new HashSet<>(Arrays.asList(p.getProperty("pose.classes",p.getProperty("classes")).split(",")));
            Map<String,Integer> audioClasses=new HashMap<>();
            Set<String> terrainClasses=new HashSet<>(Arrays.asList(p.getProperty("terrain.classes","").split(",")));terrainClasses.remove("");
            boolean terrainFailure=false;
            int audioFailures=0;boolean outputFailure=false;
            byte[] indexBytes = boundedRead(config.getParent().resolve("class-index.properties"), 2 * 1024 * 1024);
            if (!Identity.hash(indexBytes).equals(p.getProperty("class-index.sha256")))
                throw new IllegalArgumentException("original class index identity");
            Properties index = new Properties();
            index.load(new ByteArrayInputStream(indexBytes));
            if (index.size() == 0 || index.size() > 16384
                || index.size() != Integer.parseInt(p.getProperty("original.classes")))
                throw new IllegalArgumentException("original class index count");
            for (String name : index.stringPropertyNames()) {
                String hash = index.getProperty(name);
                if (!name.matches("[A-Za-z0-9_$/]+") || !hash.matches("[0-9a-f]{64}"))
                    throw new IllegalArgumentException("original class index format");
                originals.put(name, hash);
            }
            String[] names = p.getProperty("classes").split(",");
            if (names.length < 2 || names.length > 128) throw new IllegalArgumentException("class count");
            int totalBytes = 0;
            for (String name : names) {
                if (!name.matches("[A-Za-z0-9_$]+")) throw new IllegalArgumentException("class name");
                try {
                byte[] bytes = boundedRead(config.getParent().resolve("classes").resolve(name + ".class"), 1024 * 1024);
                totalBytes += bytes.length;
                if (totalBytes > 8 * 1024 * 1024) throw new IllegalArgumentException("prepared byte budget");
                if (!Identity.hash(bytes).equals(p.getProperty(name + ".prepared")))
                    throw new IllegalArgumentException("prepared identity changed");
                if (prepared.put(name, bytes) != null) throw new IllegalArgumentException("duplicate class");
                if (!Objects.equals(originals.get(name), p.getProperty(name + ".original")))
                    throw new IllegalArgumentException("original class index disagreement");
                audioClasses.put(name,Integer.parseInt(p.getProperty(name+".audio.groups","0"))&255);
                }catch(Exception failure){if(poseClasses.contains(name))throw failure;prepared.remove(name);if(terrainClasses.contains(name))terrainFailure=true;audioFailures|=Integer.parseInt(p.getProperty(name+".audio.groups","0"))&255;if("Sound".equals(name))outputFailure=true;}
            }
            ClassLoader loader = ClassLoader.getSystemClassLoader();
            for (Class<?> c : instrumentation.getAllLoadedClasses())
                if (originals.containsKey(c.getName().replace('.', '/'))) throw new IllegalArgumentException("PSX already loaded");
            // A second vendor session keeps original PSX classes and starts no exporter services.
            System.load(nativePath.toString());
            if (!BridgeNative.claimRuntime()) { Observer.disable(); return; }
            // Force helper initialization before callbacks can run.
            Observer.enabled = false;
            AudioObserver.capabilities=Integer.parseInt(p.getProperty("audio.groups","0"))&255;
            AudioObserver.disable(audioFailures);
            AudioObserver.suppressionCapabilities=outputFailure?0:Integer.parseInt(p.getProperty("audio.suppression","0"))&AudioObserver.capabilities;
            AudioOutput.ready=false;
            Terrain.ready=!terrainFailure&&Integer.parseInt(p.getProperty("terrain.capabilities","0"))==2&&prepared.keySet().containsAll(terrainClasses);
            prepareTiming();
            Set<String> basePrepared=Set.copyOf(prepared.keySet());Set<String> displayClasses=new HashSet<>();
            try{
                Map<String,byte[]> overlay=readDisplayOverlay(p,config.getParent(),originals,totalBytes);
                displayClasses.addAll(overlay.keySet());
                System.load(nativePath.toString());
                if(BridgeNative.displayAbiVersion()!=1)throw new IllegalArgumentException("display runtime ABI");
                DisplayProbe.prepare();prepared.putAll(overlay);
            }catch(Throwable unavailable){displayClasses.clear();try{DisplayProbe.failed();}catch(Throwable ignored){}}
            Set<String> aircraftBaseClasses=Set.copyOf(prepared.keySet());Map<String,Integer> aircraftClasses=new HashMap<>();
            try{
                Map<String,byte[]> overlay=readAircraftOverlay(p,config.getParent(),originals,prepared);
                System.load(nativePath.toString());if(BridgeNative.aircraftAbiVersion()!=1)throw new IllegalArgumentException("aircraft runtime ABI");
                AircraftObserver.prepare();AircraftObserver.capabilities=Integer.parseInt(p.getProperty("aircraft.groups"));
                for(String name:overlay.keySet())aircraftClasses.put(name,Integer.parseInt(p.getProperty(name+".aircraft.groups")));
                prepared.putAll(overlay);
            }catch(Throwable unavailable){aircraftClasses.clear();try{AircraftObserver.disable(63);}catch(Throwable ignored){}}
            Set<String> environmentBaseClasses=Set.copyOf(prepared.keySet()),environmentClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=EnvironmentLayer.read(p,config.getParent(),originals,prepared);if(overlay.isEmpty()||!EnvironmentLayer.available())throw new IllegalArgumentException("environment JNI unavailable");EnvironmentObserver.prepare();EnvironmentObserver.capabilities=Integer.parseInt(p.getProperty("environment.groups"));EnvironmentContextObserver.qualified=Boolean.parseBoolean(p.getProperty("environment.context","false"));WeatherChangeObserver.qualified=Boolean.parseBoolean(p.getProperty("environment.manual-weather","false"));EnvironmentVisibilityObserver.qualified=Boolean.parseBoolean(p.getProperty("environment.local-visibility","false"));prepared.putAll(overlay);environmentClasses.addAll(overlay.keySet());}
            catch(Throwable unavailable){try{EnvironmentObserver.disable();}catch(Throwable ignored){}}
            Set<String> statusBaseClasses=Set.copyOf(prepared.keySet()),statusClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=StatusLayer.read(p,config.getParent(),originals,prepared);if(overlay.isEmpty()||!StatusLayer.available())throw new IllegalArgumentException("status JNI unavailable");StatusObserver.prepare();StatusObserver.qualified=true;prepared.putAll(overlay);statusClasses.addAll(overlay.keySet());}
            catch(Throwable unavailable){try{StatusObserver.disable();}catch(Throwable ignored){}}
            Set<String> trafficBaseClasses=Set.copyOf(prepared.keySet()),trafficClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=TrafficLayer.read(p,config.getParent(),originals,prepared);if(overlay.isEmpty())throw new IllegalArgumentException("traffic unavailable");TrafficObserver.prepare();TrafficObserver.qualified=true;prepared.putAll(overlay);trafficClasses.addAll(overlay.keySet());TrafficObserver.prepareNative();}
            catch(Throwable unavailable){try{TrafficObserver.disable();}catch(Throwable ignored){}}
            Set<String> statusControlsBaseClasses=Set.copyOf(prepared.keySet()),statusControlsClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=StatusControlsLayer.read(p,config.getParent(),originals,prepared);if(overlay.isEmpty()||!StatusControlsObserver.available())throw new IllegalArgumentException("IOS Status unavailable");StatusControlsObserver.prepare();StatusControlsObserver.capabilities=Integer.parseInt(p.getProperty("iosstatus.capabilities"));prepared.putAll(overlay);statusControlsClasses.addAll(overlay.keySet());}
            catch(Throwable unavailable){try{StatusControlsObserver.disable(15);}catch(Throwable ignored){}}
            Set<String> weightBaseClasses=Set.copyOf(prepared.keySet()),weightClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=WeightLayer.read(p,config.getParent(),originals,prepared);if(overlay.isEmpty()||!WeightLayer.available())throw new IllegalArgumentException("Weight actions unavailable");WeightHooks.prepare();prepared.putAll(overlay);weightClasses.addAll(overlay.keySet());WeightService.preparedActions=15;}
            catch(Throwable unavailable){WeightService.preparedActions=0;}
            Set<String> situationBaseClasses=Set.copyOf(prepared.keySet()),situationClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=SituationLayer.read(p,config.getParent(),originals,prepared);prepared.putAll(overlay);situationClasses.addAll(overlay.keySet());}catch(Throwable unavailable){try{SituationService.disable();}catch(Throwable ignored){}}
            Set<String> fmcBaseClasses=Set.copyOf(prepared.keySet()),fmcClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=FmcSourceLayer.read(p,config.getParent(),originals,prepared);if(overlay.isEmpty()||!FmcSourceLayer.available())throw new IllegalArgumentException("FMC source unavailable");FmcCoherence.beginCopy();FmcCompatibilityWitness.begin();FmcBufferCoherence.clear();prepared.putAll(overlay);fmcClasses.addAll(overlay.keySet());FmcSourceBackend.prepared=true;}
            catch(Throwable unavailable){FmcSourceBackend.disable();}
            Set<String> nativeFmcBaseClasses=Set.copyOf(prepared.keySet()),nativeFmcClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=FmcNativeRecordsLayer.read(p,config.getParent(),originals,prepared);if(overlay.isEmpty()||!FmcNativeRecordsLayer.available())throw new IllegalArgumentException("native FMC unavailable");int budget=0;for(byte[] bytes:prepared.values())budget+=bytes.length;for(var entry:overlay.entrySet()){byte[] prior=prepared.get(entry.getKey());budget+=entry.getValue().length-(prior==null?0:prior.length);}if(budget>8*1024*1024)throw new IllegalArgumentException("native FMC prepared byte budget");prepared.putAll(overlay);nativeFmcClasses.addAll(overlay.keySet());}
            catch(Throwable unavailable){FmcNativeRecordsService.disable();}
            Set<String> instrumentBaseClasses=Set.copyOf(prepared.keySet()),instrumentClasses=new HashSet<>();
            boolean instrumentReady=false;
            // Keep verified guarded call sites even when runtime activation is
            // unavailable: Recorder-LAST was qualified against these bytes.
            // Missing instrument JNI must not invalidate the six-DU base hash.
            try{Map<String,byte[]> overlay=InstrumentLayer.read(p,config.getParent(),originals,prepared);if(overlay.isEmpty()||displayClasses.isEmpty())throw new IllegalArgumentException("instrument prerequisite");prepared.putAll(overlay);instrumentClasses.addAll(overlay.keySet());if(!InstrumentLayer.available())throw new IllegalArgumentException("instrument JNI unavailable");InstrumentRecorder.prepare(InstrumentLayer.lowerAvailable(p));instrumentReady=true;}
            catch(Throwable unavailable){try{InstrumentRecorder.failed();}catch(Throwable ignored){}}
            Set<String> recorderBaseClasses=Set.copyOf(prepared.keySet()),recorderClasses=new HashSet<>();
            try{Map<String,byte[]> overlay=readRecorderOverlay(p,config.getParent(),originals,prepared);if(displayClasses.isEmpty()||(BridgeNative.recorderAbiVersion()!=1||BridgeNative.duRecorderAbiVersion()!=1))throw new IllegalArgumentException("recorder prerequisite");PfdRecorder.prepare();prepared.putAll(overlay);recorderClasses.addAll(overlay.keySet());}
            catch(Throwable unavailable){try{PfdRecorder.failed();}catch(Throwable ignored){}}
            instrumentation.addTransformer(new PreparedTransformer(prepared, originals, loader, inputUri,poseClasses,audioClasses,terrainClasses).display(displayClasses,basePrepared).aircraft(aircraftClasses,aircraftBaseClasses).environment(environmentClasses,environmentBaseClasses).status(statusClasses,statusBaseClasses).traffic(trafficClasses,trafficBaseClasses).statusControls(statusControlsClasses,statusControlsBaseClasses).weight(weightClasses,weightBaseClasses).situation(situationClasses,situationBaseClasses).fmc(fmcClasses,fmcBaseClasses).nativeFmc(nativeFmcClasses,nativeFmcBaseClasses).instrument(instrumentClasses,instrumentBaseClasses).recorder(recorderClasses,recorderBaseClasses), false);
            if(!displayClasses.isEmpty())try{
            String metadata=recorderMetadata(p,config);
                DisplayProbe.startWorker(nativePath.toString(),metadata);
                if(!recorderClasses.isEmpty())try{PfdRecorder.startWorker(metadata);}catch(Throwable unavailable){try{PfdRecorder.failed();}catch(Throwable ignored){}}
                if(instrumentReady)try{InstrumentRecorder.startWorker(metadata);}catch(Throwable unavailable){try{InstrumentRecorder.failed();}catch(Throwable ignored){}}
            }catch(Throwable unavailable){try{DisplayProbe.failed();}catch(Throwable ignored){}}
            Observer.startWorker(nativePath.toString());
            try { NavigationSelection.start(loader, Path.of(inputUri)); } catch (Throwable ignored) {}
            try { WeightService.start(loader, Path.of(inputUri), nativePath); } catch (Throwable ignored) {}
            try { if("1".equals(p.getProperty("reset.capabilities")))ResetMalfunctions.start(loader,Path.of(inputUri),nativePath); } catch(Throwable ignored) {}
            try{if(!situationClasses.isEmpty())SituationService.start(loader,Path.of(inputUri));}catch(Throwable ignored){try{SituationService.disable();}catch(Throwable unavailable){}}
        } catch (Throwable ignored) {
            // A valid agent must never abort PSX merely because its optional export cannot start.
            Observer.disable();
        }
    }
    static void prepareTiming(){
        try{FrameTrace.prepare();}catch(Throwable ignored){
            try{FrameTrace.failed();}catch(Throwable unavailable){}
        }
    }
    static String recorderMetadata(Properties p,Path config)throws Exception {
        return "{\"exporter_version\":"+Identity.json(BuildVersion.VALUE)+",\"input_sha256\":"+Identity.json(p.getProperty("input.sha256"))+",\"agent_sha256\":"+Identity.json(p.getProperty("agent.sha256"))+",\"native_sha256\":"+Identity.json(p.getProperty("native.sha256"))+",\"prepared_sha256\":"+Identity.json(Identity.hash(config))+",\"java\":"+Identity.json(System.getProperty("java.version"))+"}";
    }
    static Map<String,byte[]> readRecorderOverlay(Properties p,Path directory,Map<String,String> originals,Map<String,byte[]> base)throws Exception {
        if(!"1".equals(p.getProperty("recorder.capabilities")))throw new IllegalArgumentException("recorder unavailable");
        Set<String> expected=new TreeSet<>(Set.of("Core","Core$PrestoThread","EfisFont","EgpwsM123457","EicasDisplayPrimary","EicasDisplayPrimary$Cf6Display","EicasDisplayPrimary$FlapGroup","EicasDisplayPrimary$GearGroup","EicasDisplayPrimary$PwDisplay","EicasDisplayPrimary$RbDisplay","EicasDisplaySecondary","EicasPage","EicasPageDrs","EicasPageEcs","EicasPageEcs$Outflow","EicasPageEcs$Pack","EicasPageEcs$Prsov","EicasPageEcs$Valve","EicasPageElec","EicasPageEng","EicasPageEng$DisplayGe","EicasPageEng$DisplayPw","EicasPageEng$DisplayRr","EicasPageEng$FltCtrlRudd","EicasPageEng$HalfGe","EicasPageEng$HalfPw","EicasPageEng$HalfRr","EicasPageFuel","EicasPageFuel$Pump","EicasPageFuel$Tank","EicasPageFuel$TankM","EicasPageFuel$Valve","EicasPageGear","EicasPageGear$Group","EicasPageGear$Nose","EicasPageHyd","EicasPageHyd$Hyd","EicasPageStat","EicasPageStat$FltCtrlBigV","EicasPageStat$FltCtrlRudd","EicasPageStat$FltCtrlSmaV","EicasPageStat$Hyd","FmcDepArr","FmcFixInfo$Intersection","FmcFixInfo$Page","FmcFixInfo$Page$1","FmcHold","FmcNg","FmcVnavCalc","FmcVnavCalc$ProfilePoint","FmcVnavDes$1","IntegratedDisplaySystem","IntegratedDisplaySystem$Box","NavigationDisplay","NavigationDisplay$Cdi","NdCircle","NdJobs","NdPreparer","PrimaryFlightDisplay","PrimaryFlightDisplay$AfdsString","RoutesStby","Tcas","Tcas$1","Tcas$2","Tcas$3","Tcas$4","Tcas$DrawJob","Tcas$Target","Visuals"));
        String[] names=p.getProperty("recorder.classes","").split(",");if(names.length!=expected.size()||!new TreeSet<>(Arrays.asList(names)).equals(expected))throw new IllegalArgumentException("recorder complete layer");
        Map<String,byte[]> overlay=new TreeMap<>();int size=base.values().stream().mapToInt(b->b.length).sum();
        for(String name:names){String baseHash=base.containsKey(name)?Identity.hash(base.get(name)):originals.get(name);if(!Objects.equals(originals.get(name),p.getProperty(name+".recorder.original"))||!Objects.equals(baseHash,p.getProperty(name+".recorder.base")))throw new IllegalArgumentException("recorder original/base identity");
            byte[] bytes=boundedRead(directory.resolve("recorder-classes").resolve(name+".class"),1024*1024);size+=bytes.length;if(size>8*1024*1024||!Identity.hash(bytes).equals(p.getProperty(name+".recorder.prepared")))throw new IllegalArgumentException("recorder prepared identity");overlay.put(name,bytes);}
        return overlay;
    }
    static Map<String,byte[]> readAircraftOverlay(Properties p,Path directory,Map<String,String> originals,Map<String,byte[]> base)throws Exception {
        int groups=Integer.parseInt(p.getProperty("aircraft.groups","0"));if(groups<=0||(groups&~63)!=0)throw new IllegalArgumentException("aircraft unavailable");
        String[] names=p.getProperty("aircraft.classes","").split(",");if(names.length==0||names.length>128)throw new IllegalArgumentException("aircraft class count");
        Map<String,byte[]> overlay=new HashMap<>();int size=base.values().stream().mapToInt(b->b.length).sum();
        for(String name:names){
            if(!name.matches("[A-Za-z0-9_$]+")||!Objects.equals(originals.get(name),p.getProperty(name+".aircraft.original")))throw new IllegalArgumentException("aircraft original identity");
            String baseHash=base.containsKey(name)?Identity.hash(base.get(name)):originals.get(name);
            if(baseHash==null||!baseHash.equals(p.getProperty(name+".aircraft.base")))throw new IllegalArgumentException("aircraft base identity");
            int mask=Integer.parseInt(p.getProperty(name+".aircraft.groups","0"));if(mask<=0||(mask&~groups)!=0)throw new IllegalArgumentException("aircraft class groups");
            byte[] bytes=boundedRead(directory.resolve("aircraft-classes").resolve(name+".class"),1024*1024);size+=bytes.length;
            if(size>8*1024*1024||!Identity.hash(bytes).equals(p.getProperty(name+".aircraft.prepared"))||overlay.put(name,bytes)!=null)throw new IllegalArgumentException("aircraft prepared identity");
        }
        if(!overlay.containsKey("Core"))throw new IllegalArgumentException("aircraft source root absent");return overlay;
    }
}
