package com.gow3.android.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Typeface;
import android.view.View;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.BufferedReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import com.gow3.android.core.settings.FrameGenSettings;
import com.gow3.android.core.settings.Gow3Settings;

/** Graphics settings for the Thor, kept in gow3.ini (run-gow3.sh reads them at start: scene
 *  size, upscaler, the game's effect patches). Other lines and comments of the file are kept. */
final class SettingsDialog {
    /** Scene sizes of the presets at the Thor's 1280x720 output (run-gow3.sh). */
    private static final String[][] EFFECTS={
        {"effect_chromatic_aberration","Chromatic aberration","1"},
        {"effect_dof","Depth of field","1"},
        {"effect_motion_blur","Motion blur","1"},
        {"effect_ssao","Ambient occlusion (SSAO)","1"},
        {"effect_game_aa","Game's own anti-aliasing","1"},
        {"effect_dynamic_shadows","Shadows from dynamic lights","1"},
        {"effect_ssr","Screen-space reflections (not in the original)","0"},
    };
    // FSR 4 (INT8) is not offered: ~470 ms a frame and a black scene on the Adreno 740 (run-gow3.sh).
    /** Game settings: the game patches run-gow3.sh applies at start (patch-parts), as upstream's
     *  System menu pages and launcher offer them: {key, title, note, default}. */
    private static final String[][] GAME_PATCHES={
        {"skip_network_choice","Skip the online/offline screen","Start straight at the main menu, offline (there is no PSN here)","1"},
        {"skip_intro","Skip startup intros","The logos and intro movie at start","0"},
        {"debug_camera","Free camera","Toggle it with Cross + L3","0"},
        {"debug_menu","Debug menu","The game's own debug menu; needs its font files","0"},
    };
    /** Cheats: code hooks of the game applied at start (build_gow3_patch_parts.py, run-gow3.sh).
     *  key MUST match the runtime's cheat_keys (gow3_settings.cpp): cheat_item is the singular one. */
    private static final String[][] CHEATS={
        {"cheat_health","Infinite health","Your HP refills to full every frame","0"},
        {"cheat_item","Infinite blood vials and items","Using a blood vial, bullet or other item leaves its count as it was "
            +"(items moved to storage or given away stay too)","0"},
    };
    private static final String[] UPSCALER_VALUES={"off","gsr2","fsr3"};
    private static final String[] UPSCALER_NAMES={"Off","Snapdragon GSR 2","FSR 3.1"};
    // 超分输出档位（gow3.ini 的 output_res）。渲染场景尺寸由 render_resolution 分辨率补丁
    // 决定，下面的映射取自 patches/*.xml 各补丁 Note 的权威尺寸（change
    // replace-android-fsr3-with-gsr2 design D8）；补丁目录新增档位时在此同步。
    private static final String[][] SCENE_SIZES={
        {"native","1920x1080"},{"480p","854x480"},{"540p","960x540"},{"720p","1280x720"},
        {"900p","1600x900"},{"1080p","1920x1080"},{"1440p","2560x1440"},{"1800p","3200x1800"},
        {"4K","3840x2160"}};
    private static final String[] OUTPUT_VALUES={"854x480","1280x720","1920x1080"};
    private static final String[] LSFG_VALUES={"0","2","3","4"};
    private static final String[] LSFG_NAMES={"Off","2× frames","3× frames","4× frames"};
    // flow scale 取值域含 0.25 档（LSFG 链支持 0.25–1.00，见 FrameGenSettings.clampFlowScale）。
    private static final String[] LSFG_FLOW_VALUES={"0.8","1.0","0.75","0.5","0.25"};
    private static final String[] LSFG_FLOW_NAMES={"0.8 (default)","1.0","0.75","0.5","0.25 (fastest)"};
    // FPS 上限只给运行时 FPS_LIMITS 的取值（gow3_graphics.h 的 FpsLimits：30/60/120/240/0）。
    // 原来还提供 40/45，但 ParseFpsLimit 把不在表里的值一律映射成 0（不限制），等于"看着限帧、
    // 实际无限"，因此删除；取值域与 my-docs/gow3/LAUNCHER.md §3.2 及 Home 页 LaunchParams.kt 对齐。
    private static final String[] FPS_VALUES={"30","60","120","240","0"};
    private static final String[] FPS_NAMES={"30 (the game's own)","60","120","240","Unlimited"};
    /** Snapdragon 8 Gen 2: three Cortex-A510, four Cortex-A715, one Cortex-X3. */
    private static final String[] CORE_NAMES={"Core 0 · efficiency A510 2.0 GHz","Core 1 · efficiency A510 2.0 GHz",
        "Core 2 · efficiency A510 2.0 GHz","Core 3 · performance A715 2.8 GHz","Core 4 · performance A715 2.8 GHz",
        "Core 5 · performance A715 2.8 GHz","Core 6 · performance A715 2.8 GHz","Core 7 · prime X3 3.2 GHz"};
    private static final String[] LOD_VALUES={"-2","0","1","2"};
    private static final String[] LOD_NAMES={"Highest","Game default","Lower","Lowest"};

    private final Activity activity;
    private final File file;
    private final Runnable restart;
    private final Map<String,String> values=new LinkedHashMap<>();

    SettingsDialog(Activity activity,File file,Runnable restart) {
        this.activity=activity; this.file=file; this.restart=restart;
    }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private String get(String key,String fallback) { String v=values.get(key); return v==null || v.isEmpty() ? fallback : v; }

    /** 装载 gow3.ini 的当前值：解析与写入统一走 Gow3Settings，本类不再自带实现。 */
    private void load() {
        values.clear();
        values.putAll(Gow3Settings.read(file));
    }
    private TextView heading(LinearLayout box,String title) {
        TextView t=new TextView(activity); t.setText(title); t.setTextSize(15);
        t.setTypeface(Typeface.DEFAULT,Typeface.BOLD); t.setPadding(0,dp(14),0,dp(4));
        box.addView(t); return t;
    }
    private Switch toggle(LinearLayout box,String title,boolean on) {
        Switch s=new Switch(activity); s.setText(title); s.setChecked(on); s.setTextSize(15);
        s.setPadding(0,dp(6),0,dp(6)); box.addView(s); return s;
    }
    private RadioGroup choices(LinearLayout box,String[] names,int selected) {
        RadioGroup g=new RadioGroup(activity);
        for (int i=0;i<names.length;++i) {
            RadioButton b=new RadioButton(activity); b.setId(View.generateViewId()); b.setText(names[i]); b.setTextSize(15);
            g.addView(b); if (i==selected) g.check(b.getId());
        }
        box.addView(g); return g;
    }
    private static int indexOf(String[] values,String value,int fallback) {
        for (int i=0;i<values.length;++i) if (values[i].equals(value)) return i;
        return fallback;
    }
    /** A taskset list ("0-2,5", "3-7"); empty: every core. */
    private static boolean[] parseCpus(String list,int count) {
        boolean[] on=new boolean[count];
        if (list.trim().isEmpty()) { java.util.Arrays.fill(on,true); return on; }
        for (String part:list.split(",")) {
            try {
                String[] range=part.trim().split("-");
                int from=Integer.parseInt(range[0].trim()), to=range.length>1 ? Integer.parseInt(range[1].trim()) : from;
                for (int i=Math.max(0,from);i<=Math.min(count-1,to);++i) on[i]=true;
            } catch (NumberFormatException ignored) {}
        }
        boolean any=false; for (boolean b:on) any|=b;
        if (!any) java.util.Arrays.fill(on,true);
        return on;
    }
    private static String cpuList(Switch[] cores) {
        StringBuilder list=new StringBuilder(); boolean all=true;
        for (int i=0;i<cores.length;++i) {
            if (!cores[i].isChecked()) { all=false; continue; }
            if (list.length()>0) list.append(',');
            list.append(i);
        }
        return all ? "" : list.toString();
    }
    private static int checkedIndex(RadioGroup g) {
        for (int i=0;i<g.getChildCount();++i) if (((RadioButton)g.getChildAt(i)).isChecked()) return i;
        return -1;
    }

    /** render_resolution 档位的场景渲染尺寸（补丁 Note 权威值）；未知档位按 native。 */
    private static String sceneSize(String resolution) {
        for (String[] entry : SCENE_SIZES) if (entry[0].equals(resolution)) return entry[1];
        return SCENE_SIZES[0][1];
    }
    private static int[] wh(String size) {
        String[] parts = size.split("x");
        try { return new int[]{Integer.parseInt(parts[0]), Integer.parseInt(parts[1])}; }
        catch (Exception ex) { return new int[]{0, 0}; }
    }
    /** 输出档位在宽高两个方向都严格大于场景尺寸时才能做超分输出（design D8）。 */
    private static boolean greater(String candidate, String scene) {
        int[] c = wh(candidate), s = wh(scene);
        return c[0] > s[0] && c[1] > s[1];
    }
    private static String sizeLabel(String size) {
        int[] wh = wh(size);
        return wh[0] + "×" + wh[1];
    }

    void show() {
        load();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));

        // 升采样入口只提供 off / gsr2 / fsr3，与设置页 LaunchParams 同域（design D12），
        // 默认 off（超分默认关闭，GSR 2 / FSR 3.1 为可选项）。运行时的 UPSCALERS 还有
        // fsr4、fsr411、taa、dlss（gow3_settings.cpp 的 UpscalerName），此处刻意不给：
        // FSR 4 / 4.1.1 在 Adreno 740 上黑屏且极慢；taa 需要 native guest 目标、dlss 需要
        // 桌面 N 卡 DLL。未知值回落默认 off。
        heading(box,"Upscaling");
        final RadioGroup upscalers=choices(box,UPSCALER_NAMES,
            indexOf(UPSCALER_VALUES,get("upscaler","off"),0));

        heading(box,"Upscaled output");
        // 只呈现宽高都严格大于当前渲染补丁场景尺寸的输出档位；一个都没有（native/1080p 及
        // 更高补丁）时不渲染单选组，如实说明。当前 output_res 不在可用集时回落显示第一档，
        // 只在 Apply 时写盘。
        final String scene=sceneSize(get("render_resolution","native"));
        final java.util.List<String> available=new ArrayList<>();
        for (String candidate : OUTPUT_VALUES) if (greater(candidate,scene)) available.add(candidate);
        final TextView outputNote=new TextView(activity); outputNote.setTextSize(13); outputNote.setAlpha(0.7f);
        box.addView(outputNote);
        final RadioGroup outputs;
        if (available.isEmpty()) {
            outputs=null;
            outputNote.setText("The render resolution patch renders at "+sizeLabel(scene)+
                " or higher: there is nothing left to upscale to.");
        } else {
            String[] names=new String[available.size()];
            for (int i=0;i<available.size();++i) names[i]=sizeLabel(available.get(i));
            outputs=choices(box,names,0);
            String current=get("output_res","1280x720");
            int selection=available.indexOf(current);
            if (selection<0) selection=0;
            ((RadioButton)outputs.getChildAt(selection)).setChecked(true);
            if (available.indexOf(current)<0 && !"1280x720".equals(current) && !current.isEmpty())
                outputNote.setText("The saved output "+sizeLabel(current)+
                    " is not usable with this render resolution.");
        }
        RadioGroup.OnCheckedChangeListener upscaling=(g,id) -> {
            boolean on=checkedIndex(upscalers)!=0;
            if (outputs!=null) for (int i=0;i<outputs.getChildCount();++i) outputs.getChildAt(i).setEnabled(on);
            if (outputs==null) return;
            outputNote.setText(on
                ? "The render patch draws at "+sizeLabel(scene)+"; the upscaler fills the output above."
                : "Upscaling is off: the picture is presented without reconstruction.");
        };
        upscalers.setOnCheckedChangeListener(upscaling); upscaling.onCheckedChanged(upscalers,0);
        // FSR 3.1 frame generation (frame_generation=1) is not wired: shadps4 has no
        // GOW3_FRAME_GEN consumer and no FSR 3.1 frame-gen patch on this tree, so the
        // option would have been a no-op. LSFG is the only frame-generation path here;
        // tune its multiplier / flow scale / performance mode from the in-game drawer.

        heading(box,"Image");
        final Switch sharpen=toggle(box,"Sharpening","1".equals(get("sharpen","0")));
        final TextView sharpLabel=new TextView(activity); sharpLabel.setTextSize(14); box.addView(sharpLabel);
        final SeekBar sharpness=new SeekBar(activity); sharpness.setMax(100);
        float initial=0.5f;
        try { initial=Float.parseFloat(get("sharpness","0.5")); } catch (NumberFormatException ignored) {}
        sharpness.setProgress(Math.round(Math.max(0,Math.min(1,initial))*100));
        SeekBar.OnSeekBarChangeListener sharpText=new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s,int p,boolean user) { sharpLabel.setText("Sharpness: "+p+"%"); }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        };
        sharpness.setOnSeekBarChangeListener(sharpText); sharpText.onProgressChanged(sharpness,sharpness.getProgress(),false);
        box.addView(sharpness);
        sharpen.setOnCheckedChangeListener((b,on) -> sharpness.setEnabled(on)); sharpness.setEnabled(sharpen.isChecked());
        // 引擎自带的「Show FPS」（gow3.ini 的 show_fps）已退役：帧率显示统一由
        // launcher 侧性能 HUD（PerfHudOverlay）提供，历史残留的 show_fps=1 由
        // Gow3Data.prepare 在数据准备时归零，这里不再提供开关入口（design D7）。

        // FPS 上限入口：取值域必须与运行时 FPS_LIMITS（gow3_graphics.h 的 FpsLimits：30/60/120/
        // 240/0）完全一致——ParseFpsLimit 把表外的值（例如旧版提供的 40/45）一律当成 0（不限制），
        // 给出这类选项等于"看着限帧、实际无限"，所以已删除。另外 120/240 还依赖引擎帧率补丁
        // （gow3.ini 的 engine_fps），本弹窗不写 engine_fps，实际帧率仍受 PresentLimit 的引擎
        // 速率约束；Home 页 LaunchParams.kt 的 fps_limit 同此。
        heading(box,"FPS limit");
        final RadioGroup fpsLimit=choices(box,FPS_NAMES,indexOf(FPS_VALUES,get("fps_limit","30"),0));
        TextView fpsNote=new TextView(activity); fpsNote.setTextSize(13); fpsNote.setAlpha(0.7f);
        fpsNote.setText("Above 30 uses the community frame-rate patch (game timing follows the real frame time).");
        box.addView(fpsNote);

        /* 以下三段已注释掉：写入 gow3.ini 的键在本仓库的 fexdroid 启动链路里没有消费方，勾选后
         * 游戏不会有任何变化，对玩家是误导，等接通再放开（不要直接删，接通时整段取消注释即可）。
         *
         * CPU 核心 → thor_cpus（taskset 列表格式）：run-gow3.sh → patches.py --extra / launch_env.py
         * → probe 都不读它，也没有对应的 GOW3_* 环境变量；消费 taskset 的是桌面端 tools/restart.sh
         * 的 $CPUS。
        heading(box,"CPU cores the game may use");
        final Switch[] cores=new Switch[CORE_NAMES.length];
        boolean[] allowed=parseCpus(get("thor_cpus",""),CORE_NAMES.length);
        for (int i=0;i<cores.length;++i) cores[i]=toggle(box,CORE_NAMES[i],allowed[i]);
        CompoundButton.OnCheckedChangeListener keepOne=(b,on) -> {
            if (on) return;
            for (Switch c:cores) if (c.isChecked()) return;
            b.setChecked(true); // at least one core
        };
        for (Switch c:cores) c.setOnCheckedChangeListener(keepOne);
         *
         * 特效 → effect_*、模型细节 → model_lod：启动链路选补丁是按补丁名经 patches.py --extra /
         * patches.json（run.sh、run-gow3.sh），既不读这些 ini 键，gow3_settings.cpp 也不解析；
         * my-docs/gow3/LAUNCHER.md §3.2 的 GOW3 launcher 键表同样不含它们（只在 Bloodborne 参考
         * 配置 bbport-ini.md 出现）。预编译它们的 Thor 路径 build_gow3_patch_parts.py 不被
         * run-gow3.sh 使用。
        heading(box,"Game effects");
        final Switch[] effects=new Switch[EFFECTS.length];
        for (int i=0;i<EFFECTS.length;++i) effects[i]=toggle(box,EFFECTS[i][1],"1".equals(get(EFFECTS[i][0],EFFECTS[i][2])));

        heading(box,"Model detail");
        int lod=1;
        for (int i=0;i<LOD_VALUES.length;++i) if (LOD_VALUES[i].equals(get("model_lod","0")) lod=i;
        final RadioGroup lods=choices(box,LOD_NAMES,lod);
         */

        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        new AlertDialog.Builder(activity).setTitle("Graphics settings").setView(scroll)
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Apply & restart",(d,w) -> {
                Map<String,String> changed=new LinkedHashMap<>();
                changed.put("upscaler",UPSCALER_VALUES[Math.max(0,checkedIndex(upscalers))]);
                changed.put("fps_limit",FPS_VALUES[Math.max(0,checkedIndex(fpsLimit))]);
                // 超分输出：只在档位组可用时写（无可用档 = 无超分放大，不动 output_res）。
                if (outputs!=null) changed.put("output_res",available.get(Math.max(0,checkedIndex(outputs))));
                changed.put("sharpen",sharpen.isChecked() ? "1" : "0");
                changed.put("sharpness",String.format(Locale.ROOT,"%.2f",sharpness.getProgress()/100f));
                // show_fps 随引擎 FPS 退役不再写回（见上方注释）。
                // thor_cpus / effect_* / model_lod 随上面三段一起注释掉了，这里也不写。
                apply(changed);
            }).show();
    }

    /** Saves the changed keys and restarts the game when any differs from the file. */
    private void apply(Map<String,String> changed) {
        boolean same=true;
        for (Map.Entry<String,String> e:changed.entrySet()) if (!e.getValue().equals(values.get(e.getKey()))) same=false;
        if (same) return;
        if (Gow3Settings.update(file, changed)) restart.run();
        else new AlertDialog.Builder(activity).setMessage("The settings could not be saved.").setPositiveButton("OK",null).show();
    }

    /* showGame() 已注释掉（入口也随之在 GameActivity 里隐藏）：这四个 ini 键
     * （skip_network_choice / skip_intro / debug_camera / debug_menu）在本仓库的启动链路里没有
     * 消费方——run-gow3.sh 与 run.sh 都是按补丁名经 patches.py --extra / patches.json 选补丁，
     * gow3_settings.cpp 也不解析它们；预编译它们的 Thor 路径 build_gow3_patch_parts.py 不被
     * run-gow3.sh 使用。开关写进 gow3.ini 后游戏不会有任何变化，属于误导入口。
     * 等启动链路把这些键翻译成补丁名（或接入 patches.json）后再放开。
    void showGame() {
        load();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));
        final Switch[] toggles=new Switch[GAME_PATCHES.length];
        for (int i=0;i<GAME_PATCHES.length;++i) {
            toggles[i]=toggle(box,GAME_PATCHES[i][1],"1".equals(get(GAME_PATCHES[i][0],GAME_PATCHES[i][3])));
            TextView note=new TextView(activity); note.setTextSize(12); note.setAlpha(0.65f);
            note.setText(GAME_PATCHES[i][2]); note.setPadding(0,0,0,dp(6));
            box.addView(note);
        }
        TextView restartNote=new TextView(activity); restartNote.setTextSize(13); restartNote.setAlpha(0.7f);
        restartNote.setText("These are patches to the game: Apply restarts it (unsaved progress is lost).");
        restartNote.setPadding(0,dp(8),0,0);
        box.addView(restartNote);
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        new AlertDialog.Builder(activity).setTitle("Game settings").setView(scroll)
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Apply",(d,w) -> {
                Map<String,String> changed=new LinkedHashMap<>();
                for (int i=0;i<GAME_PATCHES.length;++i) changed.put(GAME_PATCHES[i][0],toggles[i].isChecked() ? "1" : "0");
                apply(changed);
            }).show();
    }
     */

    /* showCheats() 与 writeCheatState() 已注释掉（入口也随之在 GameActivity 里隐藏）：作弊在
     * Android 上根本不生效，属于误导入口。原因是 hook 根本没装上——
     * src/probe.c 的 install_cheats() 被 `#if defined(GUEST_CPU_NATIVE)` 包着，而
     * GUEST_CPU_NATIVE 只在 `__x86_64__` 时定义（src/guest_cpu.h:12）。Android 是 aarch64
     * 交叉编译（NDK/Bionic 工具链），x86-64
     * guest 跑在 FEXCore JIT 上，所以 probe.c 那段连调用带定义一起被编译掉了，源码注释写得很
     * 直接："Under the JIT ... the game runs without cheats, the health bar and the orb
     * multipliers"。gow3.ini 里的 cheat_health / cheat_item 会被 gow3_settings.cpp 读进
     * Gow3Settings::cheats[]，但没有任何 hook 去读这些 flag，等于写了没人用。
     * 附带：writeCheatState 写的 cheats.state 在本仓库也没有读取方（GOW3_CHEATS_FILE 只出现在
     * 注释里），所以界面上"运行中即时生效"那句本来也不成立。
     * 要接回来的前提是 probe.c 接上 JIT 的 hook 路径（guest_cpu_hook，注释标为 not yet wired）。
    void showCheats() {
        load();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));
        final Switch[] toggles=new Switch[CHEATS.length];
        for (int i=0;i<CHEATS.length;++i) {
            toggles[i]=toggle(box,CHEATS[i][1],"1".equals(get(CHEATS[i][0],CHEATS[i][3])));
            TextView note=new TextView(activity); note.setTextSize(12); note.setAlpha(0.65f);
            note.setText(CHEATS[i][2]); note.setPadding(0,0,0,dp(6));
            box.addView(note);
        }
        TextView restartNote=new TextView(activity); restartNote.setTextSize(13); restartNote.setAlpha(0.7f);
        restartNote.setText("For God of War III 1.09 offline play. They switch at once, while you play.");
        restartNote.setPadding(0,dp(8),0,0);
        box.addView(restartNote);
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        new AlertDialog.Builder(activity).setTitle("Cheats").setView(scroll)
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Apply",(d,w) -> {
                Map<String,String> changed=new LinkedHashMap<>();
                for (int i=0;i<CHEATS.length;++i) changed.put(CHEATS[i][0],toggles[i].isChecked() ? "1" : "0");
                // The running game follows cheats.state (probe.c reads it): no restart.
                boolean ok=save(changed) && writeCheatState(new File(file.getParentFile(),"cheats.state"),
                    toggles[0].isChecked(),toggles[1].isChecked());
                android.widget.Toast.makeText(activity,ok ? "Cheats applied" : "The cheats could not be saved",
                    android.widget.Toast.LENGTH_SHORT).show();
            }).show();
    }

    // "10": infinite health on, items off (run-gow3.sh's GOW3_CHEATS_FILE), written whole then renamed.
    private static boolean writeCheatState(File state,boolean health,boolean items) {
        File temporary=new File(state.getPath()+".tmp");
        try (Writer w=new OutputStreamWriter(new FileOutputStream(temporary),"UTF-8")) {
            w.write((health ? "1" : "0")+(items ? "1" : "0")+"\n");
        } catch (Exception ex) { android.util.Log.e("Uncharted","cheats: "+ex); return false; }
        return temporary.renameTo(state);
    }
     */

    /** Lossless Scaling frame generation (LSFG). The engine runs in the game process
     *  (libbloodframe via gow3_lsfg.cpp); this dialog writes framegen_* keys and the live
     *  state file (framegen.state, ~250ms watcher). Historical note: the old lsfg-vk layer watched
     *  it; off is multiplier 1). Turning it on when the layer was not loaded restarts the game.
     *  flow scale 已按 run-gow3.sh 的白名单补齐 0.25 档（见 LSFG_FLOW_VALUES）。这是本移植唯一的
     *  帧生成路径：FSR 3.1 帧生成（frame_generation=1）没有接通，shadps4 无 GOW3_FRAME_GEN 消费者，
     *  所以这里不要再加一个"FSR 帧生成"开关。 */
    void showLossless(java.util.function.BooleanSupplier gameRunning,Runnable onLiveApplied) {
        load();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));
        heading(box,"Frames");
        // 初值迁移：新键缺失时读旧 lsfg_multiplier（FrameGenSettings 兼容读取同规则）。
        String onInitial=get("framegen_enabled",
            "1".equals(get("lsfg_multiplier","0")) ? "1" : "0");
        String multInitial=get("framegen_multiplier",
            "0".equals(onInitial) ? "2" : get("framegen_multiplier",
                "0".equals(get("lsfg_multiplier","0")) ? "2" : get("lsfg_multiplier","2")));
        final RadioGroup lsfg=choices(box,LSFG_NAMES,indexOf(LSFG_VALUES,
            "1".equals(onInitial) ? multInitial : "0", indexOf(LSFG_VALUES,"2",0)));
        heading(box,"Flow scale");
        final RadioGroup lsfgFlow=choices(box,LSFG_FLOW_NAMES,
            indexOf(LSFG_FLOW_VALUES,get("framegen_flow_scale",
                get("lsfg_flow_scale","0.8")),0));
        TextView lsfgNote=new TextView(activity); lsfgNote.setTextSize(13); lsfgNote.setAlpha(0.7f);
        lsfgNote.setText("It sees only finished frames, so fast motion and the HUD show more "
            +"artifacts. Lower flow scale is faster. Changes apply while you play (state file, "
            +"no restart).");
        box.addView(lsfgNote);
        RadioGroup.OnCheckedChangeListener lossless=(g,id) -> {
            boolean on=checkedIndex(lsfg)>0;
            for (int i=0;i<lsfgFlow.getChildCount();++i) lsfgFlow.getChildAt(i).setEnabled(on);
        };
        lsfg.setOnCheckedChangeListener(lossless); lossless.onCheckedChanged(lsfg,0);

        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        new AlertDialog.Builder(activity).setTitle("Lossless Scaling frame generation").setView(scroll)
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Apply",(d,w) -> {
                String multiplier=LSFG_VALUES[Math.max(0,checkedIndex(lsfg))];
                String flow=LSFG_FLOW_VALUES[Math.max(0,checkedIndex(lsfgFlow))];
                boolean on=!"0".equals(multiplier);
                Map<String,String> changed=new LinkedHashMap<>();
                changed.put("framegen_enabled",on ? "1" : "0");
                changed.put("framegen_multiplier",on ? multiplier : "2");
                changed.put("framegen_flow_scale",flow);
                // FIFO 前提：游戏侧 multiplier≥2 时自行强制 FIFO（协议 MODE），这里同步
                // 写 vsync=1 保持配置一致（design D6）。
                if (on && !"1".equals(get("vsync","1"))) changed.put("vsync","1");
                if (!Gow3Settings.update(file, changed)) {
                    new AlertDialog.Builder(activity).setMessage("The settings could not be saved.").setPositiveButton("OK",null).show();
                    return;
                }
                // 运行期通道：重写状态文件（gow3_lsfg_state watcher ~250ms 热应用，不重启）。
                // 未运行时下次启动由 run-bionic.sh 从 ini 播种同名状态文件。
                java.io.File state=new java.io.File(file.getParentFile(),"framegen.state");
                java.io.File tmp=new java.io.File(file.getParentFile(),"framegen.state.tmp");
                try (java.io.Writer wr=new java.io.OutputStreamWriter(
                        new java.io.FileOutputStream(tmp),"UTF-8")) {
                    wr.write("multiplier="+(on ? multiplier : "0")+"\n");
                    wr.write("flow="+flow+"\n");
                    wr.write("performance=1\nfp16=1\n");
                } catch (Exception ex) {
                    android.util.Log.e("Uncharted","framegen state: "+ex);
                }
                boolean ok=tmp.renameTo(state);
                android.widget.Toast.makeText(activity,
                    ok ? "Lossless Scaling: applied" : "Lossless Scaling: could not apply",
                    android.widget.Toast.LENGTH_SHORT).show();
                if (ok && on && onLiveApplied!=null) onLiveApplied.run();
            }).show();
    }


}
