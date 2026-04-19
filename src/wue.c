/*
 * wue.c: Windows User Experience helpers, Linux port.
 *
 * On Windows the equivalent file in upstream Rufus pokes the install.wim
 * registry hives directly (via wimlib) so the bypass keys are present
 * before setup ever boots.  On Linux we take the lighter route that
 * Rufus also falls back to: drop an `autounattend.xml` at the root of
 * the install media.  Windows Setup picks it up on first boot and runs
 * the requested `reg add` lines synchronously during the windowsPE
 * pass, before the TPM/SecureBoot/RAM checks run.
 *
 * The XML below is a trimmed version of upstream's CreateUnattendXml().
 * The flags map 1:1 onto the upstream UNATTEND_* bitmask values so the
 * UI code reads the same way.
 */
#include "rufus.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Same names and DWORD targets Microsoft's setup looks for.
 * Setting all three to 1 in HKLM\SYSTEM\Setup\LabConfig disables the
 * Windows 11 hardware gating without modifying the install image. */
static const char *bypass_name[] = {
    "BypassTPMCheck",
    "BypassSecureBootCheck",
    "BypassRAMCheck",
    "BypassCPUCheck",
    "BypassStorageCheck",
};

bool wue_is_windows_iso(const char *extract_root)
{
    if (!extract_root) return false;
    /* Setup ISOs always carry sources/install.wim or .esd. */
    char p[MAX_PATH_LEN];
    struct stat st;
    static const char *probes[] = {
        "sources/install.wim",
        "sources/install.esd",
        "sources/boot.wim",
        NULL,
    };
    for (int i = 0; probes[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", extract_root, probes[i]);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) return true;
    }
    return false;
}

/* Build autounattend.xml at <extract_root>/autounattend.xml.
 * `flags` is the bitmask of WUE_* options. */
int wue_write_unattend(const char *extract_root, uint32_t flags)
{
    if (!extract_root || flags == 0) return 0;

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof path, "%s/autounattend.xml", extract_root);
    FILE *f = fopen(path, "w");
    if (!f) {
        rufus_log("wue: cannot create %s", path);
        return -1;
    }

    rufus_log("wue: writing autounattend.xml with flags=0x%x", flags);
    fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    fprintf(f, "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");

    /* windowsPE pass: TPM / SecureBoot / RAM / CPU / storage bypass.
     * These run before Setup performs hardware checks. */
    if (flags & WUE_BYPASS_HARDWARE_CHECKS) {
        fprintf(f, "  <settings pass=\"windowsPE\">\n");
        fprintf(f, "    <component name=\"Microsoft-Windows-Setup\" "
                   "processorArchitecture=\"amd64\" language=\"neutral\" "
                   "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
                   "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                   "publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n");
        fprintf(f, "      <RunSynchronous>\n");
        int order = 1;
        for (size_t i = 0; i < sizeof bypass_name / sizeof *bypass_name; i++) {
            fprintf(f, "        <RunSynchronousCommand wcm:action=\"add\">\n");
            fprintf(f, "          <Order>%d</Order>\n", order++);
            fprintf(f, "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig "
                       "/v %s /t REG_DWORD /d 1 /f</Path>\n", bypass_name[i]);
            fprintf(f, "        </RunSynchronousCommand>\n");
        }
        fprintf(f, "      </RunSynchronous>\n");
        fprintf(f, "    </component>\n");
        fprintf(f, "  </settings>\n");
    }

    /* specialize pass: bypass online account requirement on first boot. */
    if (flags & WUE_BYPASS_ONLINE_ACCOUNT) {
        fprintf(f, "  <settings pass=\"specialize\">\n");
        fprintf(f, "    <component name=\"Microsoft-Windows-Deployment\" "
                   "processorArchitecture=\"amd64\" language=\"neutral\" "
                   "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
                   "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                   "publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n");
        fprintf(f, "      <RunSynchronous>\n");
        fprintf(f, "        <RunSynchronousCommand wcm:action=\"add\">\n");
        fprintf(f, "          <Order>1</Order>\n");
        fprintf(f, "          <Path>reg add \"HKLM\\Software\\Microsoft\\Windows"
                   "\\CurrentVersion\\OOBE\" /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n");
        fprintf(f, "        </RunSynchronousCommand>\n");
        fprintf(f, "      </RunSynchronous>\n");
        fprintf(f, "    </component>\n");
        fprintf(f, "  </settings>\n");
    }

    /* oobeSystem pass: skip privacy / EULA prompts. */
    if (flags & WUE_DISABLE_DATA_COLLECTION) {
        fprintf(f, "  <settings pass=\"oobeSystem\">\n");
        fprintf(f, "    <component name=\"Microsoft-Windows-Shell-Setup\" "
                   "processorArchitecture=\"amd64\" language=\"neutral\" "
                   "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
                   "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                   "publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n");
        fprintf(f, "      <OOBE>\n");
        fprintf(f, "        <HideEULAPage>true</HideEULAPage>\n");
        fprintf(f, "        <ProtectYourPC>3</ProtectYourPC>\n");
        fprintf(f, "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n");
        fprintf(f, "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n");
        fprintf(f, "      </OOBE>\n");
        fprintf(f, "    </component>\n");
        fprintf(f, "  </settings>\n");
    }

    fprintf(f, "</unattend>\n");
    fclose(f);
    chmod(path, 0644);
    return 0;
}
