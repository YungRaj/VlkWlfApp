package com.google.example.vkwlf;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.om.FabricatedOverlay;
import android.content.om.OverlayManager;
import android.content.om.OverlayManagerTransaction;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Insets;
import android.graphics.Typeface;
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
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Arrays;

/**
 * Host Android Activity embedding an in-process Wayland-to-Vulkan compositor
 * rendering directly onto a SurfaceView's ANativeWindow via vkQueuePresentKHR.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "VkWlfApp";
    private static Process sDirectClientProcess;

    static {
        System.loadLibrary("vkwlf_jni");
    }

    private SurfaceView mSurfaceView;
    private FrameLayout mRoot;
    private LinearLayout mHeader;
    private ImageView mHeaderIcon;
    private TextView mHeaderTitle;
    private int mCaptionHeightPx;
    private Bitmap mCurrentIconBitmap;
    private byte[] mLastOverlayIconBytes;
    private String mLastOverlayLabel;
    private String mLastAppId;

    // Mirrors WindowInsetsController.APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND /
    // APPEARANCE_LIGHT_CAPTION_BARS (API 35) so this also compiles against older android.jar.
    private static final int APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND = 1 << 7;
    private static final int APPEARANCE_LIGHT_CAPTION_BARS = 1 << 8;

    /**
     * Stable application label for the window caption / launcher overlay. Window titles change
     * constantly ("App - file.txt"), so prefer the part before the first " - " / " — " separator,
     * falling back to a capitalised app_id.
     */
    private static String deriveAppLabel(String title, String appId) {
        if (title != null && !title.isEmpty()) {
            String t = title;
            for (String sep : new String[] {" - ", " — ", " – "}) {
                int i = t.indexOf(sep);
                if (i > 0) {
                    t = t.substring(0, i);
                }
            }
            t = t.replaceAll("\\s*\\[[^\\]]*\\]\\s*$", "").trim();  // drop "[Superuser]"-style suffixes
            if (!t.isEmpty()) return t;
        }
        if (appId != null && !appId.isEmpty()) {
            String last = appId.substring(appId.lastIndexOf('.') + 1).replace('-', ' ').replace('_', ' ');
            return last.isEmpty() ? appId : Character.toUpperCase(last.charAt(0)) + last.substring(1);
        }
        return "Linux Wayland App";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
        super.onCreate(savedInstanceState);
        setupHostXdgEnvironment();
        mSurfaceView = new SurfaceView(this);
        mSurfaceView.setFocusable(true);
        mSurfaceView.setFocusableInTouchMode(true);
        mSurfaceView.getHolder().addCallback(this);
        mSurfaceView.setOnTouchListener((v, event) -> nativeOnMotionEvent(
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
            return nativeOnMotionEvent(
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
                        nativeOnSurfaceChanged(w, h);
                    }
                });

        // --- App-drawn caption header -------------------------------------------------------
        // In desktop windowing the system header shows the *host package's* static label and
        // icon, which a normal (non-privileged) app cannot change at runtime on release builds
        // (a self-targeting FabricatedOverlay is invisible to SystemUI). With a transparent
        // caption-bar background the system stops drawing the app *name* and only keeps the
        // launcher icon + app-menu chevron on the left and the window controls on the right.
        // We draw the Linux app's real icon + name in the gap between them (bounds come from
        // WindowInsets.getBoundingRects(captionBar())); the Wayland surface is laid out below.
        mRoot = new FrameLayout(this);
        mRoot.addView(mSurfaceView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        mHeader = new LinearLayout(this);
        mHeader.setOrientation(LinearLayout.HORIZONTAL);
        mHeader.setGravity(Gravity.CENTER_VERTICAL);
        final float dp = getResources().getDisplayMetrics().density;
        mHeader.setPadding((int) (72 * dp), 0, (int) (160 * dp), 0);  // refined from insets
        mHeaderIcon = new ImageView(this);
        mHeaderIcon.setImageResource(R.drawable.ic_wayland_app);
        LinearLayout.LayoutParams iconLp =
                new LinearLayout.LayoutParams((int) (20 * dp), (int) (20 * dp));
        iconLp.setMarginEnd((int) (10 * dp));
        mHeader.addView(mHeaderIcon, iconLp);
        mHeaderTitle = new TextView(this);
        mHeaderTitle.setText(getString(R.string.wayland_app_name));
        mHeaderTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        mHeaderTitle.setTypeface(Typeface.DEFAULT, Typeface.NORMAL);
        mHeaderTitle.setSingleLine(true);
        mHeaderTitle.setEllipsize(TextUtils.TruncateAt.END);
        mHeader.addView(mHeaderTitle, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        mHeader.setVisibility(View.GONE);
        // Forward drags on our header to the system so the window can still be moved.
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
    }

    private boolean isNightMode() {
        return (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
    }

    private void applyHeaderTheme() {
        boolean night = isNightMode();
        mHeader.setBackgroundColor(night ? 0xFF1B1B20 : 0xFFF2F0F6);
        mHeaderTitle.setTextColor(night ? 0xFFE3E3E8 : 0xFF1B1B1F);
    }

    /** Lays the header out over the (transparent) system caption and the surface below it. */
    private void onCaptionInsets(int captionHeightPx, WindowInsets insets, int width) {
        // Keep clear of the system-drawn caption elements (icon/chevron on the left, window
        // controls on the right). Bounding rects are in window coordinates.
        final float dp = getResources().getDisplayMetrics().density;
        int padLeft = (int) (72 * dp);
        int padRight = (int) (160 * dp);
        try {
            int leftEnd = 0, rightStart = width > 0 ? width : Integer.MAX_VALUE;
            for (android.graphics.Rect r : insets.getBoundingRects(WindowInsets.Type.captionBar())) {
                if (r.top >= captionHeightPx) continue;
                boolean onLeft = r.left < (width > 0 ? width / 2 : r.left + 1);
                if (onLeft) leftEnd = Math.max(leftEnd, r.right);
                else rightStart = Math.min(rightStart, r.left);
            }
            if (leftEnd > 0) padLeft = leftEnd + (int) (8 * dp);
            if (width > 0 && rightStart < width) padRight = width - rightStart + (int) (8 * dp);
        } catch (Throwable ignored) {
            // getBoundingRects() is API 35+; keep the fallback padding.
        }
        mHeader.setPadding(padLeft, 0, padRight, 0);
        if (captionHeightPx == mCaptionHeightPx) return;
        mCaptionHeightPx = captionHeightPx;
        FrameLayout.LayoutParams hlp = (FrameLayout.LayoutParams) mHeader.getLayoutParams();
        hlp.height = captionHeightPx;
        mHeader.setLayoutParams(hlp);
        mHeader.setVisibility(captionHeightPx > 0 ? View.VISIBLE : View.GONE);
        FrameLayout.LayoutParams slp = (FrameLayout.LayoutParams) mSurfaceView.getLayoutParams();
        slp.topMargin = captionHeightPx;
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
                Log.i(
                        TAG,
                        "Triggered direct-on-host launch of real /opt/example-ide/example-ide connected to "
                                + waylandSocket.getAbsolutePath()
                                + " without pKVM");
                return;
            }
            if (sDirectClientProcess != null && sDirectClientProcess.isAlive()) {
                return;
            }
            File clientBin =
                    new File(getApplicationInfo().nativeLibraryDir, "libvkwlf_gpu_client.so");
            if (!clientBin.exists()) {
                Log.w(TAG, "Direct host GPU Wayland client binary not found at " + clientBin);
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
            Log.i(
                    TAG,
                    "Launched direct-on-host Vulkan GPU Wayland client ("
                            + clientBin.getAbsolutePath()
                            + ") connected to "
                            + waylandSocket.getAbsolutePath()
                            + " without pKVM");
        } catch (Throwable t) {
            Log.e(TAG, "Failed to launch direct-on-host GPU Wayland client", t);
        }
    }

    /**
     * Invoked from native JNI when a guest Wayland application maps or updates a toplevel window,
     * providing its window title, xdg_toplevel app_id, and resolved PNG icon bytes.
     */
    @SuppressWarnings({"deprecation", "unused"})
    public void onWaylandAppInfo(int surfaceId, String title, String appId, byte[] iconPng) {
        final String resolvedLabel =
                (title != null && !title.isEmpty())
                        ? title
                        : ((appId != null && !appId.isEmpty()) ? appId : "Linux Wayland App");
        Bitmap decodedIcon = null;
        if (iconPng != null && iconPng.length > 0) {
            decodedIcon = BitmapFactory.decodeByteArray(iconPng, 0, iconPng.length);
        }
        final Bitmap finalIcon = decodedIcon;
        final byte[] iconBytesCopy =
                (iconPng != null && iconPng.length > 0) ? Arrays.copyOf(iconPng, iconPng.length) : null;
        final String appIdCopy = (appId != null) ? appId : "";

        runOnUiThread(() -> {
            setTitle(resolvedLabel);
            if (finalIcon != null) {
                mCurrentIconBitmap = finalIcon;
            }
            // Our own caption header: the Linux app's real icon and (stable) name.
            mHeaderTitle.setText(deriveAppLabel(title, appIdCopy));
            if (mCurrentIconBitmap != null) {
                mHeaderIcon.setImageBitmap(mCurrentIconBitmap);
            }
            if (mCurrentIconBitmap != null) {
                setTaskDescription(new ActivityManager.TaskDescription(resolvedLabel, mCurrentIconBitmap));
            } else {
                setTaskDescription(new ActivityManager.TaskDescription(resolvedLabel));
            }
            // Mirror the stable app label/icon into this package's own resources via a
            // self-targeting FabricatedOverlay. NOTE: on release (user) builds a self-targeting
            // overlay is only visible to this process, so the desktop window caption (drawn by
            // SystemUI from the static package label) keeps showing the generic app name; the
            // real Linux app name/icon is still used for Recents via the TaskDescription above.
            if (!appIdCopy.isEmpty()) {
                mLastAppId = appIdCopy;
            }
            applyDynamicAppOverlay(deriveAppLabel(title, appIdCopy), iconBytesCopy);
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

            File iconFile = new File(getCacheDir(), "wayland_app_icon.png");
            try (FileOutputStream fos = new FileOutputStream(iconFile)) {
                fos.write(mLastOverlayIconBytes);
                fos.flush();
            }

            OverlayManager om = getSystemService(OverlayManager.class);
            if (om != null) {
                FabricatedOverlay overlay =
                        new FabricatedOverlay("WaylandDynamicAppMetaOverlay", getPackageName());
                overlay.setTargetOverlayable("WaylandDynamicAppMeta");
                overlay.setResourceValue(
                        getPackageName() + ":string/wayland_app_name",
                        TypedValue.TYPE_STRING,
                        label,
                        null);
                try (ParcelFileDescriptor pfd =
                        ParcelFileDescriptor.open(iconFile, ParcelFileDescriptor.MODE_READ_ONLY)) {
                    overlay.setResourceValue(
                            getPackageName() + ":drawable/ic_wayland_app",
                            pfd,
                            null);
                    OverlayManagerTransaction tx = OverlayManagerTransaction.newInstance();
                    tx.registerFabricatedOverlay(overlay);
                    om.commit(tx);
                    Log.i(TAG, "Committed FabricatedOverlay for dynamic Wayland app icon and label='"
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
        nativeOnSurfaceCreated(surface, mSurfaceView.getWidth(), mSurfaceView.getHeight());
        ensureDirectHostWaylandClientRunning();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeOnSurfaceChanged(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeOnSurfaceDestroyed();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (nativeOnKeyEvent(
                event.getAction(),
                event.getKeyCode(),
                event.getMetaState(),
                event.getEventTime())) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private native void nativeOnSurfaceCreated(Surface surface, int width, int height);
    private static native void nativeOnSurfaceChanged(int width, int height);
    private static native void nativeOnSurfaceDestroyed();
    private static native boolean nativeOnMotionEvent(
            int action, float x, float y, int buttonState, float vscroll, float hscroll, long eventTimeMs);
    private static native boolean nativeOnKeyEvent(
            int action, int keyCode, int metaState, long eventTimeMs);
}
