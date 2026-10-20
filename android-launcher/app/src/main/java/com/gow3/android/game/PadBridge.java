package com.gow3.android.game;

import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

/** Android gamepad events -> gow3port's atomic pad-state file, without /dev/input permissions. */
final class PadBridge {
    private final File state;
    /** 物理源 → PS 按钮的用户映射（change add-pad-button-mapping）；空表 = 全默认。
     *  编码与查表规则在纯类 PadMapping,这里只做事件转发。 */
    private final java.util.Map<String,String> mapping;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Set<String> keys = new LinkedHashSet<>();
    private final Set<String> virtualKeys = new LinkedHashSet<>();
    /** 映射轴当前按下的目标按钮集:每次 motion 事件全量重算,值回中位即自动释放。 */
    private Set<String> axisKeys = new LinkedHashSet<>();
    private int lx=128, ly=128, rx=128, ry=128, l2, r2, hatX, hatY;
    private int virtualLx=128, virtualLy=128, virtualRx=128, virtualRy=128, virtualL2, virtualR2;
    private boolean virtualLeftActive, virtualRightActive;
    private boolean queued;
    /** 原生对话框打开期间置 true:key()/virtual()/motion() 只更新内存状态、不写 pad 文件,
     *  使游戏侧始终读到中性状态;对话框导航由 currentButtons() 读内存完成。 */
    private boolean dialogActive;

    PadBridge(File state, java.util.Map<String,String> mapping) {
        this.state=state;
        this.mapping=mapping==null ? java.util.Collections.emptyMap() : mapping;
        reset();
    }

    /** 对话框导航读它而不是文件:它包含物理手柄(keys/axisKeys)与屏幕手柄(virtualKeys)的并集。 */
    java.util.Set<String> currentButtons() {
        java.util.Set<String> all=new LinkedHashSet<>(keys); all.addAll(virtualKeys); all.addAll(axisKeys);
        return all;
    }

    /** 读当前 pad 文件内容,作为对话框关闭后要恢复的打开前快照。 */
    String snapshot() {
        try { return new String(java.nio.file.Files.readAllBytes(state.toPath()),StandardCharsets.US_ASCII); }
        catch (Exception ex) { return null; }
    }

    /** 对话框关闭后把快照写回,同时清掉对话框期间的按键(仍按住的那个键不会变成新的按下)。 */
    void restore(String snapshot) {
        keys.clear(); lx=ly=rx=ry=128; l2=r2=hatX=hatY=0;
        virtualKeys.clear(); virtualLx=virtualLy=virtualRx=virtualRy=128;
        virtualL2=virtualR2=0; virtualLeftActive=virtualRightActive=false;
        if (snapshot==null) { write(); return; }
        File temp=new File(state.getPath()+".tmp");
        try {
            state.getParentFile().mkdirs();
            try (FileOutputStream out=new FileOutputStream(temp)) {
                out.write(snapshot.getBytes(StandardCharsets.US_ASCII));
            }
            if (!temp.renameTo(state)) throw new java.io.IOException("Cannot restore controller state");
        } catch (java.io.IOException ex) { android.util.Log.e("Uncharted","Controller bridge restore",ex); }
    }

    void setDialogActive(boolean on) { dialogActive=on; }

    private String name(KeyEvent e) {
        // 翻译规则（默认表与用户映射）集中在纯类 PadMapping:scanCode 落在 Linux 手柄规范
        // 区间时优先 scan 路径,否则回落 keyCode;cross/circle 在所有约定里都是下/右。
        // 历史的方块/三角互换开关已删除,需要修正印刷标签的玩家在按键映射里自行绑定。
        return PadMapping.resolveButton(mapping, e.getScanCode(), e.getKeyCode());
    }

    boolean key(KeyEvent e) {
        String name=name(e);
        if (name==null) return false;
        if (e.getAction()==KeyEvent.ACTION_DOWN) keys.add(name);
        else if (e.getAction()==KeyEvent.ACTION_UP) keys.remove(name);
        // Publish button transitions immediately; the runtime samples at 20 ms intervals.
        // 对话框打开期间不写文件(保持对游戏中性),状态只留在内存供对话框导航。
        if (!dialogActive) write();
        return true;
    }

    boolean motion(MotionEvent e) {
        if (!e.isFromSource(InputDevice.SOURCE_JOYSTICK)) return false;
        lx=stick(e.getAxisValue(MotionEvent.AXIS_X));
        ly=stick(e.getAxisValue(MotionEvent.AXIS_Y));
        rx=stick(e.getAxisValue(MotionEvent.AXIS_Z));
        ry=stick(e.getAxisValue(MotionEvent.AXIS_RZ));
        l2=trigger(Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER),e.getAxisValue(MotionEvent.AXIS_BRAKE)));
        r2=trigger(Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER),e.getAxisValue(MotionEvent.AXIS_GAS)));
        hatX=Math.round(e.getAxisValue(MotionEvent.AXIS_HAT_X));
        hatY=Math.round(e.getAxisValue(MotionEvent.AXIS_HAT_Y));
        // 映射轴叠加按钮语义（design D1）:命中映射且越过阈值的轴按下对应 PS 按钮,
        // 模拟量(l2/r2 数值、HAT 方向)照常传递,两类扳机手柄绑定结果一致(spec)。
        Set<String> pressed=new LinkedHashSet<>();
        for (int axis:PadMapping.CAPTURABLE_AXES) {
            String target=PadMapping.resolveAxis(mapping,axis,e.getAxisValue(axis));
            if (target!=null) pressed.add(target);
        }
        axisKeys=pressed;
        if (!dialogActive && !queued) { queued=true; handler.postDelayed(() -> { queued=false; write(); },8); }
        return true;
    }

    private static int stick(float v) { return Math.abs(v)<0.06f ? 128 : Math.max(0,Math.min(255,Math.round((v+1)*127.5f))); }
    private static int trigger(float v) { return Math.max(0,Math.min(255,Math.round(v*255))); }

    void virtual(Set<String> buttons, int leftX, int leftY, int rightX, int rightY,
                 boolean leftActive, boolean rightActive, int leftTrigger, int rightTrigger) {
        virtualKeys.clear(); virtualKeys.addAll(buttons);
        virtualLx=leftX; virtualLy=leftY; virtualRx=rightX; virtualRy=rightY;
        virtualLeftActive=leftActive; virtualRightActive=rightActive;
        virtualL2=leftTrigger; virtualR2=rightTrigger;
        if (!dialogActive) write();
    }

    void reset() {
        keys.clear(); lx=ly=rx=ry=128; l2=r2=hatX=hatY=0;
        virtualKeys.clear(); virtualLx=virtualLy=virtualRx=virtualRy=128;
        virtualL2=virtualR2=0; virtualLeftActive=virtualRightActive=false;
        write();
    }

    private void write() {
        StringBuilder text=new StringBuilder();
        Set<String> all=new LinkedHashSet<>(keys); all.addAll(virtualKeys); all.addAll(axisKeys);
        int leftTrigger=Math.max(l2,virtualL2), rightTrigger=Math.max(r2,virtualR2);
        for (String key:all) {
            if ((key.equals("l2") && leftTrigger>0) || (key.equals("r2") && rightTrigger>0)) continue;
            text.append(key).append(' ');
        }
        // 默认 HAT 方向 token 只在对应半轴未被映射时追加:映射后由 axisKeys 提供目标按钮,
        // 避免同一物理方向同时产生默认方向与映射目标(spec:每按钮保留一个生效绑定)。
        if (hatX<0 && !axisMapped(PadMapping.AXIS_HAT_X,-1)) text.append("left ");
        if (hatX>0 && !axisMapped(PadMapping.AXIS_HAT_X, 1)) text.append("right ");
        if (hatY<0 && !axisMapped(PadMapping.AXIS_HAT_Y,-1)) text.append("up ");
        if (hatY>0 && !axisMapped(PadMapping.AXIS_HAT_Y, 1)) text.append("down ");
        text.append("lx=").append(virtualLeftActive ? virtualLx : lx)
            .append(" ly=").append(virtualLeftActive ? virtualLy : ly)
            .append(" rx=").append(virtualRightActive ? virtualRx : rx)
            .append(" ry=").append(virtualRightActive ? virtualRy : ry)
            .append(" l2=").append(leftTrigger).append(" r2=").append(rightTrigger).append('\n');
        File temp=new File(state.getPath()+".tmp");
        try {
            state.getParentFile().mkdirs();
            try (FileOutputStream out=new FileOutputStream(temp)) {
                out.write(text.toString().getBytes(StandardCharsets.US_ASCII));
            }
            if (!temp.renameTo(state)) throw new java.io.IOException("Cannot publish controller state");
        } catch (java.io.IOException ex) { android.util.Log.e("Uncharted", "Controller bridge",ex); }
    }

    /** 某模拟半轴是否被用户映射(含方向);映射后其默认语义(如 HAT 方向 token)被替换。 */
    private boolean axisMapped(int axis,float value) {
        return mapping.containsKey(PadMapping.axisSource(axis,value));
    }

    // ---- 震动回程：GOW3_PAD_RUMBLE_FILE -> InputDevice 振动 ----
    // 游戏进程里没有 SDL 手柄可开，scePadSetVibration 经 runtime_pad.c rumble_report() 把
    // 按 vibration= 百分比缩放后的强度写此文件（"rumble <strong> <weak> <ms> <seq>"，
    // 原子替换；相同强度 100ms 去重，持续效果每 200ms 心跳重写并递增 seq，ms=300）。
    // 这里 40ms 轮询：行变化（首次或新心跳）才触发，振幅取 max(strong, weak)；效果停止
    // 后 seq 不再变化，振动自然衰减，不会重放。文件缺失/损坏/无手柄设备=无震动，
    // 不影响其余桥（契约见 change port-upstream-graphics-fixes design.md 决策 9）。
    private static final long RUMBLE_POLL_MS=40;
    private android.content.Context context;
    private File rumbleFile;
    private Runnable rumbleLoop;
    private boolean rumblePolling;
    private String rumbleSeen;
    private Vibrator rumbleVibrator;
    private long rumbleVibratorCheckedAt;

    /** 开始轮询游戏侧震动请求；onCreate/onResume 调用，onPause 调 stopRumble。 */
    void startRumble(android.content.Context context,File file) {
        this.context=context;
        rumbleFile=file;
        if (rumblePolling) return;
        rumblePolling=true;
        rumbleLoop=this::pollRumble;
        handler.postDelayed(rumbleLoop,RUMBLE_POLL_MS);
    }

    /** 停止轮询并取消振动；只摘轮询回调，不碰 write() 的 8ms 合并回调。 */
    void stopRumble() {
        rumblePolling=false;
        if (rumbleLoop!=null) handler.removeCallbacks(rumbleLoop);
        rumbleSeen=null;
        if (rumbleVibrator!=null) rumbleVibrator.cancel();
    }

    /** onPause 后恢复轮询（沿用上次 startRumble 的 context 与文件）。 */
    void resumeRumble() {
        if (rumbleFile==null || rumblePolling) return;
        rumblePolling=true;
        handler.postDelayed(rumbleLoop,RUMBLE_POLL_MS);
    }

    private Vibrator gamepadVibrator() {
        long now=android.os.SystemClock.uptimeMillis();
        if (rumbleVibrator!=null || now-rumbleVibratorCheckedAt<2000) return rumbleVibrator;
        rumbleVibratorCheckedAt=now;
        if (context==null) return null;
        InputManager im=(InputManager)context.getSystemService(android.content.Context.INPUT_SERVICE);
        if (im==null) return null;
        for (int id:im.getInputDeviceIds()) {
            InputDevice dev=im.getInputDevice(id);
            if (dev==null || (dev.getSources() & InputDevice.SOURCE_JOYSTICK)==0) continue;
            Vibrator v=dev.getVibrator();
            if (v!=null && v.hasVibrator()) { rumbleVibrator=v; break; }
        }
        return rumbleVibrator;
    }

    private static int[] parseRumble(String text) {
        // 行格式 "rumble <strong> <weak> <ms> <seq>"；返回 {amplitude, ms}，无强度时 {0,0}。
        String[] parts=text.trim().split("\\s+");
        if (parts.length<4 || !parts[0].equals("rumble")) return new int[]{0,0};
        try {
            int strong=Integer.parseInt(parts[1]), weak=Integer.parseInt(parts[2]);
            int ms=Math.max(1,Math.min(1000,Integer.parseInt(parts[3])));
            return new int[]{Math.max(0,Math.min(255,Math.max(strong,weak))),ms};
        } catch (NumberFormatException ex) { return new int[]{0,0}; }
    }

    private void pollRumble() {
        if (!rumblePolling) return;
        String text=null;
        try { text=new String(java.nio.file.Files.readAllBytes(rumbleFile.toPath()),
                              StandardCharsets.US_ASCII); } catch (Exception ignored) { }
        // 行变化=首次命中或新的 200ms 心跳；效果结束后 seq 不再变化，振动自然衰减。
        if (text!=null && !text.equals(rumbleSeen)) {
            rumbleSeen=text;
            int[] r=parseRumble(text);
            if (r[0]>0) triggerVibration(r[0],r[1]);
        }
        handler.postDelayed(rumbleLoop,RUMBLE_POLL_MS);
    }

    private void triggerVibration(int amplitude,int ms) {
        Vibrator v=gamepadVibrator();
        if (v==null) return;
        if (android.os.Build.VERSION.SDK_INT>=26) {
            int a=v.hasAmplitudeControl() ? Math.max(1,amplitude) : VibrationEffect.DEFAULT_AMPLITUDE;
            v.vibrate(VibrationEffect.createOneShot(ms,a));
        } else {
            v.vibrate(ms);
        }
    }
}
