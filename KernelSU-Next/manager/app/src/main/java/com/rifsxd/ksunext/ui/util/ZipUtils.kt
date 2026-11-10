package com.rifsxd.ksunext.ui.util

import android.content.Context
import android.net.Uri
import java.io.BufferedInputStream
import java.util.zip.ZipInputStream

object ZipUtils {

    private const val MAX_SCAN_ENTRIES = 300
    private const val ANYKERNEL_MARKER = "anykernel.sh"

    /**
     * Detects whether the given zip is an AnyKernel3 package by looking for a
     * root-level "anykernel.sh" entry. Anything unreadable or non-zip is
     * treated as not-AnyKernel (i.e. a regular module).
     */
    fun isAnyKernel3Zip(context: Context, uri: Uri): Boolean {
        return try {
            context.contentResolver.openInputStream(uri)?.use { inputStream ->
                ZipInputStream(BufferedInputStream(inputStream)).use { zip ->
                    var found = false
                    var scanned = 0
                    while (!found && scanned < MAX_SCAN_ENTRIES) {
                        val entry = zip.nextEntry ?: break
                        if (!entry.isDirectory &&
                            entry.name.trimStart('.', '/').equals(ANYKERNEL_MARKER, ignoreCase = true)
                        ) {
                            found = true
                        }
                        scanned++
                    }
                    found
                }
            } ?: false
        } catch (ignored: Exception) {
            false
        }
    }
}
