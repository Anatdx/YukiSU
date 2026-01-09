package com.anatdx.yukisu.ui.shizuku;

import android.app.AlertDialog;
import android.app.Activity;
import android.content.DialogInterface;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

import rikka.shizuku.Shizuku;
import rikka.shizuku.ShizukuApiConstants;

// 需要在 AndroidManifest.xml 中注册此 Activity
// theme 建议为 @style/Theme.AppCompat.Dialog.Alert 或者透明主题
public class RequestPermissionActivity extends Activity {
    private static final String TAG = "YukiSU_RequestPerm";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int uid = getIntent().getIntExtra("uid", -1);
        int pid = getIntent().getIntExtra("pid", -1);
        int requestCode = getIntent().getIntExtra("request_code", 0);

        if (uid == -1) {
            finish();
            return;
        }

        String label = String.valueOf(uid);
        PackageManager pm = getPackageManager();
        String[] pkgs = pm.getPackagesForUid(uid);
        if (pkgs != null && pkgs.length > 0) {
            try {
                ApplicationInfo ai = pm.getApplicationInfo(pkgs[0], 0);
                label = ai.loadLabel(pm).toString();
            } catch (PackageManager.NameNotFoundException e) {
                label = pkgs[0];
            }
        }

        new AlertDialog.Builder(this)
                .setTitle("Shizuku 权限请求")
                .setMessage("应用 " + label + " (uid=" + uid + ") 正在请求使用 Shizuku 权限。\n警告：授予权限将允许该应用以 Root 或 ADB 身份执行任何命令。")
                .setPositiveButton("允许 (总是)", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        grantPermission(uid, requestCode, true);
                        finish();
                    }
                })
                .setNegativeButton("拒绝", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        grantPermission(uid, requestCode, false);
                        finish();
                    }
                })
                .setCancelable(false)
                .show();
    }

    private void grantPermission(int uid, int requestCode, boolean allow) {
        try {
            // 通过 Shizuku API (实际上是 Binder 调用到 ksud) 更新权限标志
            // 标志参考 Sui/Shizuku 常量
            // ShizukuApiConstants.MASK_PERMISSION = 4 (1<<2)
            // ShizukuApiConstants.FLAG_ALLOWED = 8 (1<<3)
            
            // 注意：这里我们通过 Shizuku.updateFlagsForUid 调用 ksud
            // 前提是 YukiSU Manager 自身必须已经连接并拥有管理权限
            
            int mask = 4; // MASK_PERMISSION
            int value = allow ? 8 : 0; // FLAG_ALLOWED
            
            Log.i(TAG, "Updating flags for uid " + uid + ": allow=" + allow);
            Shizuku.updateFlagsForUid(uid, mask, value);
            
            Toast.makeText(this, allow ? "已授权" : "已拒绝", Toast.LENGTH_SHORT).show();
            
        } catch (Exception e) {
            Log.e(TAG, "Failed to update flags", e);
            Toast.makeText(this, "授权失败: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }
}
