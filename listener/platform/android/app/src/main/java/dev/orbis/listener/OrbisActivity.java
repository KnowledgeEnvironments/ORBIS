package dev.orbis.listener;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * SDL's activity, plus the two things the C++ cannot see from SDL: orbis:// links whole (SDL only
 * forwards an intent's path) and the keyboard's height (OrbisSurface).
 */
public class OrbisActivity extends SDLActivity {
    private static String sPendingLink; // a link that launched the app, held until SDL has a queue

    /** Pixels of the window the on-screen keyboard covers; see src/platform/KeyboardAndroid.cpp. */
    static native void nativeKeyboardHeight(int pixels);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sPendingLink = takeLink(getIntent());
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= 30) {
            // Android 15 forces this at target SDK 35: draw under the bars and take the keyboard
            // as an inset. Asking for it here gives 11 to 14 the same layout.
            getWindow().setDecorFitsSystemWindows(false);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String link = takeLink(intent);
        if (link != null) {
            SDLActivity.onNativeDropFile(link);
        }
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new OrbisSurface(context);
    }

    /** Called once from the SDL thread, after SDL_Init, so the link reaches the event queue. */
    public void deliverPendingLink() {
        String link = sPendingLink;
        sPendingLink = null;
        if (link != null) {
            SDLActivity.onNativeDropFile(link);
        }
    }

    // The data comes off the intent so SDLActivity does not also drop its bare path as a file.
    private static String takeLink(Intent intent) {
        Uri uri = intent == null ? null : intent.getData();
        if (uri == null || !"orbis".equals(uri.getScheme())) {
            return null;
        }
        intent.setData(null);
        return uri.toString();
    }
}
