/* The game's names (runtime and installer) */
#pragma once
#define GAME_TITLE "Isaac Asimov's Robot City"
#define GAME_TITLE_FS "Robot City"   /* for file / folder / shortcut names */
#define GAME_SHORT "robotcity"
#define GAME_ID    "RobotCity"
#define GAME_ENV   "RC_"
#define WIDESCREEN_ENABLED 0
/* the game's two CDs and files that identify each (installer) */
#define GAME_DISCS 2
#define GAME_DISC_FILES { \
    { "RCID.000", "RC\\ROBOTCTY.EXE", NULL }, \
    { "RCID.001", "RC\\WITNESS", NULL } }
#define GAME_ICON_ICO "I_COMP\\RC.ICO"
#define GAME_ICON_LBM NULL
/* the 16-bit game program and RCP.DAT (CD drive letter) live in C:\RC like setup left them */
#define GAME_EXE_PATH "RC\\ROBOTCTY.EXE"
#define GAME_EXE_DOS  "C:\\RC\\ROBOTCTY.EXE"
