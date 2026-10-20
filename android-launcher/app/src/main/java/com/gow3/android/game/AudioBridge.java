package com.gow3.android.game;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.system.Os;
import android.system.OsConstants;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.RandomAccessFile;

/** Game audio: SDL's disk driver writes the mixed output (s16 stereo 48 kHz, run-gow3.sh) into a
 *  FIFO; this plays it. The FIFO is held open read-write, so the game's open never blocks and a
 *  restarted game writes into the same pipe. The playback rate paces the game's audio thread.
 *  Self-healing: the supervisor loop reinitializes the AudioTrack and reopens the FIFO on any
 *  transient failure (write==0 while paused, write<0, AudioTrack uninitialized, EOF), without
 *  ever deleting the FIFO inode so the game's fd can reconnect. */
final class AudioBridge {
    private static final int RATE=48000, FRAME=4;
    private static final int F_SETPIPE_SZ=1031;
    private static final long REINIT_BACKOFF_MS=250;
    private final File fifo;
    private final File ready;
    private volatile boolean running;
    private volatile boolean wantPlay=true;
    private volatile AudioTrack track;
    private volatile RandomAccessFile pipeHandle;
    private volatile int state; // 0 starting, 1 ready, -1 failed/stopped

    AudioBridge(File fifo) { this.fifo=fifo; this.ready=new File(fifo.getPath()+".ready"); }

    void start() {
        if (running) return;
        ready.delete();
        state=0;
        running=true;
        wantPlay=true;
        Thread thread=new Thread(this::run,"Uncharted-audio");
        thread.setPriority(Thread.MAX_PRIORITY); thread.setDaemon(true); thread.start();
    }
    void stop() {
        running=false; wantPlay=false; state=-1; ready.delete();
        AudioTrack t=track; if (t!=null) try { t.pause(); } catch (Exception ignored) {}
        RandomAccessFile p=pipeHandle;
        if (p!=null) try { p.close(); } catch (IOException ignored) {}
    }
    void pause() {
        wantPlay=false;
        AudioTrack t=track; if (t!=null) try { t.pause(); } catch (Exception ignored) {}
    }
    void resume() {
        wantPlay=true;
        AudioTrack t=track; if (t!=null) try { t.play(); } catch (Exception ignored) {}
    }

    boolean awaitReady(long timeoutMs) {
        long deadline=System.currentTimeMillis()+timeoutMs;
        while (state==0 && System.currentTimeMillis()<deadline) {
            try { Thread.sleep(10); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return false; }
        }
        return state==1 && running && ready.isFile();
    }

    private void run() {
        android.util.Log.i("Uncharted","audio bridge started");
        while (running) {
            AudioTrack active=null;
            RandomAccessFile pipe=null;
            try {
                File parent=fifo.getParentFile();
                if (parent!=null) parent.mkdirs();
                // Never delete an existing FIFO (the game holds the write-end fd; deleting would
                // create a new inode and break the cross-process contract). Only replace if the
                // path is not a fifo.
                if (fifo.exists() && !isFifo(fifo)) fifo.delete();
                if (!fifo.exists()) Os.mkfifo(fifo.getPath(),0600);
                pipe=new RandomAccessFile(fifo,"rw");
                try (FileInputStream in=new FileInputStream(pipe.getFD())) {
                    pipeHandle=pipe;
                    FileDescriptor fd=pipe.getFD();
                    // A small pipe keeps the delay short (~40 ms); the default 64 KiB is ~340 ms.
                    try { Os.fcntlInt(fd,F_SETPIPE_SZ,8192); } catch (Exception ignored) {}
                    int min=AudioTrack.getMinBufferSize(RATE,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT);
                    AudioTrack t=new AudioTrack.Builder()
                        .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                        .setAudioFormat(new AudioFormat.Builder().setSampleRate(RATE)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                        .setBufferSizeInBytes(Math.max(min,RATE*FRAME/20))
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY).build();
                    if (t.getState()!=AudioTrack.STATE_INITIALIZED) throw new IOException("AudioTrack is not initialized");
                    active=t;
                    track=t;
                    if (wantPlay) t.play();
                    if (wantPlay && t.getPlayState()!=AudioTrack.PLAYSTATE_PLAYING)
                        throw new IOException("AudioTrack did not start");
                    // Recreate the ready marker on every successful (re)initialization so that a
                    // restarted game or a process that consults it always sees a fresh contract.
                    if (ready.exists() && !ready.delete())
                        throw new IOException("cannot clear stale ready marker");
                    if (!ready.createNewFile())
                        throw new IOException("audio ready marker could not be created");
                    state=1;
                    android.util.Log.i("Uncharted","audio: ready (48 kHz, s16 stereo)");

                    byte[] buffer=new byte[4096];
                    int carry=0;
                    int stallLogCounter=0;
                    while (running) {
                        int n=in.read(buffer,carry,buffer.length-carry);
                        if (n<0) throw new IOException("FIFO read EOF");
                        if (n==0) { Thread.sleep(5); continue; }
                        int whole=(carry+n)/FRAME*FRAME;
                        int written=0;
                        while (written<whole && running) {
                            int result=t.write(buffer,written,whole-written,AudioTrack.WRITE_BLOCKING);
                            if (result==0) {
                                // Track is paused (onPause) or momentarily starved: sleep, then
                                // retry. If the activity is resumed again we re-arm play().
                                if (wantPlay && t.getPlayState()!=AudioTrack.PLAYSTATE_PLAYING)
                                    try { t.play(); } catch (Exception ignored) {}
                                if (++stallLogCounter%200==1)
                                    android.util.Log.d("Uncharted","audio: write returned 0 (paused/stalled)");
                                Thread.sleep(5);
                                continue;
                            }
                            if (result<0) throw new IOException("AudioTrack.write failed: "+result);
                            written+=result;
                            stallLogCounter=0;
                            if (t.getState()!=AudioTrack.STATE_INITIALIZED)
                                throw new IOException("AudioTrack became uninitialized");
                        }
                        carry=carry+n-whole;
                        if (carry>0) System.arraycopy(buffer,whole,buffer,0,carry);
                    }
                }
            } catch (Exception ex) {
                if (!running) break;
                android.util.Log.w("Uncharted","audio bridge reinitializing: "+ex.getClass().getSimpleName()+": "+ex.getMessage());
            } finally {
                if (active!=null) {
                    try { active.stop(); } catch (Exception ignored) {}
                    try { active.release(); } catch (Exception ignored) {}
                }
                if (track==null || active==track) track=null;
                if (pipe!=null) try { pipe.close(); } catch (IOException ignored) {}
                pipeHandle=null;
                // Drop the ready marker while the bridge is between sessions; the next successful
                // init recreates it. run-gow3.sh only consults it at game start, so a transient
                // absence is harmless.
                if (running) ready.delete();
            }
            if (!running) break;
            try { Thread.sleep(REINIT_BACKOFF_MS); } catch (InterruptedException ie) { Thread.currentThread().interrupt(); break; }
        }
        android.util.Log.i("Uncharted","audio bridge stopped");
    }
    private static boolean isFifo(File file) {
        try { return OsConstants.S_ISFIFO(Os.stat(file.getPath()).st_mode); } catch (Exception e) { return false; }
    }
}
