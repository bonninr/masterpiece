; Masterpiece's own files, removed at the player's choice (#1166): settings,
; logs, the sample caches and the organs unpacked from their archives. The
; organs folder and the organs installed in it stay.
;
; Runs inside the Uninstall section, so no Function and no FileFunc: labels
; only. %APPDATA% is read from the environment: CPack sets the shell context
; to all users for an all-users install, and $APPDATA then names ProgramData.
;
; Silent (/S) keeps the files; /S /REMOVEDATA removes them.

StrCpy $R5 0
StrCpy $R6 0
mp_data_scan:
  StrCpy $R7 $CMDLINE 11 $R6
  StrCmp $R7 "" mp_data_scanned
  StrCmp $R7 "/REMOVEDATA" 0 +3
    StrCpy $R5 1
    Goto mp_data_scanned
  IntOp $R6 $R6 + 1
  Goto mp_data_scan
mp_data_scanned:

StrCmp $R5 1 mp_data_remove
MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also remove Masterpiece's settings, logs, sample caches and unpacked organs?$\r$\n$\r$\nInstalled organs and the organs folder are kept." /SD IDNO IDYES mp_data_remove
Goto mp_data_kept

mp_data_remove:
  ExpandEnvStrings $R0 "%APPDATA%\Masterpiece"
  ; A cache folder chosen on another drive: its caches and unpacked organs
  ; only, never the rest of that folder.
  StrCpy $R1 ""
  ClearErrors
  FileOpen $R2 "$R0\settings.mpglobal" r
  IfErrors mp_data_no_global
  mp_data_line:
    ClearErrors
    FileRead $R2 $R3
    IfErrors mp_data_lines_done
    StrCpy $R4 $R3 9
    StrCmp $R4 "cachedir " 0 mp_data_line
    StrCpy $R1 $R3 "" 9
    Goto mp_data_line
  mp_data_lines_done:
  FileClose $R2
  mp_data_no_global:
  ; The line's CR and LF.
  mp_data_trim:
    StrCpy $R4 $R1 1 -1
    StrCmp $R4 "$\r" mp_data_cut
    StrCmp $R4 "$\n" mp_data_cut mp_data_trimmed
    mp_data_cut:
      StrCpy $R1 $R1 -1
      Goto mp_data_trim
  mp_data_trimmed:
  StrCmp $R1 "" mp_data_own
    Delete "$R1\*.mpcache"
    Delete "$R1\*.mpcache.part"
    RMDir /r "$R1\Packaged"
    RMDir "$R1"
  mp_data_own:
  RMDir /r "$R0"
mp_data_kept:
