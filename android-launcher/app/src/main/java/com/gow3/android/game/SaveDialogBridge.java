package com.gow3.android.game;

import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.concurrent.atomic.AtomicBoolean;

/** 存档/读档/删除列表的原生呈现（GOW3_SAVE_DIALOG_FILE）：运行时把请求写在文件里，这里用
 *  原生 AlertDialog 呈现（系统字体显示 CJK、条目带缩略图），选完把索引写回 .result。
 *  独立组件：只通过 SaveDialogHost 拿它需要的回调，不依赖 GameActivity 内部实现。 */
final class SaveDialogBridge {
    private final SaveDialogHost host;
    private final File base;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicBoolean answered = new AtomicBoolean();
    private boolean open, polling;
    /** stop() 触发的 dismiss 会把 pad 恢复为中性态而不是打开前快照（见 stop()）。 */
    private boolean stopping;
    private AlertDialog dialog;
    private android.widget.BaseAdapter adapter;
    private ListView list;
    private int selected, count;
    private java.util.Set<String> padSeen = new java.util.HashSet<>();
    private Bitmap[] thumbs;

    SaveDialogBridge(SaveDialogHost host, File base) { this.host=host; this.base=base; }

    /** 挂到与 IME 轮询相同的生命周期上（attach() 里调用）；内部每 250ms 自我续期。 */
    void start() {
        if (polling) return;
        polling=true;
        main.postDelayed(this::poll,250);
    }

    /** onDestroy 调用：摘掉轮询回调，关掉还在显示的对话框。此时 pad 恢复为中性态而不是
     *  打开前快照——Activity 正在拆卸，游戏进程可能还活着，不该把对话框前的按键还给它。 */
    void stop() {
        polling=false;
        main.removeCallbacksAndMessages(null);
        if (dialog!=null) {
            stopping=true;
            dialog.dismiss();
        }
    }

    /** 对话框是否打开；onBackPressed 用它决定 Back 是关对话框还是开抽屉。 */
    boolean isOpen() { return open; }

    /** onBackPressed 的取消入口。 */
    void cancel() { answer(false,-1); }

    /** 轮询请求文件（与 pollIme 同构）：请求文件被删即作为"已接手"的握手信号（design D2）。 */
    private void poll() {
        if (host.isHostDestroyed()) return;
        File request=new File(base,"save-dialog.request");
        if (!open && request.isFile()) {
            try {
                try (BufferedReader r=new BufferedReader(new InputStreamReader(new java.io.FileInputStream(request),"UTF-8"))) {
                    String handshake=r.readLine();
                    if (!"ok".equals(handshake)) throw new java.io.IOException("bad handshake");
                    int type=Integer.parseInt(r.readLine().trim());
                    int count=Integer.parseInt(r.readLine().trim());
                    String[] titles=new String[count], subs=new String[count], dates=new String[count];
                    for (int i=0;i<count;++i) {
                        titles[i]=r.readLine(); subs[i]=r.readLine(); dates[i]=r.readLine();
                    }
                    request.delete(); /* 握手：删请求 = 已接手 */
                    show(type,titles,subs,dates);
                }
            } catch (Exception ex) {
                android.util.Log.e("Uncharted","save list request: "+ex);
                request.delete();
                answered.set(false);
                answer(false,-1); /* 解析失败也回 cancel，不能把运行时挂在应答上 */
            }
        }
        if (open) pollPad();
        main.postDelayed(this::poll,250);
    }

    private void show(int type,String[] titles,String[] subs,String[] dates) {
        open=true; answered.set(false);
        String snapshot=host.pad().snapshot();
        host.pad().setDialogActive(true); host.pad().reset(); host.closeDrawer();
        padSeen=new java.util.HashSet<>();
        selected=0; count=titles.length;
        // 预解码缩略图：<path>.p<索引> 是运行时按原始 PNG 字节写的小文件（design D1）。
        thumbs=new Bitmap[count];
        boolean anyImage=false;
        for (int i=0;i<count;++i) {
            File pic=new File(base,"save-dialog.request.p"+i);
            if (pic.isFile()) {
                Bitmap b=BitmapFactory.decodeFile(pic.getPath());
                if (b!=null) { thumbs[i]=b; anyImage=true; }
            }
        }
        // 任一条目有图时所有条目都预留图片位，保持对齐（与 overlay ChoiceBox 一致，design D7）。
        final boolean any=anyImage;
        list=new ListView(host.context());
        adapter=new android.widget.BaseAdapter() {
            @Override public int getCount(){return count;}
            @Override public Object getItem(int p){return p;}
            @Override public long getItemId(int p){return p;}
            @Override public View getView(int p,View v,ViewGroup parent) {
                LinearLayout row=new LinearLayout(host.context());
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(host.dp(12),host.dp(8),host.dp(12),host.dp(8));
                row.setBackgroundResource(android.R.drawable.list_selector_background);
                if (any) {
                    ImageView img=new ImageView(host.context());
                    img.setImageBitmap(thumbs[p]);
                    img.setAdjustViewBounds(true);
                    row.addView(img,new LinearLayout.LayoutParams(host.dp(76),host.dp(43)));
                }
                LinearLayout col=new LinearLayout(host.context());
                col.setOrientation(LinearLayout.VERTICAL);
                col.setPadding(any?host.dp(10):0,0,0,0);
                TextView title=new TextView(host.context()); title.setTextSize(15);
                title.setTextColor(0xffe8e8e8); title.setSingleLine(true);
                title.setEllipsize(TextUtils.TruncateAt.END);
                TextView sub=new TextView(host.context()); sub.setTextSize(12);
                sub.setTextColor(0xffb0b0b0); sub.setSingleLine(true);
                sub.setEllipsize(TextUtils.TruncateAt.END);
                TextView date=new TextView(host.context()); date.setTextSize(12);
                date.setTextColor(0xff909090); date.setSingleLine(true);
                title.setText(titles[p]); sub.setText(subs[p]); date.setText(dates[p]);
                col.addView(title); col.addView(sub); col.addView(date);
                row.addView(col,new LinearLayout.LayoutParams(0,LinearLayout.LayoutParams.WRAP_CONTENT,1));
                row.setSelected(p==selected);
                return row;
            }
        };
        list.setAdapter(adapter);
        list.setOnItemClickListener((a,v,p,id) -> answer(true,p));
        dialog=new AlertDialog.Builder(host.context())
            .setTitle(type==1?"Save game":type==2?"Load game":"Delete save")
            .setView(list)
            .setNegativeButton("Cancel",(d,w) -> answer(false,-1))
            .setOnCancelListener(d -> answer(false,-1)).create();
        dialog.setOnDismissListener(d -> {
            open=false;
            host.pad().setDialogActive(false);
            if (stopping) host.pad().reset(); else host.pad().restore(snapshot);
            host.immersive(); host.requestFocus();
            if (list!=null) list.setAdapter(null);
            if (thumbs!=null) for (Bitmap b:thumbs) if (b!=null) b.recycle();
            thumbs=null;
        });
        dialog.show();
    }

    private void move(int delta) {
        if (count<=0) return;
        selected=(selected+delta+count)%count;
        if (adapter!=null) adapter.notifyDataSetChanged();
        if (list!=null) list.smoothScrollToPosition(selected);
    }

    /** 对话框导航：读 PadBridge 内存状态（物理手柄 + 屏幕手柄的并集），只对"新出现的"键做
     *  边沿判定，与 overlay 的 PadEdges 同语义——同一个键只移动/关闭一次，触摸与手柄同时
     *  表达同一动作也不重复计入（spec「同时到达的同一动作只计一次」）。 */
    private void pollPad() {
        java.util.Set<String> now=new java.util.HashSet<>(host.pad().currentButtons());
        java.util.Set<String> fresh=new java.util.HashSet<>(now);
        fresh.removeAll(padSeen);
        padSeen=now;
        if (fresh.contains("up")) move(-1);
        if (fresh.contains("down")) move(1);
        if (fresh.contains("cross")) answer(true,selected);
        if (fresh.contains("circle")) answer(false,-1);
    }

    private void answer(boolean ok,int index) {
        if (!answered.compareAndSet(false,true)) return;
        File temporary=new File(base,"save-dialog.request.result.tmp"), result=new File(base,"save-dialog.request.result");
        boolean wrote=false;
        try (java.io.Writer w=new java.io.OutputStreamWriter(new java.io.FileOutputStream(temporary),"UTF-8")) {
            w.write(ok ? "ok "+index+"\n" : "cancel\n");
            wrote=temporary.renameTo(result);
        } catch (Exception ex) { android.util.Log.e("Uncharted","save answer: "+ex); }
        if (!wrote) {
            /* 写失败也 dismiss：不能让对话框卡死（CAS 已置位，再点 Cancel 也会被挡）。
             * 运行时由 save_list_timeout 兜底，不会永久等。 */
            android.util.Log.e("Uncharted","save answer: write/rename failed");
            answered.set(false);
            if (dialog!=null) dialog.dismiss();
            return;
        }
        if (dialog!=null) dialog.dismiss();
    }
}
