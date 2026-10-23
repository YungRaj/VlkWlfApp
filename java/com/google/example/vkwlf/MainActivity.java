package com.google.example.vkwlf;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.content.om.FabricatedOverlay;
import android.content.om.OverlayManager;
import android.content.om.OverlayManagerTransaction;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Insets;
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
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
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
    private static final Set<Integer> sAutoLaunchedSurfaceIds =
            Collections.synchronizedSet(new HashSet<>());
    private static final Set<Integer> sActiveWindowSurfaceIds =
            Collections.synchronizedSet(new HashSet<>());
    private static final Map<Integer, Bitmap> sSurfaceIconCache = new ConcurrentHashMap<>();

    static {
        System.loadLibrary("vkwlf_jni");
    }

    private int mWindowSlot = 0;
    private int mBoundSurfaceId = 0;
    private SurfaceView mSurfaceView;
    private FrameLayout mRoot;
    private LinearLayout mHeader;
    private ImageView mHeaderIcon;
    private TextView mHeaderTitle;
    private HorizontalScrollView mAppSwitcherScroll;
    private LinearLayout mAppSwitcherBar;
    private int mCaptionHeightPx;
    private Bitmap mCurrentIconBitmap;
    private byte[] mLastOverlayIconBytes;
    private String mLastOverlayLabel;
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

    protected String getOverlayName() {
        return mWindowSlot == 0
                ? "WaylandDynamicAppMetaOverlay"
                : "WaylandDynamicAppMetaOverlaySlot" + mWindowSlot;
    }

    protected String getOverlayLabelResource() {
        return getPackageName()
                + (mWindowSlot == 0 ? ":string/wayland_app_name" : ":string/app_name_teamviewer");
    }

    protected String getOverlayIconResource() {
        return getPackageName()
                + (mWindowSlot == 0 ? ":drawable/ic_wayland_app" : ":drawable/ic_app_teamviewer");
    }

    /**
     * Stable application label for the window caption / launcher overlay. Window titles change
     * constantly ("App - file.txt"), so prefer the part before the first " - " / " — " separator,
     * falling back to a capitalised app_id.
     */
    private static String deriveAppLabel(String title, String appId) {
        if (appId != null) {
            String lowerId = appId.toLowerCase();
            if (lowerId.contains("teamviewer")) return "TeamViewer";
            if (lowerId.contains("antigravity")) return "Antigravity";
        }
        if (title != null && !title.isEmpty()) {
            if (title.toLowerCase().contains("teamviewer")) return "TeamViewer";
            String t = title;
            for (String sep : new String[] {" - ", " — ", " – "}) {
                int i = t.indexOf(sep);
                if (i > 0) {
                    t = t.substring(0, i);
                }
            }
            t = t.replaceAll("\\s*\\[[^\\]]*\\]\\s*$", "").trim();
            if (!t.isEmpty()) return t;
        }
        if (appId != null && !appId.isEmpty()) {
            String last = appId.substring(appId.lastIndexOf('.') + 1).replace('-', ' ').replace('_', ' ');
            return last.isEmpty() ? appId : Character.toUpperCase(last.charAt(0)) + last.substring(1);
        }
        return "Linux Wayland App";
    }

    private int resolveFallbackIconRes(String title, String appId) {
        String combined = ((title != null ? title : "") + " " + (appId != null ? appId : "")).toLowerCase();
        if (combined.contains("teamviewer")) {
            return R.drawable.ic_app_teamviewer;
        }
        if (combined.contains("antigravity")) {
            return R.drawable.ic_app_antigravity;
        }
        return R.drawable.ic_wayland_app;
    }

    private Bitmap getSurfaceIconBitmap(int surfaceId, String title, String appId) {
        Bitmap cached = sSurfaceIconCache.get(surfaceId);
        if (cached != null) {
            return cached;
        }
        try {
            byte[] png = nativeGetSurfaceIconPng(surfaceId);
            if (png != null && png.length > 0) {
                Bitmap decoded = BitmapFactory.decodeByteArray(png, 0, png.length);
                if (decoded != null) {
                    sSurfaceIconCache.put(surfaceId, decoded);
                    return decoded;
                }
            }
        } catch (Throwable ignored) {
        }
        return BitmapFactory.decodeResource(getResources(), resolveFallbackIconRes(title, appId));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
        super.onCreate(savedInstanceState);
        mWindowSlot = nativeAllocateWindowSlot(getPreferredWindowSlot());
        setupHostXdgEnvironment();

        final int slot = mWindowSlot;
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
        mSurfaceView.addOnLayoutChangeListener(
                (v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
                    int w = right - left;
                    int h = bottom - top;
                    int oldW = oldRight - oldLeft;
                    int oldH = oldBottom - oldTop;
                    if (w > 0 && h > 0 && (w != oldW || h != oldH)) {
                        nativeOnSurfaceChangedForSlot(slot, w, h);
                    }
                });

        mRoot = new FrameLayout(this);
        mRoot.addView(mSurfaceView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        mHeader = new LinearLayout(this);
        mHeader.setOrientation(LinearLayout.HORIZONTAL);
        mHeader.setGravity(Gravity.CENTER_VERTICAL);
        final float dp = getResources().getDisplayMetrics().density;
        mHeader.setPadding((int) (72 * dp), 0, (int) (160 * dp), 0);
        mHeaderIcon = new ImageView(this);
        mHeaderIcon.setImageResource(
                slot == 1 ? R.drawable.ic_app_teamviewer : R.drawable.ic_app_antigravity);
        LinearLayout.LayoutParams iconLp =
                new LinearLayout.LayoutParams((int) (20 * dp), (int) (20 * dp));
        iconLp.setMarginEnd((int) (10 * dp));
        mHeader.addView(mHeaderIcon, iconLp);

        mHeaderTitle = new TextView(this);
        mHeaderTitle.setText(
                getString(slot == 1 ? R.string.app_name_teamviewer : R.string.app_name_antigravity));
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

        mHeader.setVisibility(View.GONE);
        mHeader.setOnTouchListener((v, e) -> false);
        mRoot.addView(mHeader, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, Gravity.TOP));
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
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntentTargetSurface(intent);
    }

    @Override
    protected void onDestroy() {
        if (mBoundSurfaceId > 0) {
            sActiveWindowSurfaceIds.remove(mBoundSurfaceId);
        }
        nativeReleaseWindowSlot(mWindowSlot);
        super.onDestroy();
    }

    private void handleIntentTargetSurface(Intent intent) {
        if (intent == null) return;
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
        int topOffsetPx = Math.max(0, captionHeightPx);
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
        runOnUiThread(() -> {
            if (mBoundSurfaceId > 0 && mBoundSurfaceId != boundSurfaceId) {
                sActiveWindowSurfaceIds.remove(mBoundSurfaceId);
            }
            if (boundSurfaceId > 0) {
                mBoundSurfaceId = boundSurfaceId;
                sActiveWindowSurfaceIds.add(boundSurfaceId);
                sAutoLaunchedSurfaceIds.add(boundSurfaceId);
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

                pill.setOnClickListener(v -> {
                    nativeSelectSurfaceForSlot(mWindowSlot, sid);
                    mSurfaceView.requestFocus();
                });
                pill.setOnTouchListener((v, ev) -> {
                    if (ev.getActionMasked() == MotionEvent.ACTION_DOWN) {
                        nativeSelectSurfaceForSlot(mWindowSlot, sid);
                        mSurfaceView.requestFocus();
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

                if (mWindowSlot == 0 && isInMultiWindowMode()) {
                    int delayMs = 350;
                    for (int sid : surfaceIds) {
                        if (sid != boundSurfaceId && sAutoLaunchedSurfaceIds.add(sid)) {
                            final int targetSid = sid;
                            mRoot.postDelayed(() -> openWindowForSurface(targetSid), delayMs);
                            delayMs += 300;
                        }
                    }
                }
            }
        });
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
            Intent intent = new Intent(this, SecondaryWaylandActivity.class);
            intent.setData(Uri.parse("vkwlf://surface/" + targetSid));
            intent.putExtra("target_surface_id", targetSid);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_LAUNCH_ADJACENT);
            startActivity(intent);
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
        Bitmap decodedIcon = null;
        if (iconPng != null && iconPng.length > 0) {
            decodedIcon = BitmapFactory.decodeByteArray(iconPng, 0, iconPng.length);
            if (decodedIcon != null && surfaceId > 0) {
                sSurfaceIconCache.put(surfaceId, decodedIcon);
            }
        }
        if (decodedIcon == null) {
            decodedIcon = getSurfaceIconBitmap(surfaceId, title, appId);
        }
        final Bitmap finalIcon = decodedIcon;
        final byte[] iconBytesCopy =
                (iconPng != null && iconPng.length > 0) ? Arrays.copyOf(iconPng, iconPng.length) : null;
        final String appIdCopy = (appId != null) ? appId : "";

        runOnUiThread(() -> {
            setTitle(shortLabel);
            if (finalIcon != null) {
                mCurrentIconBitmap = finalIcon;
            }
            mHeaderTitle.setText(shortLabel);
            if (mCurrentIconBitmap != null) {
                mHeaderIcon.setImageBitmap(mCurrentIconBitmap);
            } else {
                mHeaderIcon.setImageResource(resolveFallbackIconRes(title, appIdCopy));
            }
            if (mCurrentIconBitmap != null) {
                setTaskDescription(new ActivityManager.TaskDescription(shortLabel, mCurrentIconBitmap));
            } else {
                setTaskDescription(new ActivityManager.TaskDescription(shortLabel));
            }
            if (!appIdCopy.isEmpty()) {
                mLastAppId = appIdCopy;
            }
            applyDynamicAppOverlay(shortLabel, iconBytesCopy);
        });
    }

    /** @return true if an overlay with the given label was committed. */
    private boolean applyDynamicAppOverlay(String label, byte[] iconPng) {
        try {
            if (iconPng != null && iconPng.length > 0) {
                mLastOverlayIconBytes = iconPng;
            }
            if (mLastOverlayIconBytes == null || mLastOverlayIconBytes.length == 0) {
                return false;
            }
            if (label.equals(mLastOverlayLabel) && iconPng == null) {
                return false;
            }
            mLastOverlayLabel = label;

            File iconFile = new File(
                    getCacheDir(), "wayland_app_icon_slot" + mWindowSlot + ".png");
            try (FileOutputStream fos = new FileOutputStream(iconFile)) {
                fos.write(mLastOverlayIconBytes);
                fos.flush();
            }

            OverlayManager om = getSystemService(OverlayManager.class);
            if (om != null) {
                FabricatedOverlay overlay =
                        new FabricatedOverlay(getOverlayName(), getPackageName());
                overlay.setTargetOverlayable("WaylandDynamicAppMeta");
                overlay.setResourceValue(
                        getOverlayLabelResource(),
                        TypedValue.TYPE_STRING,
                        label,
                        null);
                try (ParcelFileDescriptor pfd =
                        ParcelFileDescriptor.open(iconFile, ParcelFileDescriptor.MODE_READ_ONLY)) {
                    overlay.setResourceValue(
                            getOverlayIconResource(),
                            pfd,
                            null);
                    OverlayManagerTransaction tx = OverlayManagerTransaction.newInstance();
                    tx.registerFabricatedOverlay(overlay);
                    om.commit(tx);
                    Log.i(TAG, "[Slot " + mWindowSlot
                            + "] Committed FabricatedOverlay for dynamic Wayland app icon and label='"
                            + label + "' (" + mLastOverlayIconBytes.length + " bytes)");
                    return true;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed to apply FabricatedOverlay for dynamic Wayland app icon", t);
        }
        return false;
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Surface surface = holder.getSurface();
        nativeOnSurfaceCreatedForSlot(
                mWindowSlot, surface, mSurfaceView.getWidth(), mSurfaceView.getHeight());
        if (mWindowSlot == 0) {
            ensureDirectHostWaylandClientRunning();
        }
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
