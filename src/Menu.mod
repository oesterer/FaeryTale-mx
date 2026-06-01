IMPLEMENTATION MODULE Menu;

FROM Strings IMPORT Assign;
FROM Items IMPORT InventoryCount,
                  ItemGold, ItemFood, ItemKey, ItemSword,
                  ItemShield, ItemPotion, ItemGem, ItemScroll;
FROM Brothers IMPORT brothers, activeBrother, HasStuff, HasWeapon;

(* Category tab labels — always shown as top 5 in each menu *)
CONST
  TabLabels = "ItemsMagicTalk Buy  Game ";

(* Per-mode sub-option labels (5 chars each) *)
CONST
  LabItems = "List Take Look Use  Give Camp Sell ";
  LabTalk  = "Yell Say  Ask  ";
  LabGame  = "PauseMusicSoundQuit Load ";
  LabBuy   = "Food ArrowVial Mace SwordBow  Totem";
  LabMagic = "StoneJewelVial Orb  TotemRing Skull";
  LabUse   = "Dirk Mace SwordBow  Wand LassoShellKey  Sun  Book ";
  LabSave  = "Save Exit ";
  LabKeys  = "Gold GreenBlue Red  Grey White";
  LabGive  = "Gold Book Writ Bone ";
  LabFile  = "  A    B    C    D    E    F    G    H  ";
  LabSell  = "AppleGrey ";

PROCEDURE InitMenuDef(VAR m: MenuDef; lab: ARRAY OF CHAR;
                       n, col: INTEGER);
VAR i: INTEGER;
BEGIN
  Assign(lab, m.labels);
  m.num := n;
  m.color := col;
  FOR i := 0 TO MaxOpts - 1 DO
    m.enabled[i] := 0
  END
END InitMenuDef;

PROCEDURE SetEnabled(VAR m: MenuDef; idx, val: INTEGER);
BEGIN
  IF (idx >= 0) AND (idx < MaxOpts) THEN
    m.enabled[idx] := val
  END
END SetEnabled;

PROCEDURE InitMenus;
VAR i: INTEGER;
BEGIN
  cmode := MItems;

  InitMenuDef(menus[MItems], LabItems, 12, 6);
  InitMenuDef(menus[MMagic], LabMagic, 12, 5);
  InitMenuDef(menus[MTalk],  LabTalk,   8, 9);
  InitMenuDef(menus[MBuy],   LabBuy,   12, 10);
  InitMenuDef(menus[MGame],  LabGame,  10, 2);
  InitMenuDef(menus[MSave],  LabSave,   7, 2);
  InitMenuDef(menus[MKeys],  LabKeys,  11, 8);
  InitMenuDef(menus[MGive],  LabGive,   9, 10);
  InitMenuDef(menus[MUse],   LabUse,   14, 8);
  InitMenuDef(menus[MFile],  LabFile,  10, 5);
  InitMenuDef(menus[MSell],  LabSell,   7, 10);

  (* Items: tabs displayed+selectable, sub-options displayed *)
  SetEnabled(menus[MItems], 0, 3);  (* Items - selected *)
  SetEnabled(menus[MItems], 1, 2);  (* Magic *)
  SetEnabled(menus[MItems], 2, 2);  (* Talk *)
  SetEnabled(menus[MItems], 3, 2);  (* Buy *)
  SetEnabled(menus[MItems], 4, 2);  (* Game *)
  FOR i := 5 TO 11 DO SetEnabled(menus[MItems], i, 10) END;

  (* Talk *)
  SetEnabled(menus[MTalk], 0, 2);
  SetEnabled(menus[MTalk], 1, 2);
  SetEnabled(menus[MTalk], 2, 3);  (* Talk selected *)
  SetEnabled(menus[MTalk], 3, 2);
  SetEnabled(menus[MTalk], 4, 2);
  FOR i := 5 TO 7 DO SetEnabled(menus[MTalk], i, 10) END;

  (* Game *)
  SetEnabled(menus[MGame], 0, 2);
  SetEnabled(menus[MGame], 1, 2);
  SetEnabled(menus[MGame], 2, 2);
  SetEnabled(menus[MGame], 3, 2);
  SetEnabled(menus[MGame], 4, 3);  (* Game selected *)
  SetEnabled(menus[MGame], 5, 6);  (* Pause - toggle *)
  SetEnabled(menus[MGame], 6, 7);  (* Music - toggle, on *)
  SetEnabled(menus[MGame], 7, 7);  (* Sound - toggle, on *)
  SetEnabled(menus[MGame], 8, 10); (* Quit → Save/Exit *)
  SetEnabled(menus[MGame], 9, 10); (* Load *)

  (* Buy *)
  SetEnabled(menus[MBuy], 0, 2);
  SetEnabled(menus[MBuy], 1, 2);
  SetEnabled(menus[MBuy], 2, 2);
  SetEnabled(menus[MBuy], 3, 3);
  SetEnabled(menus[MBuy], 4, 2);
  FOR i := 5 TO 11 DO SetEnabled(menus[MBuy], i, 10) END;

  (* Magic *)
  SetEnabled(menus[MMagic], 0, 2);
  SetEnabled(menus[MMagic], 1, 3);
  SetEnabled(menus[MMagic], 2, 2);
  SetEnabled(menus[MMagic], 3, 2);
  SetEnabled(menus[MMagic], 4, 2);
  FOR i := 5 TO 11 DO SetEnabled(menus[MMagic], i, 8) END;

  (* Save *)
  SetEnabled(menus[MSave], 0, 2);
  SetEnabled(menus[MSave], 1, 2);
  SetEnabled(menus[MSave], 2, 2);
  SetEnabled(menus[MSave], 3, 2);
  SetEnabled(menus[MSave], 4, 2);
  SetEnabled(menus[MSave], 5, 10);
  SetEnabled(menus[MSave], 6, 10);

  (* Keys *)
  SetEnabled(menus[MKeys], 0, 2);
  SetEnabled(menus[MKeys], 1, 2);
  SetEnabled(menus[MKeys], 2, 2);
  SetEnabled(menus[MKeys], 3, 2);
  SetEnabled(menus[MKeys], 4, 2);
  FOR i := 5 TO 10 DO SetEnabled(menus[MKeys], i, 10) END;

  (* Give: {2,2,2,2,2, 10,0,0,0,0,0} *)
  SetEnabled(menus[MGive], 0, 2);
  SetEnabled(menus[MGive], 1, 2);
  SetEnabled(menus[MGive], 2, 2);
  SetEnabled(menus[MGive], 3, 2);
  SetEnabled(menus[MGive], 4, 2);
  SetEnabled(menus[MGive], 5, 10);

  (* Use: {10,10,10,10,10, 10,10,10,10,0,10,10} *)
  FOR i := 0 TO 11 DO SetEnabled(menus[MUse], i, 10) END;
  SetEnabled(menus[MUse], 9, 0);

  (* File: {10,10,10,10,10, 10,10,10,0,0,0,0} *)
  FOR i := 0 TO 7 DO SetEnabled(menus[MFile], i, 10) END;

  (* Sell: 5=Apple, 6=Grey key *)
  SetEnabled(menus[MSell], 5, 8);
  SetEnabled(menus[MSell], 6, 8);

  cmode := MItems;
  BuildOptions  (* just build initial options without reading inventory *)
END InitMenus;

PROCEDURE BuildOptions;
VAR i, j, start: INTEGER;
BEGIN
  j := 0;
  (* Sub-menus (USE, MAGIC, KEYS, GIVE, BUY, SAVE, FILE, SELL)
     only show their own items (slots 5+), not the category tabs.
     Main menus (ITEMS, TALK, GAME) show tabs (slots 0-4) + items. *)
  IF (cmode = MItems) OR (cmode = MTalk) OR (cmode = MGame) THEN
    start := 0
  ELSE
    start := 5
  END;
  FOR i := start TO menus[cmode].num - 1 DO
    IF (menus[cmode].enabled[i] # 0) AND
       (BAND(CARDINAL(menus[cmode].enabled[i]), 2) # 0) THEN
      realOptions[j] := i;
      INC(j);
      IF j > 11 THEN i := menus[cmode].num END  (* break *)
    END
  END;
  optionCount := j;
  WHILE j <= 11 DO
    realOptions[j] := -1;
    INC(j)
  END
END BuildOptions;

PROCEDURE StuffFlag(itemId: INTEGER): INTEGER;
BEGIN
  (* 8 = visible but greyed, 10 = visible and bright *)
  IF InventoryCount(itemId) > 0 THEN
    RETURN 10
  ELSE
    RETURN 8
  END
END StuffFlag;

PROCEDURE SF(stuffIdx: INTEGER): INTEGER;
BEGIN
  IF HasStuff(stuffIdx) THEN RETURN 10 ELSE RETURN 8 END
END SF;

PROCEDURE WF(weapIdx: INTEGER): INTEGER;
BEGIN
  IF HasWeapon(weapIdx) THEN RETURN 10 ELSE RETURN 8 END
END WF;

PROCEDURE SetOptions;
VAR i, j: INTEGER;
BEGIN
  (* All menus: slots 0-4 are category tabs (always displayed).
     Sub-items start at slot 5. *)

  (* USE: 5=Dirk 6=Mace 7=Sword 8=Bow 9=Wand 10=Lasso 11=Shell 12=Key 13=Sun *)
  menus[MUse].enabled[5]  := WF(1);
  menus[MUse].enabled[6]  := WF(2);
  menus[MUse].enabled[7]  := WF(3);
  menus[MUse].enabled[8]  := WF(4);
  menus[MUse].enabled[9]  := WF(5);
  menus[MUse].enabled[10] := SF(5);   (* Lasso *)
  menus[MUse].enabled[11] := SF(6);   (* Shell *)
  (* Key: enabled if any key owned — original: menus[USE].enabled[7] = j *)
  j := 8;
  FOR i := 0 TO 5 DO
    IF brothers[activeBrother].stuff[16 + i] > 0 THEN j := 10 END
  END;
  menus[MUse].enabled[12] := j;       (* Keys *)
  menus[MUse].enabled[13] := SF(7);   (* Sun Stone *)

  (* MAGIC: 5-11 = stuff[9-15] *)
  FOR i := 0 TO 6 DO
    menus[MMagic].enabled[i + 5] := SF(i + 9)
  END;

  (* KEYS: 5-10 = stuff[16-21] *)
  FOR i := 0 TO 5 DO
    menus[MKeys].enabled[i + 5] := SF(i + 16)
  END;

  (* GIVE: 5=gold 6=book 7=writ 8=bone *)
  IF brothers[activeBrother].wealth > 2 THEN
    menus[MGive].enabled[5] := 10
  ELSE
    menus[MGive].enabled[5] := 8
  END;
  menus[MGive].enabled[6] := SF(26);
  menus[MGive].enabled[7] := SF(28);
  menus[MGive].enabled[8] := SF(29);

  (* SELL: 5=Apple, 6=Grey key *)
  menus[MSell].enabled[5] := SF(24);
  menus[MSell].enabled[6] := SF(20);

  BuildOptions
END SetOptions;

PROCEDURE GoMenu(mode: INTEGER);
BEGIN
  IF (mode < 0) OR (mode > 10) THEN RETURN END;
  cmode := mode;
  SetOptions
END GoMenu;

PROCEDURE HandleMenuKey(ch: CHAR);
BEGIN
  CASE ch OF
    'I': GoMenu(MItems) |
    'T': GoMenu(MTalk) |
    'G': GoMenu(MGive) |
    'Q': GoMenu(MGame) |
    'L': GoMenu(MGame) |
    'Y': GoMenu(MTalk) |
    'A': GoMenu(MTalk) |
    'U': GoMenu(MUse) |
    'B': GoMenu(MBuy) |
    'K': GoMenu(MKeys) |
    'V': GoMenu(MSave) |
    'X': GoMenu(MSave) |
    CHR(27): GoMenu(MItems)  (* ESC — return to top-level menu *)
  ELSE
  END
END HandleMenuKey;

END Menu.
