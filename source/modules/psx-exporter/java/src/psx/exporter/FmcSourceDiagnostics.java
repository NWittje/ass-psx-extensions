package psx.exporter;

import java.nio.file.*;

/** Bounded current-state explanation on the existing service worker. No
 * injected callback calls this formatter or performs filesystem work. The
 * process/birth pair prevents an old diagnostic from becoming live evidence. */
final class FmcSourceDiagnostics {
    private static String last;
    private static int changes;
    private static String captureFailure="";
    private static String nativeFailure="null";
    private FmcSourceDiagnostics(){}
    static synchronized String lastNativeFailure(){return nativeFailure;}
    static synchronized void nativeActionFailure(long operation,FmcCommandDispatch.Observation outcome){
        try{
        nativeFailure="{\"operation\":"+operation+",\"phase\":"+Identity.json(outcome.phase().name())
            +",\"failure\":"+Identity.json(explanation(outcome.failure()))
            +",\"final_check_failure\":"+Identity.json(explanation(outcome.finalCheckFailure()))
            +",\"scheduling_failure\":"+Identity.json(explanation(outcome.schedulingFailure()))+"}";
        record("native_record_action_failure",outcome.failure()!=null?outcome.failure():outcome.finalCheckFailure()!=null?outcome.finalCheckFailure():outcome.schedulingFailure());
        }catch(Throwable unavailable){/* Preserve the native outcome if formatting fails. */}
    }
    private static String explanation(Throwable failure){
        if(failure==null)return "";
        StringBuilder text=new StringBuilder(failure.toString());
        StackTraceElement[] frames=failure.getStackTrace();
        for(int i=0;i<Math.min(4,frames.length)&&text.length()<512;i++)text.append(" at ").append(frames[i]);
        return text.substring(0,Math.min(512,text.length()));
    }
    static synchronized void record(String phase,Throwable failure){
        try{
            String reason=failure==null?"":failure.toString();
            if(reason.length()>512)reason=reason.substring(0,512);
            if(phase.equals("capture_failure"))captureFailure=reason;
            String state=phase+":"+reason;
            boolean nativeAction=phase.equals("native_record_action_failure");
            if(!nativeAction&&(state.equals(last)||changes>=32))return;
            String local=System.getenv("LOCALAPPDATA");if(local==null||local.isBlank())return;
            ProcessHandle process=ProcessHandle.current();
            Path folder=Path.of(local).resolve("PSX-XPlane/exporter/fmc-source");Files.createDirectories(folder);
            Path target=folder.resolve(process.pid()+".json"),stage=folder.resolve(process.pid()+".pending");
            String report="{\"schema\":1,\"pid\":"+process.pid()+",\"process_start_unix_ms\":"+process.info().startInstant().orElseThrow().toEpochMilli()
                +",\"recorded_unix_ms\":"+System.currentTimeMillis()+",\"phase\":"+Identity.json(phase)+",\"reason\":"+Identity.json(reason)
                +",\"last_capture_failure\":"+Identity.json(captureFailure)+",\"last_native_failure\":"+nativeFailure+",\"transition\":"+(changes+1)+"}";
            Files.writeString(stage,report,StandardOpenOption.CREATE,StandardOpenOption.TRUNCATE_EXISTING);
            Files.move(stage,target,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
            last=state;changes++;
        }catch(Throwable unavailable){/* Diagnostic failure cannot affect a service. */}
    }
}
