IMPLEMENTATION MODULE Menu;

FROM Strings IMPORT Assign;
FROM Items IMPORT InventoryCount,
                  ItemGold, ItemFood, ItemKey, ItemSword,
                  ItemShield, ItemPotion, ItemGem, ItemScroll;
FROM Brothers IMPORT brothers, activeBrother, HasStuff, HasWeapon,
                     StMandrake, StWolfsbane, StMugwort, StYarrow,
                     StNightshade, StBloodroot,
                     StWardScroll, StFreezeScroll, StFireScroll, StFearScroll,
                     StLightScroll, StSanctuaryScroll, StHarvestScroll,
                     StHealScroll;

(* Category tab labels — always shown as top 5 in each menu *)
CONST
  TabLabels = "ItemsMagicTalk TradeGame ";
  TradeAllBuy = 4064;   (* slots 5-11 *)
  TradeAllSell = 4064;  (* slots 5-11 *)

(* Per-mode sub-option labels (5 chars each) *)
CONST
  LabItems = "List Take Look Use  Do   ";
  LabTalk  = "Yell Say  Ask  ";
  LabGame  = "PauseMusicSoundSave Load Exit ";
  LabBuy   = "Food ArrowVial Mace SwordBow  Totem";
  LabMagic = "StoneJewelVial Orb  TotemRing SkullSpellStudyHerbs";
  LabUse   = "Dirk Mace SwordBow  Wand LassoShellKey  Sun  Book ";
  LabSave  = "Save Exit ";
  LabKeys  = "Gold GreenBlue Red  Grey White";
  LabGive  = "Gold Book Writ Bone ";
  LabFile  = "  A    B    C    D    E    F    G    H  ";
  LabSell  = "AppleGrey Potn Vial MandrGem  Mace ";
  LabSpells = "Ward FreezFire Fear LightSanctHarvsHeal ";
  LabStudy = "Ward FreezFire Fear LightSanctHarvsHeal ";
  LabHerbs = "MandrWolfsMugwtYarroNightBlood";
  LabTrade = "Buy  Sell Give ";
  LabDo    = "Camp Eat  ";
  LabHerbBuy = "MandrWolfsMugwtYarroNightBlood";
  LabHerbSell = "MandrWolfsMugwtYarroNightBlood";
  LabScrollBuy = "Ward FreezFire Fear LightSanctHarvsHeal ";
  LabAppleBuy = "Apple";

VAR
  tradeBuyMask: INTEGER;
  tradeSellMask: INTEGER;

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

PROCEDURE SlotBit(slot: INTEGER): INTEGER;
BEGIN
  CASE slot OF
     5: RETURN 32 |
     6: RETURN 64 |
     7: RETURN 128 |
     8: RETURN 256 |
     9: RETURN 512 |
    10: RETURN 1024 |
    11: RETURN 2048 |
    12: RETURN 4096 |
    13: RETURN 8192 |
    14: RETURN 16384
  ELSE
    RETURN 0
  END
END SlotBit;

PROCEDURE SetTradeFilters(buyMask, sellMask: INTEGER);
BEGIN
  tradeBuyMask := buyMask;
  tradeSellMask := sellMask
END SetTradeFilters;

PROCEDURE InitMenus;
VAR i: INTEGER;
BEGIN
  cmode := MItems;
  tradeBuyMask := TradeAllBuy;
  tradeSellMask := TradeAllSell;

  InitMenuDef(menus[MItems], LabItems, 10, 6);
  InitMenuDef(menus[MMagic], LabMagic, 15, 5);
  InitMenuDef(menus[MTalk],  LabTalk,   8, 9);
  InitMenuDef(menus[MBuy],   LabBuy,   12, 10);
  InitMenuDef(menus[MGame],  LabGame,  11, 2);
  InitMenuDef(menus[MSave],  LabSave,   7, 2);
  InitMenuDef(menus[MKeys],  LabKeys,  11, 8);
  InitMenuDef(menus[MGive],  LabGive,   9, 10);
  InitMenuDef(menus[MUse],   LabUse,   14, 8);
  InitMenuDef(menus[MFile],  LabFile,  13, 5);
  InitMenuDef(menus[MSell],  LabSell,  12, 10);
  InitMenuDef(menus[MSpells], LabSpells, 13, 5);
  InitMenuDef(menus[MStudy],  LabStudy,  13, 5);
  InitMenuDef(menus[MHerbs],  LabHerbs, 11, 6);
  InitMenuDef(menus[MTrade],  LabTrade,  8, 10);
  InitMenuDef(menus[MDo],     LabDo,     7, 6);
  InitMenuDef(menus[MHerbBuy], LabHerbBuy, 11, 10);
  InitMenuDef(menus[MHerbSell], LabHerbSell, 11, 10);
  InitMenuDef(menus[MScrollBuy], LabScrollBuy, 13, 10);
  InitMenuDef(menus[MAppleBuy], LabAppleBuy, 6, 10);

  (* Items: tabs displayed+selectable, sub-options displayed *)
  SetEnabled(menus[MItems], 0, 3);  (* Items - selected *)
  SetEnabled(menus[MItems], 1, 2);  (* Magic *)
  SetEnabled(menus[MItems], 2, 2);  (* Talk *)
  SetEnabled(menus[MItems], 3, 2);  (* Trade *)
  SetEnabled(menus[MItems], 4, 2);  (* Game *)
  FOR i := 5 TO 9 DO SetEnabled(menus[MItems], i, 10) END;

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
  SetEnabled(menus[MGame], 8, 10); (* Save *)
  SetEnabled(menus[MGame], 9, 10); (* Load *)
  SetEnabled(menus[MGame], 10, 10); (* Exit *)

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
  SetEnabled(menus[MMagic], 12, 10);
  SetEnabled(menus[MMagic], 13, 10);
  SetEnabled(menus[MMagic], 14, 10);

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

  (* File: 5-12 = slots A-H *)
  FOR i := 5 TO 12 DO SetEnabled(menus[MFile], i, 10) END;

  (* Sell: Apple, Grey key, Potion, Vial, Mandrake, Gem, Mace *)
  FOR i := 5 TO 11 DO SetEnabled(menus[MSell], i, 8) END;

  (* Spells: entries are shown once their scroll has been collected. *)
  FOR i := 5 TO 12 DO SetEnabled(menus[MSpells], i, 0) END;

  (* Study: entries are shown once their scroll has been collected. *)
  FOR i := 5 TO 12 DO SetEnabled(menus[MStudy], i, 0) END;

  (* Herbs: always list each magical ingredient. *)
  FOR i := 5 TO 10 DO SetEnabled(menus[MHerbs], i, 10) END;

  (* Trade: choose transaction type. *)
  FOR i := 5 TO 7 DO SetEnabled(menus[MTrade], i, 10) END;

  (* Do: general character actions. *)
  FOR i := 5 TO 6 DO SetEnabled(menus[MDo], i, 10) END;

  (* Herb merchant buy and sell menus. *)
  FOR i := 5 TO 10 DO SetEnabled(menus[MHerbBuy], i, 10) END;
  FOR i := 5 TO 10 DO SetEnabled(menus[MHerbSell], i, 8) END;
  FOR i := 5 TO 12 DO SetEnabled(menus[MScrollBuy], i, 10) END;
  SetEnabled(menus[MAppleBuy], 5, 10);

  cmode := MItems;
  BuildOptions  (* just build initial options without reading inventory *)
END InitMenus;

PROCEDURE BuildOptions;
VAR i, j, start: INTEGER;
BEGIN
  j := 0;
  (* Sub-menus (USE, MAGIC, KEYS, GIVE, BUY, SAVE, FILE, SELL, TRADE, DO)
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
  menus[MMagic].enabled[12] := 10;
  menus[MMagic].enabled[13] := 10;
  menus[MMagic].enabled[14] := 10;

  FOR i := 0 TO 7 DO
    IF HasStuff(StWardScroll + i) THEN
      menus[MSpells].enabled[5 + i] := 10;
      menus[MStudy].enabled[5 + i] := 10
    ELSE
      menus[MSpells].enabled[5 + i] := 0;
      menus[MStudy].enabled[5 + i] := 0
    END
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

  (* BUY: hide goods the current trading partner does not sell. *)
  FOR i := 5 TO 11 DO
    IF BAND(CARDINAL(tradeBuyMask), CARDINAL(SlotBit(i))) # 0 THEN
      menus[MBuy].enabled[i] := 10
    ELSE
      menus[MBuy].enabled[i] := 0
    END
  END;

  (* SELL: hide goods the current trading partner does not buy. *)
  IF BAND(CARDINAL(tradeSellMask), CARDINAL(SlotBit(5))) # 0 THEN
    menus[MSell].enabled[5] := SF(24)
  ELSE menus[MSell].enabled[5] := 0 END;
  IF BAND(CARDINAL(tradeSellMask), CARDINAL(SlotBit(6))) # 0 THEN
    menus[MSell].enabled[6] := SF(20)
  ELSE menus[MSell].enabled[6] := 0 END;
  IF BAND(CARDINAL(tradeSellMask), CARDINAL(SlotBit(7))) # 0 THEN
    menus[MSell].enabled[7] := StuffFlag(ItemPotion)
  ELSE menus[MSell].enabled[7] := 0 END;
  IF BAND(CARDINAL(tradeSellMask), CARDINAL(SlotBit(8))) # 0 THEN
    menus[MSell].enabled[8] := SF(11)
  ELSE menus[MSell].enabled[8] := 0 END;
  IF BAND(CARDINAL(tradeSellMask), CARDINAL(SlotBit(9))) # 0 THEN
    menus[MSell].enabled[9] := SF(StMandrake)
  ELSE menus[MSell].enabled[9] := 0 END;
  IF BAND(CARDINAL(tradeSellMask), CARDINAL(SlotBit(10))) # 0 THEN
    menus[MSell].enabled[10] := StuffFlag(ItemGem)
  ELSE menus[MSell].enabled[10] := 0 END;
  IF BAND(CARDINAL(tradeSellMask), CARDINAL(SlotBit(11))) # 0 THEN
    menus[MSell].enabled[11] := SF(1)
  ELSE menus[MSell].enabled[11] := 0 END;

  (* HERB SELL: 5-10 = magical ingredients. *)
  menus[MHerbSell].enabled[5] := SF(StMandrake);
  menus[MHerbSell].enabled[6] := SF(StWolfsbane);
  menus[MHerbSell].enabled[7] := SF(StMugwort);
  menus[MHerbSell].enabled[8] := SF(StYarrow);
  menus[MHerbSell].enabled[9] := SF(StNightshade);
  menus[MHerbSell].enabled[10] := SF(StBloodroot);

  BuildOptions
END SetOptions;

PROCEDURE GoMenu(mode: INTEGER);
BEGIN
  IF (mode < 0) OR (mode > 19) THEN RETURN END;
  cmode := mode;
  SetOptions
END GoMenu;

PROCEDURE HandleMenuKey(ch: CHAR);
BEGIN
  CASE ch OF
    'I': GoMenu(MItems) |
    'T': GoMenu(MTalk) |
    'G': GoMenu(MTrade) |
    'Q': GoMenu(MGame) |
    'L': GoMenu(MGame) |
    'Y': GoMenu(MTalk) |
    'A': GoMenu(MTalk) |
    'U': GoMenu(MUse) |
    'B': GoMenu(MTrade) |
    'D': GoMenu(MDo) |
    'K': GoMenu(MKeys) |
    'V': GoMenu(MSave) |
    'X': GoMenu(MSave) |
    CHR(27): GoMenu(MItems)  (* ESC — return to top-level menu *)
  ELSE
  END
END HandleMenuKey;

END Menu.
