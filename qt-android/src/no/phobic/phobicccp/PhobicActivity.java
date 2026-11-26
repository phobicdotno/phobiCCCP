// The pad's activity: Qt's own, plus the Android ways files get in and out of
// an app. Android's file picker, "Open with" and "Share" hand over content://
// links, which a design (an SQLite file) cannot be opened through, so every
// file that comes in is copied into the app's cache first and its path given
// to the program (src/androidfiles.cpp); the program decides what it is and
// where it is kept (src/incomingfiles.cpp). Going out, a file the program
// wrote is shared through the app's FileProvider or copied to wherever the
// user picks in Android's save dialog.

package no.phobic.phobicccp;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;

import androidx.core.content.FileProvider;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;

import org.qtproject.qt.android.bindings.QtActivity;

public class PhobicActivity extends QtActivity
{
    // Implemented in src/androidfiles.cpp. `purpose`: 0 open, 1 import,
    // 2 a file another app opened or shared with phobiCCCP.
    private static native void fileArrived(String path, String displayName, int purpose);
    private static native void fileMessage(String text, boolean error);

    private static final int REQUEST_PICK = 0x5C00;   // + purpose
    private static final int REQUEST_SAVE = 0x5C10;
    private static final int PURPOSE_INCOMING = 2;
    // Bigger than any design or drawing; a picked film should not fill the tablet.
    private static final long MAX_BYTES = 512L * 1024 * 1024;

    // Set once the program can take files (nativeReady); until then the
    // intent that started the app waits in getIntent().
    private boolean m_ready = false;
    private String m_pendingSave = null;

    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onNewIntent(Intent intent)
    {
        super.onNewIntent(intent);
        setIntent(intent);
        if (m_ready)
            handleIntent(intent);
    }

    // Called by the program when its window is up and listening.
    public void nativeReady()
    {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (m_ready)
                    return;
                m_ready = true;
                // What came in last time is open or imported by now: cleared
                // before the file that started the app (if any) is copied in.
                final Uri first = takeIncoming(getIntent());
                new Thread(new Runnable() {
                    @Override public void run() {
                        deleteTree(new File(getCacheDir(), "incoming"), false);
                        if (first != null)
                            copyIn(first, PURPOSE_INCOMING);
                    }
                }).start();
            }
        });
    }

    // Android's picker, to open or import a file from anywhere (Downloads,
    // Drive, a USB stick).
    public void pickFile(final int purpose)
    {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                // Android has no type for .c2d and guesses wrong for DXF: show everything.
                i.setType("*/*");
                try {
                    startActivityForResult(i, REQUEST_PICK + purpose);
                } catch (ActivityNotFoundException e) {
                    fileMessage("This tablet has no file picker", true);
                }
            }
        });
    }

    // The share sheet for files the program wrote (one path per line).
    public void shareFiles(final String paths, final String mimeType, final String title)
    {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                ArrayList<Uri> uris = new ArrayList<Uri>();
                try {
                    for (String p : paths.split("\n"))
                        if (!p.isEmpty())
                            uris.add(FileProvider.getUriForFile(PhobicActivity.this,
                                                                getPackageName() + ".qtprovider",
                                                                new File(p)));
                } catch (IllegalArgumentException e) {
                    fileMessage("Cannot share a file from that folder", true);
                    return;
                }
                if (uris.isEmpty())
                    return;
                Intent i;
                if (uris.size() == 1) {
                    i = new Intent(Intent.ACTION_SEND);
                    i.putExtra(Intent.EXTRA_STREAM, uris.get(0));
                } else {
                    i = new Intent(Intent.ACTION_SEND_MULTIPLE);
                    i.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
                }
                i.setType(mimeType);
                ClipData clip = ClipData.newRawUri(title, uris.get(0));
                for (int n = 1; n < uris.size(); ++n)
                    clip.addItem(new ClipData.Item(uris.get(n)));
                i.setClipData(clip);
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                try {
                    startActivity(Intent.createChooser(i, title));
                } catch (ActivityNotFoundException e) {
                    fileMessage("No app on this tablet can take it", true);
                }
            }
        });
    }

    // Android's save dialog: the user picks a place (Downloads, Drive, a USB
    // stick) and a name, and the file at `path` is copied there.
    public void saveCopy(final String path, final String mimeType, final String suggestedName)
    {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                m_pendingSave = path;
                Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType(mimeType);
                i.putExtra(Intent.EXTRA_TITLE, suggestedName);
                try {
                    startActivityForResult(i, REQUEST_SAVE);
                } catch (ActivityNotFoundException e) {
                    m_pendingSave = null;
                    fileMessage("This tablet has no save dialog", true);
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data)
    {
        if (requestCode == REQUEST_SAVE) {
            final String from = m_pendingSave;
            m_pendingSave = null;
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null && from != null) {
                final Uri to = data.getData();
                new Thread(new Runnable() {
                    @Override public void run() { copyOut(from, to); }
                }).start();
            }
            return;
        }
        if (requestCode >= REQUEST_PICK && requestCode < REQUEST_PICK + 2) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null)
                copyInLater(data.getData(), requestCode - REQUEST_PICK);
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // "Open with" from the Files app or a browser's downloads, or "Share"
    // from another app. Handled once: a rotation recreates nothing here, but
    // an intent kept in getIntent() would otherwise come back on a restart.
    private void handleIntent(Intent intent)
    {
        Uri uri = takeIncoming(intent);
        if (uri != null)
            copyInLater(uri, PURPOSE_INCOMING);
    }

    // The file an intent brings, if any; the intent is then marked as handled.
    private Uri takeIncoming(Intent intent)
    {
        if (intent == null)
            return null;
        Uri uri = null;
        String action = intent.getAction();
        if (Intent.ACTION_VIEW.equals(action)) {
            uri = intent.getData();
        } else if (Intent.ACTION_SEND.equals(action)) {
            Object s = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            if (s instanceof Uri)
                uri = (Uri) s;
            else if (intent.getClipData() != null && intent.getClipData().getItemCount() > 0)
                uri = intent.getClipData().getItemAt(0).getUri();
        }
        if (uri != null)
            setIntent(new Intent(Intent.ACTION_MAIN));
        return uri;
    }

    private void copyInLater(final Uri uri, final int purpose)
    {
        new Thread(new Runnable() {
            @Override public void run() { copyIn(uri, purpose); }
        }).start();
    }

    private void copyIn(Uri uri, int purpose)
    {
        String name = displayName(uri);
        // A folder of its own, so two files of one name never meet.
        File dir = new File(new File(getCacheDir(), "incoming"), Long.toString(System.nanoTime()));
        dir.mkdirs();
        File out = new File(dir, safeName(name));
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream os = new FileOutputStream(out)) {
            if (in == null)
                throw new IOException("nothing to read");
            byte[] buf = new byte[1 << 16];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
                if (total > MAX_BYTES)
                    throw new IOException("the file is too big");
                os.write(buf, 0, n);
            }
        } catch (IOException | SecurityException e) {
            deleteTree(dir, true);
            fileMessage("Could not read " + name + ": " + e.getMessage(), true);
            return;
        }
        fileArrived(out.getAbsolutePath(), name, purpose);
    }

    private void copyOut(String from, Uri to)
    {
        String name = displayName(to);
        OutputStream os = null;
        try (InputStream in = new FileInputStream(from)) {
            try {
                os = getContentResolver().openOutputStream(to, "wt");
            } catch (FileNotFoundException | IllegalArgumentException | UnsupportedOperationException e) {
                os = getContentResolver().openOutputStream(to, "w");   // a provider without truncate
            }
            if (os == null)
                throw new IOException("nowhere to write");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0)
                os.write(buf, 0, n);
            os.close();
            os = null;
            fileMessage("Saved " + name, false);
        } catch (IOException | SecurityException e) {
            fileMessage("Could not save " + name + ": " + e.getMessage(), true);
        } finally {
            if (os != null)
                try { os.close(); } catch (IOException e) { }
        }
    }

    private String displayName(Uri uri)
    {
        String name = null;
        if ("content".equals(uri.getScheme())) {
            try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME },
                                                       null, null, null)) {
                if (c != null && c.moveToFirst() && !c.isNull(0))
                    name = c.getString(0);
            } catch (RuntimeException e) {
                name = null;
            }
        }
        if (name == null || name.isEmpty())
            name = uri.getLastPathSegment();
        return name == null || name.isEmpty() ? "file" : name;
    }

    // The program cleans the name properly; this only keeps it one file in `dir`.
    private static String safeName(String name)
    {
        String s = name.replace('/', '_').replace('\\', '_').replace('\0', '_');
        if (s.isEmpty() || s.equals(".") || s.equals(".."))
            s = "file";
        return s.length() > 200 ? s.substring(s.length() - 200) : s;
    }

    private static void deleteTree(File f, boolean self)
    {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids)
                deleteTree(k, true);
        if (self)
            f.delete();
    }
}
