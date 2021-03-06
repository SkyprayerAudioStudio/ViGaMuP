package nl.vlessert.vigamup;

import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;

import ir.mahdi.mzip.rar.Archive;
import ir.mahdi.mzip.rar.exception.RarException;
import ir.mahdi.mzip.rar.rarfile.FileHeader;

/**
 * extract an archive to the given location
 *
 * @author edmund wagner
 *
 */
public class ExtractArchive {

    private static final String LOG_TAG = "ViGaMuP extracting rar";

    public void extractArchive(File archive, File destination) {
        Archive arch = null;
        try {
            arch = new Archive(archive);
        } catch (RarException e) {
            logError(e);
        } catch (IOException e1) {
            logError(e1);
        }
        if (arch != null) {
            if (arch.isEncrypted()) {
                logWarn("archive is encrypted cannot extreact");
                return;
            }
            FileHeader fh = null;
            while (true) {
                fh = arch.nextFileHeader();
                if (fh == null) {
                    break;
                }
                String fileNameString = fh.getFileNameString();
                if (fh.isEncrypted()) {
                    logWarn("file is encrypted cannot extract: "+ fileNameString);
                    continue;
                }
                logInfo("extracting: " + fileNameString);
                try {
                    if (fh.isDirectory()) {
                        createDirectory(fh, destination);
                    } else {
                        File f = createFile(fh, destination);
                        OutputStream stream = new FileOutputStream(f);
                        arch.extractFile(fh, stream);
                        stream.close();
                    }
                } catch (IOException e) {
                    logError(e, "error extracting the file");
                } catch (RarException e) {
                    logError(e,"error extraction the file");
                }
            }
        }
    }

    public void extractFileFromArchive(File archive, File fileNameToExtract, File destination){
        Archive arch = null;
        OutputStream nullOutputStream = new OutputStream() { @Override public void write(int b) { } };
        boolean fileFound = false;

        try {
            arch = new Archive(archive);
        } catch (RarException e) {
            logError(e);
        } catch (IOException e1) {
            logError(e1);
        }
        if (arch != null) {
            if (arch.isEncrypted()) {
                logWarn("archive is encrypted cannot extreact");
                return;
            }
            FileHeader fh = null;
            while (true) {
                fh = arch.nextFileHeader();
                if (fh == null) {
                    break;
                }
                String fileNameString = fh.getFileNameString();
                if (fh.isEncrypted()) {
                    logWarn("file is encrypted cannot extract: " + fileNameString);
                    continue;
                }
                //Log.d(LOG_TAG, "fileNameToExtract: "+ fileNameToExtract + ", fileNameString: " + fileNameString);
                if (fileNameToExtract.toString().equals(fileNameString)) {
                    logInfo("extracting: " + fileNameString);
                    fileFound = true;

                    try {
                        if (fh.isDirectory()) {
                            createDirectory(fh, destination);
                        } else {
                            File f = createFile(fh, destination);
                            OutputStream stream = new FileOutputStream(f);
                            arch.extractFile(fh, stream);
                            stream.close();
                        }
                    } catch (IOException e) {
                        logError(e, "error extracting the file");
                    } catch (RarException e) {
                        logError(e, "error extraction the file");
                    }
                    Log.d(LOG_TAG, "extracted: " + fileNameString);

                } else {
                    try {
                        arch.extractFile(fh, nullOutputStream);
                        //logInfo ("Skipping " + fh.getFileNameString());
                    } catch (RarException e) {
                        logError(e, "error extraction the file");
                    }
                }
            }
        }
        if (!fileFound) logInfo("File not found in archive it seems...");
    }

    private void logWarn(String warning) {
        Log.d(LOG_TAG, warning);
    }

    private void logInfo(String info) {
        Log.d(LOG_TAG, info);
    }

    private void logError(Exception e, String errorMessage) {
        Log.d(LOG_TAG, errorMessage + e);
    }

    private void logError(Exception e) {
        Log.d(LOG_TAG, e.getMessage() + e);
    }

    private File createFile(FileHeader fh, File destination) {
        File f = null;
        String name = null;
        if (fh.isFileHeader() && fh.isUnicode()) {
            name = fh.getFileNameW();
        } else {
            name = fh.getFileNameString();
        }
        f = new File(destination, name);
        if (!f.exists()) {
            try {
                f = makeFile(destination, name);
            } catch (IOException e) {
                logError(e, "error creating the new file: " + f.getName());
            }
        }
        return f;
    }

    private static File makeFile(File destination, String name)
            throws IOException {
        String[] dirs = name.split("\\\\");
        if (dirs == null) {
            return null;
        }
        String path = "";
        int size = dirs.length;
        if (size == 1) {
            return new File(destination, name);
        } else if (size > 1) {
            for (int i = 0; i < dirs.length - 1; i++) {
                path = path + File.separator + dirs[i];
                new File(destination, path).mkdir();
            }
            path = path + File.separator + dirs[dirs.length - 1];
            File f = new File(destination, path);
            f.createNewFile();
            return f;
        } else {
            return null;
        }
    }

    private static void createDirectory(FileHeader fh, File destination) {
        File f = null;
        if (fh.isDirectory() && fh.isUnicode()) {
            f = new File(destination, fh.getFileNameW());
            if (!f.exists()) {
                makeDirectory(destination, fh.getFileNameW());
            }
        } else if (fh.isDirectory() && !fh.isUnicode()) {
            f = new File(destination, fh.getFileNameString());
            if (!f.exists()) {
                makeDirectory(destination, fh.getFileNameString());
            }
        }
    }

    private static void makeDirectory(File destination, String fileName) {
        String[] dirs = fileName.split("\\\\");
        if (dirs == null) {
            return;
        }
        String path = "";
        for (String dir : dirs) {
            path = path + File.separator + dir;
            new File(destination, path).mkdir();
        }

    }
}