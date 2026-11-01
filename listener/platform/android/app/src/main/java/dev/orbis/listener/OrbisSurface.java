package dev.orbis.listener;

import android.content.Context;
import android.graphics.Insets;
import android.os.Build;
import android.view.View;
import android.view.WindowInsets;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * SDLSurface reports whether the keyboard is up; the phone frame needs how much it covers. It also
 * counts the system gesture zones as unsafe, which are for touches, not for drawing: left in, every
 * phone with gesture navigation loses a band down each side of a screen that has few columns.
 */
class OrbisSurface extends SDLSurface {
    OrbisSurface(Context context) {
        super(context);
    }

    @Override
    public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
        WindowInsets result = super.onApplyWindowInsets(v, insets);
        if (Build.VERSION.SDK_INT >= 30) {
            boolean up = insets.isVisible(WindowInsets.Type.ime());
            OrbisActivity.nativeKeyboardHeight(up ? insets.getInsets(WindowInsets.Type.ime()).bottom : 0);
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            SDLActivity.onNativeInsetsChanged(bars.left, bars.right, bars.top, bars.bottom);
        }
        return result;
    }
}
