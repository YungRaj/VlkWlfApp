package com.google.example.vkwlf;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.content.Intent;
import android.content.om.FabricatedOverlay;
import android.content.om.OverlayManager;
import android.content.om.OverlayManagerTransaction;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.text.TextUtils;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Host Android Activity embedding an in-process Wayland-to-Vulkan compositor
 * rendering directly onto a SurfaceView's ANativeWindow via vkQueuePresentKHR.
 * Supports any number (N) of simultaneous Wayland applications and native Android desktop windows.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "VkWlfApp";
    private static Process sDirectClientProcess;
    private static final Set<Integer> sActiveWindowSurfaceIds =
            Collections.synchronizedSet(new HashSet<>());
    private static final Map<String, Bitmap> sSurfaceIconCache = new ConcurrentHashMap<>();
    private static final Map<String, Bitmap> sLabelIconCache = new ConcurrentHashMap<>();
    private static final Map<String, Bitmap> sGeneratedIconCache = new ConcurrentHashMap<>();
    private static final Map<Integer, Bitmap> sFallbackIconCache = new ConcurrentHashMap<>();
    private static final Map<Integer, String> sSurfaceAppLabels = new ConcurrentHashMap<>();
    private static final Map<Integer, String> sGenericSlotOverlaySigs = new ConcurrentHashMap<>();
    private static final Map<Integer, MainActivity> sActiveSlotActivities =
            new ConcurrentHashMap<>();
    private static volatile MainActivity sPendingFinishActivity = null;
    private static volatile MainActivity sDesiredForegroundActivity = null;
    private static volatile long sSuppressFocusPromoteUntilMs = 0L;
    private static volatile long sLastTransitionStartMs = 0L;
    private static volatile int sCurrentDockLeaderTaskId = -1;
    private static final android.os.Handler sDockOrderHandler =
            new android.os.Handler(android.os.Looper.getMainLooper());
    private static volatile boolean sOneTimeHostSetupDone = false;
    private static volatile int sLastCaptionTopPx = -1;
    private static volatile int[] sLastKnownSurfaceIds = null;
    private static volatile String[] sLastKnownTitles = null;
    private static volatile String[] sLastKnownAppIds = null;

    static {
        System.loadLibrary("vkwlf_jni");
    }

    private int mWindowSlot = 0;
    private int mBoundSurfaceId = 0;
    private boolean mIsBackgroundRecycle = false;
    private SurfaceView mSurfaceView;
    private FrameLayout mRoot;
    private LinearLayout mHeader;
    private ImageView mHeaderIcon;
    private TextView mHeaderTitle;
    private HorizontalScrollView mAppSwitcherScroll;
    private LinearLayout mAppSwitcherBar;
    private int mCaptionHeightPx;
    private Bitmap mCurrentIconBitmap;
    private String mLastAppId;
    private String mLastSwitcherSignature = "";

    // Mirrors WindowInsetsController.APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND /
    // APPEARANCE_LIGHT_CAPTION_BARS (API 35) so this also compiles against older android.jar.
    private static final int APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND = 1 << 7;
    private static final int APPEARANCE_LIGHT_CAPTION_BARS = 1 << 8;

    protected int getPreferredWindowSlot() {
        return 0;
    }

    protected int getWindowSlot() {
        return mWindowSlot;
    }

    /**
     * Unregisters any stale self-targeting FabricatedOverlays from earlier runs so that
     * R.drawable.ic_app_teamviewer and R.string.app_name_teamviewer are never overwritten.
     */
    private void cleanupStaleDynamicOverlays() {
        try {
            OverlayManager om = getSystemService(OverlayManager.class);
            if (om != null) {
                String pkg = getPackageName();
                String[] staleNames = {
                    "WaylandDynamicAppMetaOverlay",
                    "WaylandDynamicAppMetaOverlaySlot0",
                    "WaylandDynamicAppMetaOverlaySlot1",
                    "WaylandDynamicAppMetaOverlaySlot2",
                    "WaylandDynamicAppMetaOverlaySlot3"
                };
                for (String name : staleNames) {
                    try {
                        OverlayManagerTransaction tx = OverlayManagerTransaction.newInstance();
                        tx.unregisterFabricatedOverlay(
                                new FabricatedOverlay(name, pkg).getIdentifier());
                        om.commit(tx);
                    } catch (Throwable ignored) {
                    }
                }
            }
            for (int i = 0; i < 4; i++) {
                File f = new File(getCacheDir(), "wayland_app_icon_slot" + i + ".png");
                if (f.exists()) {
                    f.delete();
                }
            }
        } catch (Throwable ignored) {
        }
    }

    /**
     * Stable application label for any Wayland or X11 application. Window titles change constantly
     * ("Page — Mozilla Firefox", "App - file.txt"), so we normalize known apps and extract a clean
     * human-readable application name from appId or title.
     */
    private static String deriveAppLabel(String title, String appId) {
        String combined =
                ((appId != null ? appId : "") + " " + (title != null ? title : "")).toLowerCase();
        if (combined.contains("teamviewer")) return "TeamViewer";
        if (combined.contains("antigravity")) return "Antigravity";
        if (combined.contains("firefox")) return "Firefox";
        if (combined.contains("chromium")) return "Chromium";
        if (combined.contains("google-chrome") || combined.contains("chrome")) return "Chrome";
        if (combined.contains("gimp")) return "GIMP";
        if (combined.contains("libreoffice")) return "LibreOffice";
        if (combined.contains("vlc")) return "VLC";
        if (combined.contains("code") || combined.contains("vscode")) return "VS Code";

        if (appId != null && !appId.isEmpty()) {
            String cleanId = appId.trim();
            if (cleanId.endsWith(".desktop")) {
                cleanId = cleanId.substring(0, cleanId.length() - 8);
            }
            int slash = cleanId.lastIndexOf('/');
            if (slash >= 0 && slash + 1 < cleanId.length()) {
                cleanId = cleanId.substring(slash + 1);
            }
            int dot = cleanId.lastIndexOf('.');
            if (dot >= 0 && dot + 1 < cleanId.length()) {
                cleanId = cleanId.substring(dot + 1);
            }
            cleanId = cleanId.replaceAll("(?i)-(esr|bin|wayland|x11)$", "");
            String lowerClean = cleanId.toLowerCase();
            if (!lowerClean.isEmpty()
                    && !lowerClean.equals("xwayland")
                    && !lowerClean.equals("wayland")
                    && !lowerClean.equals("wlroots")) {
                String spaced = cleanId.replace('-', ' ').replace('_', ' ').trim();
                if (!spaced.isEmpty()) {
                    return Character.toUpperCase(spaced.charAt(0)) + spaced.substring(1);
                }
            }
        }
        if (title != null && !title.isEmpty()) {
            String t = title;
            for (String sep : new String[] {" — ", " – ", " - "}) {
                int i = t.lastIndexOf(sep);
                if (i > 0 && i + sep.length() < t.length()) {
                    String suffix = t.substring(i + sep.length()).trim();
                    String prefix = t.substring(0, i).trim();
                    if (!suffix.isEmpty() && suffix.length() <= 28) {
                        t = suffix;
                    } else {
                        t = prefix;
                    }
                    break;
                }
            }
            t = t.replaceAll("\\s*\\[[^\\]]*\\]\\s*$", "").trim();
            if (!t.isEmpty()) return t;
        }
        return "Antigravity";
    }

    private int resolveFallbackIconRes(String title, String appId) {
        String combined =
                ((title != null ? title : "") + " " + (appId != null ? appId : "")).toLowerCase();
        if (combined.contains("teamviewer")) {
            return R.drawable.ic_app_teamviewer;
        }
        if (combined.contains("firefox")) {
            return R.drawable.ic_app_firefox;
        }
        if (combined.contains("vscode")
                || combined.contains("visual studio code")
                || combined.contains("vs code")) {
            return R.drawable.ic_app_vscode;
        }
        if (combined.contains("antigravity")) {
            return R.drawable.ic_app_antigravity;
        }
        switch (getPreferredWindowSlot()) {
            case 1:
                return R.drawable.ic_app_teamviewer;
            case 2:
                return R.drawable.ic_app_firefox;
            case 3:
                return R.drawable.ic_app_vscode;
            case 4:
                return R.drawable.ic_wayland_app;
            case 5:
                return R.drawable.ic_wayland_app_2;
            default:
                return R.drawable.ic_app_antigravity;
        }
    }

    protected String getDefaultActivityLabel() {
        switch (getPreferredWindowSlot()) {
            case 1:
                return getString(R.string.app_name_teamviewer);
            case 2:
                return getString(R.string.app_name_firefox);
            case 3:
                return getString(R.string.app_name_vscode);
            case 4:
                return getString(R.string.wayland_app_name);
            case 5:
                return getString(R.string.wayland_app_name_2);
            default:
                return getString(R.string.app_name_antigravity);
        }
    }

    private Class<? extends MainActivity> resolveActivityClassForSurface(
            int surfaceId, String titleOrLabel, String appId) {
        String label = deriveAppLabel(titleOrLabel, appId);
        if ((titleOrLabel == null || titleOrLabel.isEmpty())
                && (appId == null || appId.isEmpty())
                && surfaceId > 0
                && sSurfaceAppLabels.containsKey(surfaceId)) {
            label = sSurfaceAppLabels.get(surfaceId);
        }
        String low = (label != null ? label : "").toLowerCase();
        if (low.contains("teamviewer")) {
            return SecondaryWaylandActivity.class;
        }
        if (low.contains("firefox")) {
            return FirefoxWaylandActivity.class;
        }
        if (low.contains("vs code") || low.contains("vscode")) {
            return VSCodeWaylandActivity.class;
        }
        if (low.contains("antigravity")) {
            return MainActivity.class;
        }
        MainActivity existingForSid = findOpenActivityBySurface(surfaceId);
        if (existingForSid != null) {
            return existingForSid.getClass();
        }
        if ((getClass() == GenericWaylandActivity.class
                        || getClass() == GenericWaylandActivity2.class)
                && (mBoundSurfaceId == surfaceId || mBoundSurfaceId <= 0)) {
            return getClass();
        }
        if (getClass() == GenericWaylandActivity.class) {
            return GenericWaylandActivity2.class;
        }
        if (getClass() == GenericWaylandActivity2.class) {
            return GenericWaylandActivity.class;
        }
        if (!isActivityClassOpen(GenericWaylandActivity.class)) {
            return GenericWaylandActivity.class;
        }
        return GenericWaylandActivity2.class;
    }

    private void applyGenericActivityOverlayIfNeeded(
            Class<? extends MainActivity> targetClass, int sid, String label, Bitmap icon) {
        if (targetClass != GenericWaylandActivity.class
                && targetClass != GenericWaylandActivity2.class) {
            return;
        }
        if (icon == null || label == null || label.isEmpty()) return;
        final int slotNum = (targetClass == GenericWaylandActivity.class) ? 4 : 5;
        final String sig = label + "|" + sid + "|" + icon.getGenerationId();
        if (sig.equals(sGenericSlotOverlaySigs.get(slotNum))) {
            return;
        }
        try {
            OverlayManager om = getSystemService(OverlayManager.class);
            if (om == null) return;
            File iconFile = new File(getCacheDir(), "wayland_generic_slot" + slotNum + ".png");
            try (FileOutputStream fos = new FileOutputStream(iconFile)) {
                icon.compress(Bitmap.CompressFormat.PNG, 100, fos);
                fos.flush();
            }
            String pkg = getPackageName();
            String overlayName = "WaylandGenericOverlaySlot" + slotNum;
            String drawableRes =
                    (slotNum == 4)
                            ? (pkg + ":drawable/ic_wayland_app")
                            : (pkg + ":drawable/ic_wayland_app_2");
            String stringRes =
                    (slotNum == 4)
                            ? (pkg + ":string/wayland_app_name")
                            : (pkg + ":string/wayland_app_name_2");
            try (ParcelFileDescriptor pfd =
                    ParcelFileDescriptor.open(iconFile, ParcelFileDescriptor.MODE_READ_ONLY)) {
                FabricatedOverlay overlay = new FabricatedOverlay(overlayName, pkg);
                overlay.setTargetOverlayable("WaylandDynamicAppMeta");
                overlay.setResourceValue(drawableRes, pfd, null);
                overlay.setResourceValue(stringRes, TypedValue.TYPE_STRING, label, null);
                OverlayManagerTransaction tx = OverlayManagerTransaction.newInstance();
                tx.registerFabricatedOverlay(overlay);
                om.commit(tx);
                sGenericSlotOverlaySigs.put(slotNum, sig);
                Log.i(TAG, "Committed generic Wayland app icon overlay for " + label
                        + " on slot " + slotNum);
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed to commit generic Wayland overlay for " + label, t);
        }
    }

    private static Bitmap generateMonogramIconBitmap(String label) {
        String clean = (label != null && !label.trim().isEmpty()) ? label.trim() : "W";
        Bitmap existing = sGeneratedIconCache.get(clean);
        if (existing != null) {
            return existing;
        }
        final int size = 192;
        Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bmp);
        final int[] palette = {
            0xFFE65100, 0xFF1E88E5, 0xFF43A047, 0xFF8E24AA,
            0xFFD81B60, 0xFF00897B, 0xFF3949AB, 0xFFF4511E
        };
        int color = palette[Math.abs(clean.hashCode()) % palette.length];
        Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        bgPaint.setColor(color);
        RectF rect = new RectF(12f, 12f, size - 12f, size - 12f);
        canvas.drawRoundRect(rect, 42f, 42f, bgPaint);

        Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        textPaint.setColor(0xFFFFFFFF);
        textPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        textPaint.setTextSize(108f);
        textPaint.setTextAlign(Paint.Align.CENTER);
        String letter = clean.substring(0, 1).toUpperCase();
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        float centerY = (size / 2f) - ((fm.ascent + fm.descent) / 2f);
        canvas.drawText(letter, size / 2f, centerY, textPaint);

        sGeneratedIconCache.put(clean, bmp);
        return bmp;
    }

    private Bitmap getSurfaceIconBitmap(int surfaceId, String title, String appId) {
        String label = deriveAppLabel(title, appId);
        if (surfaceId > 0) {
            String cacheKey = surfaceId + "|" + label;
            Bitmap cached = sSurfaceIconCache.get(cacheKey);
            if (cached != null) {
                return cached;
            }
            try {
                byte[] png = nativeGetSurfaceIconPng(surfaceId);
                if (png != null && png.length > 0) {
                    Bitmap decoded = BitmapFactory.decodeByteArray(png, 0, png.length);
                    if (decoded != null) {
                        sSurfaceIconCache.put(cacheKey, decoded);
                        sLabelIconCache.put(label, decoded);
                        return decoded;
                    }
                }
            } catch (Throwable ignored) {
            }
        }
        Bitmap byLabel = sLabelIconCache.get(label);
        if (byLabel != null) {
            return byLabel;
        }
        String lowerLabel = label.toLowerCase();
        int resId = 0;
        if (lowerLabel.contains("teamviewer")) {
            resId = R.drawable.ic_app_teamviewer;
        } else if (lowerLabel.contains("firefox")) {
            resId = R.drawable.ic_app_firefox;
        } else if (lowerLabel.contains("vs code") || lowerLabel.contains("vscode")) {
            resId = R.drawable.ic_app_vscode;
        } else if (lowerLabel.contains("antigravity")) {
            resId = R.drawable.ic_app_antigravity;
        }
        if (resId != 0) {
            Bitmap fallback = sFallbackIconCache.get(resId);
            if (fallback == null) {
                fallback = BitmapFactory.decodeResource(getResources(), resId);
                if (fallback != null) {
                    sFallbackIconCache.put(resId, fallback);
                }
            }
            if (fallback != null) {
                return fallback;
            }
        }
        return generateMonogramIconBitmap(label);
    }

    private static MainActivity findOpenActivityBySurface(int surfaceId) {
        if (surfaceId <= 0) return null;
        for (MainActivity act : sActiveSlotActivities.values()) {
            if (act != null
                    && act != sPendingFinishActivity
                    && !act.isFinishing()
                    && !act.isDestroyed()
                    && act.mBoundSurfaceId == surfaceId) {
                return act;
            }
        }
        return null;
    }

    private static MainActivity findOtherOpenActivity(MainActivity self) {
        for (MainActivity act : sActiveSlotActivities.values()) {
            if (act != null
                    && act != self
                    && act != sPendingFinishActivity
                    && !act.isFinishing()
                    && !act.isDestroyed()) {
                return act;
            }
        }
        return null;
    }

    private static boolean isActivityClassOpen(Class<? extends MainActivity> cls) {
        for (MainActivity act : sActiveSlotActivities.values()) {
            if (act != null
                    && act != sPendingFinishActivity
                    && !act.isFinishing()
                    && !act.isDestroyed()
                    && act.getClass() == cls) {
                return true;
            }
        }
        return false;
    }

    private static MainActivity findOpenActivityByClass(Class<? extends MainActivity> cls) {
        for (MainActivity act : sActiveSlotActivities.values()) {
            if (act != null
                    && act != sPendingFinishActivity
                    && !act.isFinishing()
                    && !act.isDestroyed()
                    && act.getClass() == cls) {
                return act;
            }
        }
        return null;
    }

    protected void setTaskExcludeFromRecents(boolean exclude) {
        try {
            ActivityManager am = getSystemService(ActivityManager.class);
            if (am != null) {
                final int myTaskId = getTaskId();
                for (ActivityManager.AppTask task : am.getAppTasks()) {
                    ActivityManager.RecentTaskInfo info = task.getTaskInfo();
                    if (info != null && (info.taskId == myTaskId || info.id == myTaskId)) {
                        task.setExcludeFromRecents(exclude);
                        Log.i(TAG, "setExcludeFromRecents(" + exclude + ") for taskId=" + myTaskId
                                + " (" + getClass().getSimpleName() + ")");
                        break;
                    }
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed setExcludeFromRecents(" + exclude + ")", t);
        }
    }

    private void clearAllAppTasksExcludeFromRecents() {
        try {
            ActivityManager am = getSystemService(ActivityManager.class);
            if (am != null) {
                for (ActivityManager.AppTask task : am.getAppTasks()) {
                    task.setExcludeFromRecents(false);
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed clearAllAppTasksExcludeFromRecents", t);
        }
    }

    private void cancelPendingDockOrderAndClearExclusions() {
        sDockOrderHandler.removeCallbacksAndMessages(null);
        clearAllAppTasksExcludeFromRecents();
    }

    private void triggerRecentTasksRefresh() {
        try {
            // Calling setPictureInPictureParams invokes ActivityRecord.setPictureInPictureParams()
            // -> getTask().onPictureInPictureParamsChanged() -> Task.dispatchTaskInfoChangedIfNeeded(true)
            // on the Freeform leaf task (organized by ShellTaskOrganizer) ->
            // RecentTasksController.onTaskInfoChanged() -> RecentTasksController.notifyRecentTasksChanged()
            // synchronously with zero visual side effects and zero WindowManager transitions.
            setPictureInPictureParams(new android.app.PictureInPictureParams.Builder().build());
        } catch (Throwable t) {
            Log.w(TAG, "Failed triggerRecentTasksRefresh", t);
        }
    }

    private static void bringActivityToFront(MainActivity fg) {
        if (fg == null || fg.isFinishing() || fg.isDestroyed()) return;
        try {
            sLastTransitionStartMs = System.currentTimeMillis();
            ActivityManager am = fg.getSystemService(ActivityManager.class);
            if (am != null) {
                am.moveTaskToFront(fg.getTaskId(), ActivityManager.MOVE_TASK_NO_USER_ACTION);
            }
            fg.overridePendingTransition(0, 0);
        } catch (Throwable t) {
            Log.w(TAG, "Failed moveTaskToFront for taskId=" + fg.getTaskId(), t);
        }
    }

    /**
     * Reorders NexusLauncher's TaskbarRecentAppsController.orderedRunningTaskIds in-place so this
     * foreground window is at index 0 (updating the GoogleBook OS dock icon to match this window)
     * WITHOUT destroying, recreating, or moving any window.
     *
     * How it works:
     * 1. During any WindowManager transition, all tasks have FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS
     *    cleared so SystemUI's TrampolineLaunchTransitionObserver never closes any background task.
     * 2. Once transitions are idle, Step 1 briefly sets setExcludeFromRecents(true) on background
     *    tasks and calls setShouldDockBigOverlays(false) to trigger RecentTasksController
     *    .notifyRecentTasksChanged(), removing background taskIds from orderedRunningTaskIds and
     *    leaving [myTaskId] at index 0.
     * 3. 70ms later, Step 2 restores setExcludeFromRecents(false) on all tasks and triggers
     *    notifyRecentTasksChanged() again, re-appending background taskIds after myTaskId so
     *    orderedRunningTaskIds = [myTaskId, bgTaskId...] and all tasks remain in Recents.
     */
    private void promoteToDockLeader() {
        if (isFinishing() || isDestroyed() || this == sPendingFinishActivity) return;
        final int myTaskId = getTaskId();
        boolean hasOtherOpenWindow = false;
        for (MainActivity act : sActiveSlotActivities.values()) {
            if (act != null
                    && act != this
                    && act != sPendingFinishActivity
                    && !act.isFinishing()
                    && !act.isDestroyed()) {
                hasOtherOpenWindow = true;
                break;
            }
        }
        if (!hasOtherOpenWindow) {
            sCurrentDockLeaderTaskId = myTaskId;
            return;
        }
        if (sCurrentDockLeaderTaskId == myTaskId) {
            return;
        }
        cancelPendingDockOrderAndClearExclusions();
        long elapsedSinceTransition = System.currentTimeMillis() - sLastTransitionStartMs;
        long delayMs = (elapsedSinceTransition < 350L) ? 360L : 100L;
        final MainActivity fg = this;
        sDockOrderHandler.postDelayed(() -> {
            if (fg.isFinishing() || fg.isDestroyed() || sDesiredForegroundActivity != fg) {
                fg.clearAllAppTasksExcludeFromRecents();
                return;
            }
            try {
                ActivityManager am = fg.getSystemService(ActivityManager.class);
                if (am != null) {
                    for (ActivityManager.AppTask task : am.getAppTasks()) {
                        ActivityManager.RecentTaskInfo info = task.getTaskInfo();
                        if (info != null) {
                            int tid = (info.taskId != -1) ? info.taskId : info.id;
                            task.setExcludeFromRecents(tid != myTaskId);
                        }
                    }
                }
                fg.triggerRecentTasksRefresh();
                Log.i(TAG, "DockOrder Step 1: promoted taskId=" + myTaskId
                        + " (" + fg.getClass().getSimpleName() + ") to orderedRunningTaskIds[0]");
            } catch (Throwable t) {
                Log.w(TAG, "DockOrder Step 1 failed", t);
            }
            sDockOrderHandler.postDelayed(() -> {
                fg.clearAllAppTasksExcludeFromRecents();
                fg.triggerRecentTasksRefresh();
                sCurrentDockLeaderTaskId = myTaskId;
                Log.i(TAG, "DockOrder Step 2: restored background tasks in recents behind taskId="
                        + myTaskId);
            }, 150L);
        }, delayMs);
    }

    private void markAsForegroundTaskForDock() {
        if (isFinishing() || isDestroyed() || this == sPendingFinishActivity) return;
        sDesiredForegroundActivity = this;
        promoteToDockLeader();
    }

    private static int resolveIntentTargetSurface(Intent intent) {
        if (intent == null) return 0;
        int targetSid = intent.getIntExtra("target_surface_id", 0);
        if (targetSid <= 0 && intent.getData() != null) {
            try {
                String lastSeg = intent.getData().getLastPathSegment();
                if (lastSeg != null) {
                    targetSid = Integer.parseInt(lastSeg);
                }
            } catch (NumberFormatException ignored) {
            }
        }
        return targetSid;
    }

    @SuppressWarnings("deprecation")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
        super.onCreate(savedInstanceState);
        Intent launchIntent = getIntent();
        mIsBackgroundRecycle =
                launchIntent != null
                        && launchIntent.getBooleanExtra("is_background_recycle", false);
        mWindowSlot = nativeAllocateWindowSlot(getPreferredWindowSlot());
        sActiveSlotActivities.put(mWindowSlot, this);
        if (!sOneTimeHostSetupDone) {
            sOneTimeHostSetupDone = true;
            setupHostXdgEnvironment();
            Thread cleanupThread = new Thread(this::cleanupStaleDynamicOverlays, "VkWlfOverlayCleanup");
            cleanupThread.setDaemon(true);
            cleanupThread.start();
        }

        final int slot = mWindowSlot;
        final int initialTargetSid = resolveIntentTargetSurface(getIntent());
        final String defaultLabel = getDefaultActivityLabel();
        final String initialLabel =
                (initialTargetSid > 0 && sSurfaceAppLabels.containsKey(initialTargetSid))
                        ? sSurfaceAppLabels.get(initialTargetSid)
                        : defaultLabel;
        final Bitmap initialIcon = getSurfaceIconBitmap(initialTargetSid, initialLabel, initialLabel);
        setTitle(initialLabel);
        if (initialIcon != null) {
            mCurrentIconBitmap = initialIcon;
            applyGenericActivityOverlayIfNeeded(
                    getClass(), initialTargetSid, initialLabel, initialIcon);
            setTaskDescription(new ActivityManager.TaskDescription(initialLabel, initialIcon));
        } else {
            setTaskDescription(new ActivityManager.TaskDescription(initialLabel));
        }

        final float dp = getResources().getDisplayMetrics().density;
        final int barHeightPx = (int) (38 * dp);
        final int topOffsetPx = (sLastCaptionTopPx >= 0) ? sLastCaptionTopPx : (int) (40 * dp);
        mCaptionHeightPx = topOffsetPx + barHeightPx;

        mSurfaceView = new SurfaceView(this);
        mSurfaceView.setFocusable(true);
        mSurfaceView.setFocusableInTouchMode(true);
        mSurfaceView.getHolder().addCallback(this);
        mSurfaceView.setOnTouchListener((v, event) -> nativeOnMotionEventForSlot(
                slot,
                event.getActionMasked(),
                event.getX(),
                event.getY(),
                event.getButtonState(),
                0.0f,
                0.0f,
                event.getEventTime()));
        mSurfaceView.setOnGenericMotionListener((v, event) -> {
            float vscroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL);
            float hscroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL);
            return nativeOnMotionEventForSlot(
                    slot,
                    event.getActionMasked(),
                    event.getX(),
                    event.getY(),
                    event.getButtonState(),
                    vscroll,
                    hscroll,
                    event.getEventTime());
        });

        mRoot = new FrameLayout(this);
        FrameLayout.LayoutParams surfaceLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        surfaceLp.topMargin = mCaptionHeightPx;
        mRoot.addView(mSurfaceView, surfaceLp);

        mHeader = new LinearLayout(this);
        mHeader.setOrientation(LinearLayout.HORIZONTAL);
        mHeader.setGravity(Gravity.CENTER_VERTICAL);
        mHeader.setPadding((int) (14 * dp), 0, (int) (14 * dp), 0);
        mHeaderIcon = new ImageView(this);
        if (mCurrentIconBitmap != null) {
            mHeaderIcon.setImageBitmap(mCurrentIconBitmap);
        } else {
            mHeaderIcon.setImageResource(resolveFallbackIconRes(initialLabel, initialLabel));
        }
        LinearLayout.LayoutParams iconLp =
                new LinearLayout.LayoutParams((int) (20 * dp), (int) (20 * dp));
        iconLp.setMarginEnd((int) (10 * dp));
        mHeader.addView(mHeaderIcon, iconLp);

        mHeaderTitle = new TextView(this);
        mHeaderTitle.setText(initialLabel);
        mHeaderTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        mHeaderTitle.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        mHeaderTitle.setSingleLine(true);
        mHeaderTitle.setEllipsize(TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams titleLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        titleLp.setMarginEnd((int) (14 * dp));
        mHeader.addView(mHeaderTitle, titleLp);

        mAppSwitcherScroll = new HorizontalScrollView(this);
        mAppSwitcherScroll.setHorizontalScrollBarEnabled(false);
        mAppSwitcherBar = new LinearLayout(this);
        mAppSwitcherBar.setOrientation(LinearLayout.HORIZONTAL);
        mAppSwitcherBar.setGravity(Gravity.CENTER_VERTICAL);
        mAppSwitcherScroll.addView(mAppSwitcherBar, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        mHeader.addView(mAppSwitcherScroll, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        mHeader.setVisibility(View.VISIBLE);
        mHeader.setOnTouchListener((v, e) -> false);
        FrameLayout.LayoutParams headerLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, barHeightPx, Gravity.TOP);
        headerLp.topMargin = topOffsetPx;
        mRoot.addView(mHeader, headerLp);
        applyHeaderTheme();

        setContentView(mRoot);
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController ic = getWindow().getInsetsController();
        if (ic != null) {
            int appearance = APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND
                    | (isNightMode() ? 0 : APPEARANCE_LIGHT_CAPTION_BARS);
            ic.setSystemBarsAppearance(appearance,
                    APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND | APPEARANCE_LIGHT_CAPTION_BARS);
        }
        mRoot.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets caption = insets.getInsets(WindowInsets.Type.captionBar());
            onCaptionInsets(caption.top, insets, v.getWidth());
            return insets;
        });
        mSurfaceView.requestFocus();
        handleIntentTargetSurface(getIntent());
        int[] cachedSids = sLastKnownSurfaceIds;
        String[] cachedTitles = sLastKnownTitles;
        String[] cachedAppIds = sLastKnownAppIds;
        if (cachedSids != null && cachedTitles != null && cachedAppIds != null) {
            populateAppSwitcherBarUi(
                    cachedSids,
                    cachedTitles,
                    cachedAppIds,
                    mBoundSurfaceId > 0 ? mBoundSurfaceId : initialTargetSid);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        mIsBackgroundRecycle =
                intent != null && intent.getBooleanExtra("is_background_recycle", false);
        setIntent(intent);
        handleIntentTargetSurface(intent);
        markAsForegroundTaskForDock();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (this != sPendingFinishActivity) {
            markAsForegroundTaskForDock();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && this != sPendingFinishActivity && !isFinishing()) {
            markAsForegroundTaskForDock();
        }
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        if (ev != null && ev.getActionMasked() == MotionEvent.ACTION_DOWN) {
            mIsBackgroundRecycle = false;
            sSuppressFocusPromoteUntilMs = 0L;
            sDesiredForegroundActivity = this;
            markAsForegroundTaskForDock();
        }
        return super.dispatchTouchEvent(ev);
    }

    @Override
    protected void onDestroy() {
        if (sPendingFinishActivity == this) {
            sPendingFinishActivity = null;
        }
        sActiveSlotActivities.remove(mWindowSlot, this);
        if (mBoundSurfaceId > 0) {
            boolean stillBoundElsewhere = false;
            for (MainActivity act : sActiveSlotActivities.values()) {
                if (act != null
                        && act != this
                        && !act.isFinishing()
                        && !act.isDestroyed()
                        && act.mBoundSurfaceId == mBoundSurfaceId) {
                    stillBoundElsewhere = true;
                    break;
                }
            }
            if (!stillBoundElsewhere) {
                sActiveWindowSurfaceIds.remove(mBoundSurfaceId);
            }
        }
        if (sCurrentDockLeaderTaskId == getTaskId()) {
            sCurrentDockLeaderTaskId = -1;
        }
        clearAllAppTasksExcludeFromRecents();
        nativeReleaseWindowSlot(mWindowSlot);
        super.onDestroy();
    }

    private void handleIntentTargetSurface(Intent intent) {
        int targetSid = resolveIntentTargetSurface(intent);
        if (targetSid > 0) {
            mBoundSurfaceId = targetSid;
            sActiveWindowSurfaceIds.add(targetSid);
            nativeSelectSurfaceForSlot(mWindowSlot, targetSid);
        }
    }

    private boolean isNightMode() {
        return (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
    }

    private void applyHeaderTheme() {
        boolean night = isNightMode();
        int bgColor = night ? 0xFF1B1B20 : 0xFFF2F0F6;
        int fgColor = night ? 0xFFE3E3E8 : 0xFF1B1B1F;
        if (mRoot != null) {
            mRoot.setBackgroundColor(bgColor);
        }
        if (getWindow() != null && getWindow().getDecorView() != null) {
            getWindow().getDecorView().setBackgroundColor(bgColor);
            WindowInsetsController ic = getWindow().getInsetsController();
            if (ic != null) {
                int appearance = APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND
                        | (night ? 0 : APPEARANCE_LIGHT_CAPTION_BARS);
                ic.setSystemBarsAppearance(appearance,
                        APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND | APPEARANCE_LIGHT_CAPTION_BARS);
            }
        }
        mHeader.setBackgroundColor(bgColor);
        mHeaderTitle.setTextColor(fgColor);
        mLastSwitcherSignature = "";
    }

    /** Lays the header out directly below the system caption bar so SystemUI caption drag overlays never intercept tab clicks. */
    private void onCaptionInsets(int captionHeightPx, WindowInsets insets, int width) {
        final float dp = getResources().getDisplayMetrics().density;
        int barHeightPx = (int) (38 * dp);
        int topOffsetPx;
        if (captionHeightPx > 0) {
            topOffsetPx = captionHeightPx;
            sLastCaptionTopPx = topOffsetPx;
        } else {
            topOffsetPx = (sLastCaptionTopPx > 0) ? sLastCaptionTopPx : (int) (40 * dp);
        }
        int totalTopPx = topOffsetPx + barHeightPx;
        mHeader.setPadding((int) (14 * dp), 0, (int) (14 * dp), 0);
        if (totalTopPx == mCaptionHeightPx) return;
        mCaptionHeightPx = totalTopPx;
        FrameLayout.LayoutParams hlp = (FrameLayout.LayoutParams) mHeader.getLayoutParams();
        hlp.topMargin = topOffsetPx;
        hlp.height = barHeightPx;
        mHeader.setLayoutParams(hlp);
        mHeader.setVisibility(View.VISIBLE);
        FrameLayout.LayoutParams slp = (FrameLayout.LayoutParams) mSurfaceView.getLayoutParams();
        slp.topMargin = totalTopPx;
        mSurfaceView.setLayoutParams(slp);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        applyHeaderTheme();
    }

    private void extractAssetDirRecursive(String assetPath, File destDir) {
        try {
            String[] children = getAssets().list(assetPath);
            if (children == null || children.length == 0) {
                try (InputStream in = getAssets().open(assetPath);
                     FileOutputStream out = new FileOutputStream(destDir)) {
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                    }
                } catch (Exception ignored) {
                }
            } else {
                destDir.mkdirs();
                for (String child : children) {
                    String childAsset = assetPath.isEmpty() ? child : (assetPath + "/" + child);
                    extractAssetDirRecursive(childAsset, new File(destDir, child));
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Failed extracting asset path " + assetPath, e);
        }
    }

    private void setupHostXdgEnvironment() {
        try {
            File xdgDir = new File(getFilesDir(), "xdg");
            extractAssetDirRecursive("xdg", xdgDir);
            Os.setenv("XDG_RUNTIME_DIR", getCacheDir().getAbsolutePath(), true);
            Os.setenv("XDG_DATA_HOME", xdgDir.getAbsolutePath(), true);
            Os.setenv(
                    "XDG_DATA_DIRS",
                    xdgDir.getAbsolutePath() + ":/usr/local/share:/usr/share",
                    true);
        } catch (Throwable t) {
            Log.w(TAG, "Failed to configure XDG environment variables on host", t);
        }
    }

    private synchronized void ensureDirectHostWaylandClientRunning() {
        try {
            File waylandSocket = new File(getCacheDir(), "wayland-0");
            File linuxRoot = new File("/data/local/linux-root");
            if (linuxRoot.exists()) {
                File triggerFile = new File(getCacheDir(), "launch_linux_app.trigger");
                try (FileOutputStream fos = new FileOutputStream(triggerFile)) {
                    String payload =
                            "socket="
                                    + waylandSocket.getAbsolutePath()
                                    + "\ncache="
                                    + getCacheDir().getAbsolutePath()
                                    + "\nts="
                                    + System.currentTimeMillis()
                                    + "\n";
                    fos.write(payload.getBytes());
                }
                triggerFile.setReadable(true, false);
                return;
            }
            if (sDirectClientProcess != null && sDirectClientProcess.isAlive()) {
                return;
            }
            File clientBin =
                    new File(getApplicationInfo().nativeLibraryDir, "libvkwlf_gpu_client.so");
            if (!clientBin.exists()) {
                return;
            }
            ProcessBuilder pb =
                    new ProcessBuilder(
                            clientBin.getAbsolutePath(),
                            "--gpu-only",
                            "--stay-open");
            pb.environment().put("WAYLAND_DISPLAY", waylandSocket.getAbsolutePath());
            pb.environment().put("XDG_RUNTIME_DIR", getCacheDir().getAbsolutePath());
            pb.redirectErrorStream(true);
            sDirectClientProcess = pb.start();
        } catch (Throwable t) {
            Log.e(TAG, "Failed to launch direct-on-host GPU Wayland client", t);
        }
    }

    /**
     * Invoked from native JNI when the list of active Wayland toplevel applications changes
     * or when this window slot's bound surface changes. Supports any number (N) of open apps.
     */
    @SuppressWarnings("unused")
    public void onWaylandAppsChanged(
            int[] surfaceIds, String[] titles, String[] appIds, int boundSurfaceId) {
        if (surfaceIds == null || titles == null || appIds == null) return;
        sLastKnownSurfaceIds = surfaceIds;
        sLastKnownTitles = titles;
        sLastKnownAppIds = appIds;
        for (int i = 0; i < surfaceIds.length; i++) {
            String t = i < titles.length ? titles[i] : "";
            String a = i < appIds.length ? appIds[i] : "";
            sSurfaceAppLabels.put(surfaceIds[i], deriveAppLabel(t, a));
        }
        runOnUiThread(() -> populateAppSwitcherBarUi(surfaceIds, titles, appIds, boundSurfaceId));
    }

    private void populateAppSwitcherBarUi(
            int[] surfaceIds, String[] titles, String[] appIds, int boundSurfaceId) {
        if (surfaceIds == null || titles == null || appIds == null) return;
        if (mBoundSurfaceId > 0 && mBoundSurfaceId != boundSurfaceId) {
            sActiveWindowSurfaceIds.remove(mBoundSurfaceId);
        }
        if (boundSurfaceId > 0) {
            mBoundSurfaceId = boundSurfaceId;
            sActiveWindowSurfaceIds.add(boundSurfaceId);
        }

        if (mAppSwitcherBar == null) return;
        StringBuilder sigBuilder = new StringBuilder();
        sigBuilder.append(boundSurfaceId).append('|');
        for (int i = 0; i < surfaceIds.length; i++) {
            String t = i < titles.length ? titles[i] : "";
            String a = i < appIds.length ? appIds[i] : "";
            sigBuilder.append(surfaceIds[i]).append(':').append(deriveAppLabel(t, a)).append(';');
        }
        String sig = sigBuilder.toString();
        if (sig.equals(mLastSwitcherSignature) && mAppSwitcherBar.getChildCount() > 0) {
            return;
        }
        mLastSwitcherSignature = sig;

        mAppSwitcherBar.removeAllViews();
        final float dp = getResources().getDisplayMetrics().density;
        final boolean night = isNightMode();

        for (int i = 0; i < surfaceIds.length; i++) {
            final int sid = surfaceIds[i];
            final String t = i < titles.length ? titles[i] : "";
            final String a = i < appIds.length ? appIds[i] : "";
            final String shortLabel = deriveAppLabel(t, a);
            final boolean selected = (sid == boundSurfaceId);

            LinearLayout pill = new LinearLayout(this);
            pill.setOrientation(LinearLayout.HORIZONTAL);
            pill.setGravity(Gravity.CENTER_VERTICAL);
            pill.setClickable(true);
            pill.setFocusable(false);
            pill.setPadding((int) (12 * dp), (int) (5 * dp), (int) (12 * dp), (int) (5 * dp));

            GradientDrawable bg = new GradientDrawable();
            bg.setCornerRadius(14 * dp);
            if (selected) {
                bg.setColor(night ? 0xFF2E5BFF : 0xFF1A73E8);
            } else {
                bg.setColor(night ? 0xFF2C2C35 : 0xFFDADCE0);
            }
            pill.setBackground(bg);

            ImageView pillIcon = new ImageView(this);
            Bitmap iconBmp = getSurfaceIconBitmap(sid, t, a);
            if (iconBmp != null) {
                pillIcon.setImageBitmap(iconBmp);
            } else {
                pillIcon.setImageResource(resolveFallbackIconRes(t, a));
            }
            LinearLayout.LayoutParams pilp =
                    new LinearLayout.LayoutParams((int) (16 * dp), (int) (16 * dp));
            pilp.setMarginEnd((int) (6 * dp));
            pill.addView(pillIcon, pilp);

            TextView pillText = new TextView(this);
            pillText.setText(shortLabel);
            pillText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            pillText.setTypeface(Typeface.DEFAULT, selected ? Typeface.BOLD : Typeface.NORMAL);
            pillText.setTextColor(selected ? 0xFFFFFFFF : (night ? 0xFFD0D0D8 : 0xFF202124));
            pill.addView(pillText);

            pill.setOnClickListener(v -> switchToWaylandSurface(sid, shortLabel));
            pill.setOnTouchListener((v, ev) -> {
                if (ev.getActionMasked() == MotionEvent.ACTION_DOWN) {
                    switchToWaylandSurface(sid, shortLabel);
                    return true;
                }
                return false;
            });

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMarginEnd((int) (8 * dp));
            mAppSwitcherBar.addView(pill, lp);
        }

        if (surfaceIds.length >= 2) {
            TextView popBtn = new TextView(this);
            popBtn.setText("⧉ Open Window");
            popBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            popBtn.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            popBtn.setTextColor(night ? 0xFF8AB4F8 : 0xFF1A73E8);
            popBtn.setClickable(true);
            popBtn.setFocusable(false);
            popBtn.setPadding((int) (12 * dp), (int) (5 * dp), (int) (12 * dp), (int) (5 * dp));
            GradientDrawable popBg = new GradientDrawable();
            popBg.setCornerRadius(14 * dp);
            popBg.setStroke((int) (1 * dp), night ? 0xFF8AB4F8 : 0xFF1A73E8);
            popBtn.setBackground(popBg);
            popBtn.setOnClickListener(v -> openNextUnboundSurfaceWindow(surfaceIds, boundSurfaceId));
            popBtn.setOnTouchListener((v, ev) -> {
                if (ev.getActionMasked() == MotionEvent.ACTION_DOWN) {
                    openNextUnboundSurfaceWindow(surfaceIds, boundSurfaceId);
                    return true;
                }
                return false;
            });
            mAppSwitcherBar.addView(popBtn);
        }
    }

    private void finishPendingActivityNow() {
        MainActivity pending = sPendingFinishActivity;
        if (pending != null && pending != this) {
            sPendingFinishActivity = null;
            final MainActivity self = this;
            pending.runOnUiThread(() -> {
                try {
                    if (!pending.isFinishing() && !pending.isDestroyed()) {
                        pending.finishAndRemoveTask();
                        pending.overridePendingTransition(0, 0);
                    }
                } catch (Throwable ignored) {
                }
                self.runOnUiThread(self::markAsForegroundTaskForDock);
            });
        }
    }

    /**
     * Invoked from native JNI right after the initial Vulkan frame has been rendered and presented
     * onto this Activity's SurfaceView. If a previous single-window Activity was waiting underneath
     * to be replaced, we finish it now that our frame is already visible on screen.
     */
    @SuppressWarnings("unused")
    public void onFirstVulkanFramePresented() {
        runOnUiThread(this::finishPendingActivityNow);
    }

    private void switchToWaylandSurface(int sid, String shortLabel) {
        if (sid <= 0) return;
        String resolvedLabel =
                (shortLabel != null && !shortLabel.isEmpty())
                        ? shortLabel
                        : sSurfaceAppLabels.getOrDefault(sid, getTitle().toString());
        if (resolvedLabel != null && !resolvedLabel.isEmpty()) {
            sSurfaceAppLabels.put(sid, resolvedLabel);
        }
        Bitmap resolvedIcon = getSurfaceIconBitmap(sid, resolvedLabel, resolvedLabel);
        Class<? extends MainActivity> targetClass =
                resolveActivityClassForSurface(sid, resolvedLabel, resolvedLabel);

        if (sid == mBoundSurfaceId && targetClass == getClass()) {
            markAsForegroundTaskForDock();
            nativeSelectSurfaceForSlot(mWindowSlot, sid);
            mSurfaceView.requestFocus();
            return;
        }
        MainActivity existingWithSurface = findOpenActivityBySurface(sid);
        if (existingWithSurface != null
                && existingWithSurface != this
                && existingWithSurface.getClass() == targetClass) {
            cancelPendingDockOrderAndClearExclusions();
            existingWithSurface.mIsBackgroundRecycle = false;
            sSuppressFocusPromoteUntilMs = 0L;
            sCurrentDockLeaderTaskId = -1;
            sDesiredForegroundActivity = existingWithSurface;
            bringActivityToFront(existingWithSurface);
            existingWithSurface.markAsForegroundTaskForDock();
            return;
        }
        MainActivity existingByClass = findOpenActivityByClass(targetClass);
        if (existingByClass != null && existingByClass != this) {
            cancelPendingDockOrderAndClearExclusions();
            existingByClass.mIsBackgroundRecycle = false;
            sSuppressFocusPromoteUntilMs = 0L;
            sCurrentDockLeaderTaskId = -1;
            sDesiredForegroundActivity = existingByClass;
            Intent rebindIntent = new Intent(this, targetClass);
            rebindIntent.setData(Uri.parse("vkwlf://surface/" + sid));
            rebindIntent.putExtra("target_surface_id", sid);
            existingByClass.handleIntentTargetSurface(rebindIntent);
            bringActivityToFront(existingByClass);
            existingByClass.markAsForegroundTaskForDock();
            return;
        }
        if (targetClass != getClass()) {
            try {
                cancelPendingDockOrderAndClearExclusions();
                if (resolvedIcon != null) {
                    applyGenericActivityOverlayIfNeeded(
                            targetClass, sid, resolvedLabel, resolvedIcon);
                }
                sLastTransitionStartMs = System.currentTimeMillis();
                Intent intent = new Intent(this, targetClass);
                intent.setData(Uri.parse("vkwlf://surface/" + sid));
                intent.putExtra("target_surface_id", sid);
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION);
                Rect bounds = getWindowManager().getCurrentWindowMetrics().getBounds();
                ActivityOptions ao = ActivityOptions.makeCustomAnimation(this, 0, 0);
                if (bounds != null && !bounds.isEmpty()) {
                    ao.setLaunchBounds(bounds);
                }
                sPendingFinishActivity = this;
                sActiveSlotActivities.remove(mWindowSlot, this);
                startActivity(intent, ao.toBundle());
                overridePendingTransition(0, 0);
                return;
            } catch (Throwable t) {
                Log.w(TAG, "Failed to switch Activity for surface " + sid, t);
            }
        }
        if (mBoundSurfaceId > 0 && mBoundSurfaceId != sid) {
            sActiveWindowSurfaceIds.remove(mBoundSurfaceId);
        }
        mBoundSurfaceId = sid;
        sActiveWindowSurfaceIds.add(sid);
        markAsForegroundTaskForDock();
        nativeSelectSurfaceForSlot(mWindowSlot, sid);
        mSurfaceView.requestFocus();
        if (resolvedIcon != null) {
            mCurrentIconBitmap = resolvedIcon;
            applyGenericActivityOverlayIfNeeded(getClass(), sid, resolvedLabel, resolvedIcon);
            mHeaderIcon.setImageBitmap(resolvedIcon);
            mHeaderTitle.setText(resolvedLabel);
            setTitle(resolvedLabel);
        }
    }

    private void openNextUnboundSurfaceWindow(int[] surfaceIds, int currentBoundSid) {
        if (surfaceIds == null || surfaceIds.length == 0) return;
        for (int sid : surfaceIds) {
            if (sid != currentBoundSid && !sActiveWindowSurfaceIds.contains(sid)) {
                openWindowForSurface(sid);
                return;
            }
        }
        for (int sid : surfaceIds) {
            if (sid != currentBoundSid) {
                openWindowForSurface(sid);
                return;
            }
        }
    }

    private void openWindowForSurface(int targetSid) {
        if (targetSid <= 0) return;
        try {
            cancelPendingDockOrderAndClearExclusions();
            sLastTransitionStartMs = System.currentTimeMillis();
            String label = sSurfaceAppLabels.getOrDefault(targetSid, "Wayland");
            Bitmap icon = getSurfaceIconBitmap(targetSid, label, label);
            Class<? extends MainActivity> targetClass =
                    resolveActivityClassForSurface(targetSid, label, label);
            MainActivity existingForSid = findOpenActivityBySurface(targetSid);
            if (existingForSid != null
                    && existingForSid != this
                    && existingForSid.getClass() == targetClass) {
                existingForSid.mIsBackgroundRecycle = false;
                sSuppressFocusPromoteUntilMs = 0L;
                sCurrentDockLeaderTaskId = -1;
                sDesiredForegroundActivity = existingForSid;
                bringActivityToFront(existingForSid);
                existingForSid.markAsForegroundTaskForDock();
                return;
            }
            if (targetClass == getClass()) {
                // Current window is already the matching Activity class; pick an available window
                // class for the other surface or generic slot.
                targetClass =
                        !isActivityClassOpen(GenericWaylandActivity.class)
                                ? GenericWaylandActivity.class
                                : GenericWaylandActivity2.class;
            }
            if (icon != null) {
                applyGenericActivityOverlayIfNeeded(targetClass, targetSid, label, icon);
            }
            MainActivity existing = findOpenActivityByClass(targetClass);
            Intent intent = new Intent(this, targetClass);
            intent.setData(Uri.parse("vkwlf://surface/" + targetSid));
            intent.putExtra("target_surface_id", targetSid);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_LAUNCH_ADJACENT);
            if (existing != null) {
                existing.mIsBackgroundRecycle = false;
                sSuppressFocusPromoteUntilMs = 0L;
                sCurrentDockLeaderTaskId = -1;
                sDesiredForegroundActivity = existing;
                existing.handleIntentTargetSurface(intent);
                bringActivityToFront(existing);
                existing.markAsForegroundTaskForDock();
                return;
            }
            Bundle opts = null;
            Rect cur = getWindowManager().getCurrentWindowMetrics().getBounds();
            Rect max = getWindowManager().getMaximumWindowMetrics().getBounds();
            if (cur != null && !cur.isEmpty()) {
                float dp = getResources().getDisplayMetrics().density;
                int dx = (int) (80 * dp);
                int dy = (int) (56 * dp);
                Rect next = new Rect(cur);
                if (max != null && !max.isEmpty() && next.right + dx > max.right - (int) (24 * dp)) {
                    dx = -dx;
                }
                if (max != null && !max.isEmpty() && next.bottom + dy > max.bottom - (int) (80 * dp)) {
                    dy = -dy;
                }
                next.offset(dx, dy);
                if (max != null && !max.isEmpty()) {
                    if (next.left < max.left + (int) (16 * dp)) {
                        next.offsetTo(max.left + (int) (16 * dp), next.top);
                    }
                    if (next.top < max.top + (int) (48 * dp)) {
                        next.offsetTo(next.left, max.top + (int) (48 * dp));
                    }
                }
                ActivityOptions ao = ActivityOptions.makeCustomAnimation(this, 0, 0);
                ao.setLaunchBounds(next);
                opts = ao.toBundle();
            }
            startActivity(intent, opts);
            overridePendingTransition(0, 0);
        } catch (Throwable t) {
            Log.w(TAG, "Failed to launch Wayland window for surface " + targetSid, t);
        }
    }

    /**
     * Invoked from native JNI when a guest Wayland application maps or updates a toplevel window,
     * providing its window title, xdg_toplevel app_id, and resolved PNG icon bytes.
     */
    @SuppressWarnings({"deprecation", "unused"})
    public void onWaylandAppInfo(int surfaceId, String title, String appId, byte[] iconPng) {
        final String shortLabel = deriveAppLabel(title, appId);
        final String cacheKey = surfaceId + "|" + shortLabel;
        if (surfaceId > 0) {
            sSurfaceAppLabels.put(surfaceId, shortLabel);
        }
        Bitmap decodedIcon = (surfaceId > 0) ? sSurfaceIconCache.get(cacheKey) : null;
        if (decodedIcon == null && iconPng != null && iconPng.length > 0) {
            decodedIcon = BitmapFactory.decodeByteArray(iconPng, 0, iconPng.length);
            if (decodedIcon != null) {
                if (surfaceId > 0) {
                    sSurfaceIconCache.put(cacheKey, decodedIcon);
                }
                sLabelIconCache.put(shortLabel, decodedIcon);
            }
        }
        if (decodedIcon == null) {
            decodedIcon = getSurfaceIconBitmap(surfaceId, title, appId);
        }
        final Bitmap finalIcon = decodedIcon;
        final String appIdCopy = (appId != null) ? appId : "";

        runOnUiThread(() -> {
            if (surfaceId > 0 && (mBoundSurfaceId <= 0 || mBoundSurfaceId == surfaceId)) {
                mBoundSurfaceId = surfaceId;
                sActiveWindowSurfaceIds.add(surfaceId);
                Class<? extends MainActivity> expectedClass =
                        resolveActivityClassForSurface(surfaceId, title, appIdCopy);
                if (expectedClass != getClass()
                        && !isFinishing()
                        && !isDestroyed()
                        && this != sPendingFinishActivity
                        && sPendingFinishActivity == null
                        && findOpenActivityByClass(expectedClass) == null) {
                    switchToWaylandSurface(surfaceId, shortLabel);
                    return;
                }
            }
            boolean labelChanged = !shortLabel.equals(getTitle());
            boolean iconChanged = (finalIcon != null && finalIcon != mCurrentIconBitmap);
            if (labelChanged) {
                setTitle(shortLabel);
            }
            if (finalIcon != null) {
                mCurrentIconBitmap = finalIcon;
            }
            mHeaderTitle.setText(shortLabel);
            if (mCurrentIconBitmap != null) {
                mHeaderIcon.setImageBitmap(mCurrentIconBitmap);
            } else {
                mHeaderIcon.setImageResource(resolveFallbackIconRes(title, appIdCopy));
            }
            if (labelChanged || iconChanged) {
                if (mCurrentIconBitmap != null) {
                    applyGenericActivityOverlayIfNeeded(
                            getClass(), surfaceId, shortLabel, mCurrentIconBitmap);
                    setTaskDescription(
                            new ActivityManager.TaskDescription(shortLabel, mCurrentIconBitmap));
                    mLastSwitcherSignature = "";
                    int[] cachedSids = sLastKnownSurfaceIds;
                    String[] cachedTitles = sLastKnownTitles;
                    String[] cachedAppIds = sLastKnownAppIds;
                    if (cachedSids != null && cachedTitles != null && cachedAppIds != null) {
                        populateAppSwitcherBarUi(
                                cachedSids, cachedTitles, cachedAppIds, mBoundSurfaceId);
                    }
                } else {
                    setTaskDescription(new ActivityManager.TaskDescription(shortLabel));
                }
            }
            if (!appIdCopy.isEmpty()) {
                mLastAppId = appIdCopy;
            }
        });
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Surface surface = holder.getSurface();
        nativeOnSurfaceCreatedForSlot(
                mWindowSlot, surface, mSurfaceView.getWidth(), mSurfaceView.getHeight());
        if (sPendingFinishActivity != null && sPendingFinishActivity != this) {
            mRoot.postDelayed(this::finishPendingActivityNow, 220);
        }
        Thread clientLaunchThread =
                new Thread(this::ensureDirectHostWaylandClientRunning, "VkWlfClientLauncher");
        clientLaunchThread.setDaemon(true);
        clientLaunchThread.start();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeOnSurfaceChangedForSlot(mWindowSlot, width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeOnSurfaceDestroyedForSlot(mWindowSlot);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (nativeOnKeyEventForSlot(
                mWindowSlot,
                event.getAction(),
                event.getKeyCode(),
                event.getMetaState(),
                event.getEventTime())) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    protected static native int nativeAllocateWindowSlot(int preferredSlot);
    protected static native void nativeReleaseWindowSlot(int slot);
    protected static native byte[] nativeGetSurfaceIconPng(int surfaceId);
    protected native void nativeOnSurfaceCreatedForSlot(
            int slot, Surface surface, int width, int height);
    protected static native void nativeOnSurfaceChangedForSlot(int slot, int width, int height);
    protected static native void nativeOnSurfaceDestroyedForSlot(int slot);
    protected static native boolean nativeOnMotionEventForSlot(
            int slot, int action, float x, float y, int buttonState, float vscroll, float hscroll, long eventTimeMs);
    protected static native boolean nativeOnKeyEventForSlot(
            int slot, int action, int keyCode, int metaState, long eventTimeMs);
    protected static native void nativeSelectSurfaceForSlot(int slot, int surfaceId);

    private native void nativeOnSurfaceCreated(Surface surface, int width, int height);
    private static native void nativeOnSurfaceChanged(int width, int height);
    private static native void nativeOnSurfaceDestroyed();
    private static native boolean nativeOnMotionEvent(
            int action, float x, float y, int buttonState, float vscroll, float hscroll, long eventTimeMs);
    private static native boolean nativeOnKeyEvent(
            int action, int keyCode, int metaState, long eventTimeMs);
}
