package org.sddc.rx888bridge;

import android.app.*;
import android.os.*;
import android.content.*;
import android.graphics.Color;
import android.text.InputType;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

public class MainActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private LinearLayout panel;
    private EditText port, frequency, gain;
    private CheckBox synthetic;
    private TextView state, logs, diagnostics;
    private String logSnapshot = "";
    private final Runnable refresh = new Runnable() { public void run() { updateStatus(); handler.postDelayed(this, 1000); } };
    private int dp(int v) { return (int)(getResources().getDisplayMetrics().density * v); }
    private TextView text(String s, int size, int color) {
        TextView t = new TextView(this); t.setText(s); t.setTextSize(size); t.setTextColor(color); t.setPadding(0, dp(7), 0, dp(7));
        panel.addView(t, new LinearLayout.LayoutParams(-1, -2)); return t;
    }
    private EditText field(String label, String value, boolean decimal) {
        text(label, 14, 0xffa9b9cc); EditText e = new EditText(this); e.setSingleLine(true); e.setText(value); e.setTextColor(Color.WHITE);
        e.setInputType(InputType.TYPE_CLASS_NUMBER | (decimal ? InputType.TYPE_NUMBER_FLAG_DECIMAL : 0));
        panel.addView(e, new LinearLayout.LayoutParams(-1, -2)); return e;
    }
    private void button(String name, View.OnClickListener listener) {
        Button b = new Button(this); b.setText(name); b.setAllCaps(false); b.setOnClickListener(listener); panel.addView(b, new LinearLayout.LayoutParams(-1, dp(52)));
    }
    public void onCreate(Bundle saved) {
        super.onCreate(saved); getWindow().setStatusBarColor(0xff101a29); getWindow().setNavigationBarColor(0xff101a29);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
        ScrollView scroll = new ScrollView(this); scroll.setFillViewport(true); scroll.setBackgroundColor(0xff101a29);
        panel = new LinearLayout(this); panel.setOrientation(LinearLayout.VERTICAL); panel.setFocusableInTouchMode(true); panel.setPadding(dp(22), dp(24), dp(22), dp(30)); scroll.addView(panel); setContentView(scroll);
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            v.setPadding(0, insets.getSystemWindowInsetTop(), 0, insets.getSystemWindowInsetBottom()); return insets;
        });
        text("RX888 MKII 本机桥接", 28, Color.WHITE);
        text("1.0.0 正式版 · USB → DDC → 本机 SpyServer", 14, 0xff6ee7c6);
        text("接收模式使用 root 和 USB 3；请确保 USB 供电充足，看到 READY 后再连接 OnAir。", 14, 0xffbec9d9);
        state = text("尚未启动", 18, 0xff6ee7c6);
        diagnostics = text("接收统计：连接并接收后每 5 秒更新", 13, 0xffbec9d9);
        android.content.SharedPreferences settings = getSharedPreferences("bridge", 0);
        port = field("本机端口", settings.getString("port", "5557"), false);
        frequency = field("初始频率（MHz，连接后由 OnAir 调谐）", settings.getString("frequency", "137.9"), true);
        gain = field("增益（0–49；VHF 为 RF 增益 dB）", settings.getString("gain", "30"), false);
        text("IQ 采样率：0.25 / 0.5 / 1 / 2 / 4 MS/s，初始 1 MS/s，由 OnAir 选择。VHF 的 ADC 输入为 16 MS/s。", 14, 0xffbec9d9);
        text("低于 32 MHz 使用 HF 口；32 MHz 及以上使用 VHF 口。", 14, 0xffbec9d9);
        synthetic = new CheckBox(this); synthetic.setText("内置测试信号（无需电台、无需 root）"); synthetic.setTextColor(Color.WHITE); panel.addView(synthetic);
        button("启动本机服务", v -> startBridge());
        button("停止服务", v -> startService(new Intent(this, BridgeService.class).setAction("STOP")));
        text("OnAir 连接", 19, Color.WHITE);
        text("网络电台 → SpyServer\n地址 127.0.0.1，端口与上方一致\n服务强制发送 16-bit IQ；连接后可在 OnAir 调频、选采样率和调整增益。看到 READY 后再连接。", 14, 0xffbec9d9);
        text("运行日志", 19, Color.WHITE);
        button("导出完整运行日志", v -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain").addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE, "RX888-Bridge-1.0.0.log"); startActivityForResult(i, 102);
        });
        logs = text("", 12, 0xffa9b9cc); logs.setTypeface(android.graphics.Typeface.MONOSPACE); logs.setTextIsSelectable(true);
        panel.requestFocus();
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != 0) requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, 11);
    }
    private void toast(String message) { Toast.makeText(this, message, Toast.LENGTH_LONG).show(); }
    private void startBridge() {
        if (BridgeService.running) { toast("服务已经在运行"); return; }
        try {
            int p = Integer.parseInt(port.getText().toString()); long f = Math.round(Double.parseDouble(frequency.getText().toString()) * 1000000); int g = Integer.parseInt(gain.getText().toString());
            if (p < 1024 || p > 65535 || f < 10000 || f > 1750000000L || g < 0 || g > 49) throw new IllegalArgumentException("端口、频率或增益超出范围");
            getSharedPreferences("bridge", 0).edit().putString("port", port.getText().toString()).putString("frequency", frequency.getText().toString()).putString("gain", gain.getText().toString()).apply();
            Intent i = new Intent(this, BridgeService.class).setAction("START").putExtra("port", p).putExtra("frequency", f).putExtra("gain", g).putExtra("rate", 4000000).putExtra("synthetic", synthetic.isChecked());
            startForegroundService(i);
        } catch (Exception e) { toast("参数错误：" + e.getMessage()); }
    }
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        if (request == 102) {
            try (InputStream in = new FileInputStream(new File(getFilesDir(), "bridge.log")); OutputStream out = getContentResolver().openOutputStream(data.getData())) {
                byte[] b = new byte[32768]; int n; while ((n = in.read(b)) != -1) out.write(b, 0, n); toast("日志已导出");
            } catch (Exception e) { toast("导出失败：" + e.getMessage()); }
            return;
        }
    }
    private void updateStatus() {
        if (state == null) return;
        state.setText(getSharedPreferences("bridge", 0).getString("state", "尚未启动"));
        File file = new File(getFilesDir(), "bridge.log"); if (!file.exists()) return;
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            long from = Math.max(0, f.length() - 24000); f.seek(from); byte[] data = new byte[(int)(f.length() - from)]; f.readFully(data);
            String snapshot = new String(data, StandardCharsets.UTF_8);
            if (!snapshot.equals(logSnapshot)) {
                logSnapshot = snapshot; logs.setText(snapshot);
                int start = snapshot.lastIndexOf("STAT:");
                if (start >= 0) {
                    int end = snapshot.indexOf('\n', start); String stat = snapshot.substring(start, end < 0 ? snapshot.length() : end);
                    diagnostics.setText("采样率：" + statValue(stat, "rate") + " S/s\nUSB 错误事件：" + statValue(stat, "usb_errors") + "\n原始 ADC 队列丢样：" + statValue(stat, "raw_dropped_samples") + "\nIQ 队列丢样：" + statValue(stat, "queue_dropped_samples") + "\n队列：" + statValue(stat, "queue") + " 块；TCP 等待：" + statValue(stat, "tcp_waits") + " 次");
                } else diagnostics.setText("接收统计：连接并接收后每 5 秒更新");
            }
        }
        catch (Exception ignored) { }
    }
    private static String statValue(String line, String name) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("(?:^| )" + name + "=([^ ]+)").matcher(line); return m.find() ? m.group(1) : "—";
    }
    protected void onResume() { super.onResume(); handler.post(refresh); }
    protected void onPause() { handler.removeCallbacks(refresh); super.onPause(); }
}
