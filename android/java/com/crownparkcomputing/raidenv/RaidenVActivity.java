package com.crownparkcomputing.raidenv;

import org.libsdl.app.SDLActivity;
import android.os.Bundle;
import java.io.File;
import com.crownparkcomputing.ps4runtime.GraphicsDriverSettings;

/** SDL owns the surface, gamepad and audio device; libmain runs the port. */
public class RaidenVActivity extends SDLActivity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        GraphicsDriverSettings.registerShortcut(this);
    }
    public void showGraphicsDriverSettings() {
        runOnUiThread(() -> startActivity(new android.content.Intent(this,
            com.crownparkcomputing.ps4runtime.GraphicsDriverActivity.class)));
    }
    public String getGraphicsDriverPath() {
        return GraphicsDriverSettings.selectedLibraryPath(this);
    }
    private static boolean hasGameData(File directory) {
        return directory != null
            && new File(directory, "boot-linked.bin").isFile()
            && new File(directory, "app0/eboot.bin").isFile();
    }
    /** Game data may occupy our SD-card directory; settings and saves stay on primary. */
    public String getGameDataDirectory() {
        File primary = getExternalFilesDir(null);
        if (hasGameData(primary)) return primary.getAbsolutePath();
        File[] directories = getExternalFilesDirs(null);
        if (directories != null) {
            for (File directory : directories) {
                if (hasGameData(directory)) return directory.getAbsolutePath();
            }
        }
        return primary == null ? null : primary.getAbsolutePath();
    }
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
