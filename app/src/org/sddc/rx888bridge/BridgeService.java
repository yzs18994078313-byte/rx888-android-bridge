package org.sddc.rx888bridge;

import android.app.*;
import android.os.*;
import android.content.*;
import android.content.pm.ServiceInfo;
import java.io.*;
import java.lang.Process;
import java.nio.charset.StandardCharsets;

public class BridgeService extends Service {
    public static volatile boolean running = false;
    private volatile boolean wanted = false;
    private volatile Process child;
    private PowerManager.WakeLock wake;
    private NotificationManager notifications;
    private static final String CHANNEL = "rx888_receiver";
    public IBinder onBind(Intent i) { return null; }
    public static void copyFirmware(Context c) throws IOException {
        File file=new File(c.getFilesDir(),"SDDC_FX3.img");
        try(InputStream in=c.getAssets().open("SDDC_FX3.img");OutputStream out=new FileOutputStream(file)){
            byte[] bytes=new byte[32768];int n;while((n=in.read(bytes))!=-1)out.write(bytes,0,n);
        }
    }
    private Notification notification(String message) {
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, BridgeService.class).setAction("STOP"), PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.ic_menu_compass).setContentTitle("RX888 MKII 本机桥接").setContentText(message).setContentIntent(open).setOngoing(true).addAction(new Notification.Action.Builder(null, "停止", stop).build()).build();
    }
    private void state(String s) { getSharedPreferences("bridge", 0).edit().putString("state", s).apply(); if (running) notifications.notify(1, notification(s)); }
    private synchronized void log(String line) {
        File file = new File(getFilesDir(), "bridge.log");
        try (OutputStream out = new FileOutputStream(file, file.length() < 1024 * 1024)) { out.write((line + "\n").getBytes(StandardCharsets.UTF_8)); } catch (IOException ignored) { }
    }
    public int onStartCommand(Intent i, int flags, int id) {
        if (i == null || "STOP".equals(i.getAction())) {
            if (running) { wanted = false; sendStop(); state("正在停止接收进程"); } else stopSelf();
            return START_NOT_STICKY;
        }
        if (running) return START_NOT_STICKY;
        notifications = getSystemService(NotificationManager.class); notifications.createNotificationChannel(new NotificationChannel(CHANNEL, "RX888 接收服务", NotificationManager.IMPORTANCE_LOW));
        running = true; wanted = true;
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, notification("正在初始化"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE); else startForeground(1, notification("正在初始化"));
        wake = ((PowerManager)getSystemService(POWER_SERVICE)).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "rx888bridge:receiver"); wake.acquire();
        new Thread(() -> runReceiver(i), "rx888-backend").start(); return START_NOT_STICKY;
    }
    private static String quote(String s) { return "'" + s.replace("'", "'\\''") + "'"; }
    private void runReceiver(Intent i) {
        try {
            new FileOutputStream(new File(getFilesDir(), "bridge.log")).close(); copyFirmware(this);
            String libs = getApplicationInfo().nativeLibraryDir, binary = libs + "/librx888_bridge.so";
            String images = new File(getFilesDir(), "SDDC_FX3.img").getAbsolutePath(); boolean fake = i.getBooleanExtra("synthetic", false);
            String command = "export LD_LIBRARY_PATH=" + quote(libs) + "; exec " + quote(binary) + " --managed --firmware " + quote(images) + " --port " + i.getIntExtra("port", 5557) + " --frequency " + i.getLongExtra("frequency", 137900000) + " --gain " + i.getIntExtra("gain", 30) + " --max-rate " + 4000000 + (fake ? " --synthetic" : "");
            state(fake ? "正在启动测试信号" : "正在请求 root 并初始化 RX888 MKII"); log(fake ? "TEST MODE: 无需电台；用于核对本机 SpyServer 连接" : "正在启动 root 接收进程；请在 root 管理器中授权");
            if (!wanted) return;
            child = new ProcessBuilder(fake ? new String[]{"/system/bin/sh", "-c", command} : new String[]{"su", "-c", command}).redirectErrorStream(true).start();
            if (!wanted) sendStop();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(child.getInputStream(), StandardCharsets.UTF_8))) {
                String line; while ((line = reader.readLine()) != null) { log(line); if (line.startsWith("READY")) state("就绪 · 127.0.0.1:" + i.getIntExtra("port", 5557) + " · 16-bit IQ"); if (line.startsWith("FATAL")) state(line); }
            }
            int exit = child.waitFor(); log("后端退出码：" + exit); state(exit == 0 ? "服务已停止" : "启动或接收失败，请查看日志（退出码 " + exit + "）");
        } catch (Exception e) { log("ERROR: " + e); state("启动失败：" + e.getMessage()); }
        finally { stopSelf(); }
    }
    private void sendStop() { Process p = child; if (p != null) try { p.getOutputStream().write("STOP\n".getBytes(StandardCharsets.US_ASCII)); p.getOutputStream().flush(); p.getOutputStream().close(); } catch (IOException ignored) { } }
    public void onDestroy() {
        wanted = false; sendStop(); running = false;
        if (wake != null && wake.isHeld()) wake.release(); stopForeground(STOP_FOREGROUND_REMOVE); super.onDestroy();
    }
}
