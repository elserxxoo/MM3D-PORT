package org.triaevum.android;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

/**
 * Feeds the device's gyroscope/accelerometer to the native runtime so the
 * game's motion aiming (first-person view with the slingshot, bow, hookshot...)
 * works like on the original console. Enabled by default (override with the preference key).
 *
 * Samples are rotated into the screen frame (X right, Y up, Z toward the
 * player) according to the current display rotation, which is the frame the
 * native side expects (same convention as SDL game controllers).
 */
public final class MotionSensorBridge implements SensorEventListener {
    private static final String TAG = "TriAevum";
    public static final String PREF_KEY = "EmulationMenuSettings_GyroEnabled";
    private static final String PREFS_NAME = "org.triaevum.android_preferences";
    private static final int SAMPLING_PERIOD_US = 10000;  // 100 Hz, no special permission needed

    public static native void nativeSetMotionEnabled(boolean enabled);
    public static native void nativeMotionSample(float gx, float gy, float gz, float ax, float ay, float az);
    public static native void nativeMotionStopped();

    private static MotionSensorBridge sInstance;

    private final Activity mActivity;
    private final SensorManager mSensorManager;
    private final Sensor mGyroscope;
    private final Sensor mAccelerometer;
    private HandlerThread mThread;
    private boolean mRegistered;

    // Latest accelerometer sample in screen frame (default: device upright at rest).
    private float mAccelX = 0f;
    private float mAccelY = 9.80665f;
    private float mAccelZ = 0f;
    private final float[] mTmp = new float[2];

    private MotionSensorBridge(Activity activity) {
        mActivity = activity;
        mSensorManager = (SensorManager) activity.getSystemService(Context.SENSOR_SERVICE);
        mGyroscope = mSensorManager != null ? mSensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE) : null;
        mAccelerometer = mSensorManager != null ? mSensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) : null;
    }

    /** True when the device has a gyroscope. */
    public static boolean hasGyroscope(Context context) {
        SensorManager manager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        return manager != null && manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null;
    }

    private static boolean isEnabledInPrefs(Context context) {
        SharedPreferences prefs = context.getApplicationContext()
            .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        return prefs.getBoolean(PREF_KEY, true);  // no settings menu in this port: on when a gyroscope exists
    }

    /** Called from TriAevumActivity.onResume(). */
    public static void onActivityResume(Activity activity) {
        try {
            if (sInstance == null || sInstance.mActivity != activity) {
                if (sInstance != null) {
                    sInstance.unregister();
                }
                sInstance = new MotionSensorBridge(activity);
            }
            refresh();
        } catch (Throwable error) {
            Log.w(TAG, "Motion sensors unavailable", error);
        }
    }

    /** Called from TriAevumActivity.onPause(). */
    public static void onActivityPause() {
        try {
            if (sInstance != null) {
                sInstance.unregister();
            }
        } catch (Throwable error) {
            Log.w(TAG, "Motion sensors stop failed", error);
        }
    }

    /** Re-reads the preference and starts/stops the sensors accordingly. */
    public static void refresh() {
        try {
            MotionSensorBridge instance = sInstance;
            if (instance == null) {
                return;
            }
            boolean enabled = isEnabledInPrefs(instance.mActivity) && instance.mGyroscope != null;
            notifyEnabled(enabled);
            if (enabled) {
                instance.register();
            } else {
                instance.unregister();
            }
        } catch (Throwable error) {
            Log.w(TAG, "Motion refresh failed", error);
        }
    }

    private static void notifyEnabled(boolean enabled) {
        try {
            nativeSetMotionEnabled(enabled);
        } catch (UnsatisfiedLinkError error) {
            Log.w(TAG, "Native runtime has no motion support", error);
        }
    }

    private synchronized void register() {
        if (mRegistered || mSensorManager == null || mGyroscope == null) {
            return;
        }
        mThread = new HandlerThread("TriAevumMotion");
        mThread.start();
        Handler handler = new Handler(mThread.getLooper());
        mSensorManager.registerListener(this, mGyroscope, SAMPLING_PERIOD_US, handler);
        if (mAccelerometer != null) {
            mSensorManager.registerListener(this, mAccelerometer, SAMPLING_PERIOD_US, handler);
        }
        mRegistered = true;
        Log.i(TAG, "Motion sensors registered (gyroscope" + (mAccelerometer != null ? " + accelerometer" : "") + ")");
    }

    private synchronized void unregister() {
        if (!mRegistered) {
            return;
        }
        mRegistered = false;
        if (mSensorManager != null) {
            mSensorManager.unregisterListener(this);
        }
        if (mThread != null) {
            mThread.quitSafely();
            mThread = null;
        }
        try {
            nativeMotionStopped();
        } catch (UnsatisfiedLinkError error) {
            // Native runtime without motion support.
        }
    }

    /** Rotates a device-frame (x, y) pair into the screen frame for the given display rotation. */
    private void remap(int rotation, float x, float y) {
        switch (rotation) {
            case Surface.ROTATION_90:   // device turned 90 degrees counter-clockwise
                mTmp[0] = -y;
                mTmp[1] = x;
                break;
            case Surface.ROTATION_180:
                mTmp[0] = -x;
                mTmp[1] = -y;
                break;
            case Surface.ROTATION_270:  // device turned 90 degrees clockwise
                mTmp[0] = y;
                mTmp[1] = -x;
                break;
            default:
                mTmp[0] = x;
                mTmp[1] = y;
                break;
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        final int rotation = mActivity.getWindowManager().getDefaultDisplay().getRotation();
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            remap(rotation, event.values[0], event.values[1]);
            mAccelX = mTmp[0];
            mAccelY = mTmp[1];
            mAccelZ = event.values[2];
        } else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) {
            remap(rotation, event.values[0], event.values[1]);
            final float gx = mTmp[0];
            final float gy = mTmp[1];
            final float gz = event.values[2];
            try {
                nativeMotionSample(gx, gy, gz, mAccelX, mAccelY, mAccelZ);
            } catch (UnsatisfiedLinkError error) {
                // Native runtime without motion support; ignore.
            }
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }
}
