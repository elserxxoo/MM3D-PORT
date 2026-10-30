package org.triaevum.android;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.MotionEvent;
import android.view.View;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.FrameLayout;

/**
 * Dual-screen support: shows a {@link Presentation} with a {@link SurfaceView}
 * on a secondary display (e.g. the lower screen of the AYN Thor) and hands its
 * Surface to native code, which presents the 3DS bottom screen to it.
 *
 * The native renderer owns the Vulkan swapchain for this surface. Nothing here
 * touches the main game surface.
 */
public final class SecondaryDisplayPresenter {
    private static final String TAG = "TriAevum";

    public static native void nativeSecondarySurfaceChanged(Surface surface, int width, int height);
    public static native void nativeSecondarySurfaceDestroyed();
    public static native void nativeSecondaryTouch(float x, float y, int width, int height, boolean pressed);

    private static SecondaryDisplayPresenter sInstance;

    /** Called from TriAevumActivity.onResume(). */
    public static void onActivityResume(Activity activity) {
        try {
            if (sInstance == null || sInstance.mActivity != activity) {
                if (sInstance != null) {
                    sInstance.stop();
                }
                sInstance = new SecondaryDisplayPresenter(activity);
            }
            sInstance.start();
        } catch (Throwable error) {
            Log.w(TAG, "Secondary display unavailable", error);
        }
    }

    /** Called from TriAevumActivity.onPause(). */
    public static void onActivityPause() {
        try {
            if (sInstance != null) {
                sInstance.stop();
            }
        } catch (Throwable error) {
            Log.w(TAG, "Secondary display stop failed", error);
        }
    }

    private static void notifyChanged(Surface surface, int width, int height) {
        try {
            nativeSecondarySurfaceChanged(surface, width, height);
        } catch (UnsatisfiedLinkError error) {
            Log.w(TAG, "Native runtime has no secondary display support", error);
        }
    }

    private static void notifyTouch(float x, float y, int width, int height, boolean pressed) {
        try {
            nativeSecondaryTouch(x, y, width, height, pressed);
        } catch (UnsatisfiedLinkError error) {
            // Native runtime without dual-screen support.
        }
    }

    private static void notifyDestroyed() {
        try {
            nativeSecondarySurfaceDestroyed();
        } catch (UnsatisfiedLinkError error) {
            // Native runtime without dual-screen support: nothing to release.
        }
    }

    private final Activity mActivity;
    private final DisplayManager mDisplayManager;
    private Presentation mPresentation;
    private boolean mStarted;

    private final Runnable mRefreshRunnable = new Runnable() {
        @Override
        public void run() {
            refresh();
        }
    };

    private final DisplayManager.DisplayListener mListener = new DisplayManager.DisplayListener() {
        @Override
        public void onDisplayAdded(int displayId) {
            mActivity.runOnUiThread(mRefreshRunnable);
        }

        @Override
        public void onDisplayRemoved(int displayId) {
            mActivity.runOnUiThread(mRefreshRunnable);
        }

        @Override
        public void onDisplayChanged(int displayId) {
            // Size/rotation changes are delivered through surfaceChanged.
        }
    };

    public SecondaryDisplayPresenter(Activity activity) {
        mActivity = activity;
        mDisplayManager = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
    }

    public void start() {
        if (mStarted || mDisplayManager == null) {
            return;
        }
        mStarted = true;
        mDisplayManager.registerDisplayListener(mListener, null);
        refresh();
    }

    public void stop() {
        if (!mStarted) {
            return;
        }
        mStarted = false;
        if (mDisplayManager != null) {
            mDisplayManager.unregisterDisplayListener(mListener);
        }
        dismiss();
    }

    private int primaryDisplayId() {
        try {
            Display display = mActivity.getWindowManager().getDefaultDisplay();
            if (display != null) {
                return display.getDisplayId();
            }
        } catch (Exception error) {
            Log.w(TAG, "Could not resolve the primary display", error);
        }
        return Display.DEFAULT_DISPLAY;
    }

    private Display findSecondaryDisplay() {
        if (mDisplayManager == null) {
            return null;
        }
        final int primary = primaryDisplayId();
        Display best = null;
        // Prefer displays flagged as presentation-capable; fall back to any other
        // display (built-in secondary screens are often not flagged as such).
        Display[] presentation = mDisplayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        for (Display display : presentation) {
            if (display.getDisplayId() != primary && display.getState() != Display.STATE_OFF) {
                best = display;
                break;
            }
        }
        if (best == null) {
            for (Display display : mDisplayManager.getDisplays()) {
                if (display.getDisplayId() != primary && display.getState() != Display.STATE_OFF) {
                    best = display;
                    break;
                }
            }
        }
        return best;
    }

    private void refresh() {
        if (!mStarted) {
            return;
        }
        Display target = findSecondaryDisplay();
        if (mPresentation != null) {
            Display current = mPresentation.getDisplay();
            if (target != null && current != null && current.getDisplayId() == target.getDisplayId()) {
                return;  // already showing on the right display
            }
            dismiss();
        }
        if (target == null) {
            Log.i(TAG, "No secondary display available");
            return;
        }
        try {
            mPresentation = new SecondaryPresentation(mActivity, target);
            mPresentation.show();
            Log.i(TAG, "Secondary display presentation shown on display " + target.getDisplayId()
                + " (" + target.getName() + ")");
        } catch (Exception error) {
            Log.w(TAG, "Failed to show the secondary display presentation", error);
            mPresentation = null;
        }
    }

    private void dismiss() {
        if (mPresentation != null) {
            try {
                mPresentation.dismiss();
            } catch (Exception error) {
                Log.w(TAG, "Failed to dismiss the secondary display presentation", error);
            }
            mPresentation = null;
        }
        notifyDestroyed();
    }

    private static final class SecondaryPresentation extends Presentation implements SurfaceHolder.Callback {
        SecondaryPresentation(Context context, Display display) {
            super(context, display);
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            FrameLayout root = new FrameLayout(getContext());
            root.setBackgroundColor(Color.BLACK);
            SurfaceView view = new SurfaceView(getContext());
            view.getHolder().addCallback(this);
            view.setOnTouchListener(new View.OnTouchListener() {
                @Override
                public boolean onTouch(View v, MotionEvent event) {
                    final int action = event.getActionMasked();
                    if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE) {
                        notifyTouch(event.getX(0), event.getY(0), v.getWidth(), v.getHeight(), true);
                    } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                        notifyTouch(0f, 0f, v.getWidth(), v.getHeight(), false);
                    }
                    return true;
                }
            });
            root.addView(view, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            setContentView(root);
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            Log.i(TAG, "secondary surfaceCreated: " + holder.getSurface());
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            Log.i(TAG, "secondary surfaceChanged: " + width + "x" + height);
            notifyChanged(holder.getSurface(), width, height);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            Log.i(TAG, "secondary surfaceDestroyed");
            notifyDestroyed();
        }
    }
}
