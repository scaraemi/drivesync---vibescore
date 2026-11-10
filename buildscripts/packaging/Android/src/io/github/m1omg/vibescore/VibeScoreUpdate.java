/*
 * SPDX-License-Identifier: GPL-3.0-only
 * VibeScore - MuseScore Studio for mobile devices
 *
 * Copyright (C) 2026 VibeScore contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
package io.github.m1omg.vibescore;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import androidx.core.content.FileProvider;

import java.io.File;

/**
 * Installs a downloaded update (APK) over the app with the system installer. Called from native code.
 * Android asks the user once to allow the app to install apps ("Install unknown apps").
 */
public class VibeScoreUpdate
{
    private static final String TAG = "VibeScore";
    private static final String PROVIDER_SUFFIX = ".qtprovider";

    public static boolean canInstallPackages(Context context)
    {
        if (context == null) {
            return false;
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return true;
        }
        return context.getPackageManager().canRequestPackageInstalls();
    }

    /** The system page where the user allows this app to install apps. Returns an error, or "" */
    public static String openInstallSettings(Context context)
    {
        if (context == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return "";
        }
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                       Uri.parse("package:" + context.getPackageName()));
            if (!(context instanceof Activity)) {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            }
            context.startActivity(intent);
            return "";
        } catch (Exception e) {
            Log.e(TAG, "Update: install settings: " + e);
            return e.toString();
        }
    }

    /** Hands the APK to the system installer. Returns an error, or "" */
    public static String installApk(Context context, String path)
    {
        if (context == null || path == null) {
            return "No context";
        }
        try {
            File file = new File(path);
            if (!file.isFile()) {
                return "Missing file: " + path;
            }
            Uri uri = FileProvider.getUriForFile(context, context.getPackageName() + PROVIDER_SUFFIX, file);
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(uri, "application/vnd.android.package-archive");
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            intent.putExtra(Intent.EXTRA_NOT_UNKNOWN_SOURCE, true);
            if (!(context instanceof Activity)) {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            }
            context.startActivity(intent);
            Log.i(TAG, "Update: installer started for " + uri);
            return "";
        } catch (Exception e) {
            Log.e(TAG, "Update: install: " + e);
            return e.toString();
        }
    }
}
