/* What the installer does that is particular to Robot City.
 *
 * The game comes on two CDs.  Both are copied into one folder,
 * <install>\game\cd (the game's D: drive), so no disc is ever asked for
 * again: the game looks for RCID.000 / RCID.001 at the CD root to know which
 * disc is in, and both are there after the merge.
 * <install>\game\hd\RC is C:\RC, what the original setup (SETUP.INS) put on
 * the hard disk: ROBOTCTY.EXE, SCIFI.EXE and RCP.DAT, a copy of
 * I_COMP\<drive letter>.DAT naming the CD drive (D:\). */
#include "setup.h"
#include <string.h>
#include "install.h"

int game_is_disc(CdSrc *c, int n)
{
    static const char *const files[GAME_DISCS][4] = GAME_DISC_FILES;
    if (!c || n < 0 || n >= GAME_DISCS) return 0;
    for (int i = 0; files[n][i]; i++)
        if (!cd_exists(c, files[n][i])) return 0;
    return 1;
}

int game_install_files(const char *cd, const char *hd)
{
    char rc[MAX_PATH * 2], s[MAX_PATH * 3], d[MAX_PATH * 3];
    snprintf(rc, sizeof rc, "%s\\RC", hd);
    if (!make_dirs(rc)) { setup_log("ERROR: cannot create %s", rc); return 0; }
    static const char *const prog[] = { "ROBOTCTY.EXE", "SCIFI.EXE" };
    for (int i = 0; i < 2; i++) {
        snprintf(s, sizeof s, "%s\\RC\\%s", cd, prog[i]);
        snprintf(d, sizeof d, "%s\\%s", rc, prog[i]);
        if (!CopyFileA(s, d, FALSE)) {
            if (i == 0) {
                MessageBoxA(NULL, "RC\\ROBOTCTY.EXE is missing on disc 1.", GAME_TITLE " setup", MB_ICONERROR);
                return 0;
            }
            continue;
        }
        SetFileAttributesA(d, FILE_ATTRIBUTE_NORMAL);   /* files from a CD are read-only */
    }
    /* RCP.DAT: the CD is drive D: (the original setup copied I_COMP\D.DAT) */
    snprintf(d, sizeof d, "%s\\RCP.DAT", rc);
    static const char rcp[] = { 'D', ':', 92, '\r', '\n', '\r', '\n' };
    if (!write_file(d, rcp, sizeof rcp)) { setup_log("ERROR: cannot write %s", d); return 0; }
    setup_log("game files set up in %s", rc);
    return 1;
}

/* the exe's icon: I_COMP\RC.ICO on disc 1 */
const char *game_icon_exe(void) { return NULL; }
const char *game_icon_lbm(void) { return GAME_ICON_LBM; }
const char *game_icon_ico(void) { return GAME_ICON_ICO; }
