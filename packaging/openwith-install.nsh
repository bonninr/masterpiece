; "Open with Masterpiece" for organ definitions and packages (#90). Offered in
; the Open with list, never made the default: a .rar keeps whatever opens it.
WriteRegStr HKCR "Applications\Masterpiece.exe" "FriendlyAppName" "Masterpiece"
WriteRegStr HKCR "Applications\Masterpiece.exe\shell\open\command" "" '"$INSTDIR\bin\Masterpiece.exe" "%1"'
!macro MP_OPENWITH EXT
  WriteRegStr HKCR "Applications\Masterpiece.exe\SupportedTypes" "${EXT}" ""
  WriteRegStr HKCR "${EXT}\OpenWithList\Masterpiece.exe" "" ""
!macroend
!insertmacro MP_OPENWITH ".Organ_Hauptwerk_xml"
!insertmacro MP_OPENWITH ".CustomOrgan_Hauptwerk_xml"
!insertmacro MP_OPENWITH ".organ"
!insertmacro MP_OPENWITH ".orgue"
!insertmacro MP_OPENWITH ".rar"
