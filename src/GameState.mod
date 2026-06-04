IMPLEMENTATION MODULE GameState;

FROM Strings IMPORT Assign, Concat, Length;
FROM InOut IMPORT WriteString, WriteInt, WriteLn;
FROM Platform IMPORT PollInput, InputState, DirN, DirNE, DirE, DirSE,
                    DirS, DirSW, DirW, DirNW, DirNone,
                    ScreenW, PlayH, TextH, Scale, cheatKeys;
FROM Actor IMPORT actors, actorCount, InitAll,
                  TypeEnemy, TypeSetfig,
                  StWalking, StStill, StFighting, StDead, StDying, StShoot1,
                  StSleep, StFall,
                  GoalAttack1, GoalFlee, GoalStand;
FROM World IMPORT InitWorld, TileSize, WorldW, WorldH, UpdateCamera,
                  GetTerrain, TerrSwamp, TerrWater, camX, camY;
FROM Movement IMPORT MoveActor, ProxCheck;
FROM EnemyAI IMPORT UpdateEnemies;
FROM Combat IMPORT UpdateCombat, wardTimer;
FROM Items IMPORT InitItems, CheckPickup, UseItem, InventoryCount,
                  AddToInventory, SpawnItem,
                  ItemNone, ItemGold, ItemFood, ItemPotion,
                  ItemSword, ItemKey, ItemGem, ItemShield, ItemScroll;
FROM DayNight IMPORT InitDayNight, UpdateDayNight, brightness, isNight,
                     lightlevel, MusicTickDue;
FROM Brothers IMPORT InitBrothers, SwitchToNext, ActiveName,
                     SaveBrotherState, RestoreBrotherState, brothers,
                     activeBrother, GiveStuff, SetStuff, AddWealth,
                     HasWeapon, HasStuff, AddStuffN,
                     DecLuck, IncBrave, DecKind,
                     StMandrake, StWolfsbane, StMugwort, StYarrow,
                     StNightshade, StBloodroot,
                     StWardScroll, StFreezeScroll, StFireScroll, StFearScroll,
                     StLightScroll, StSanctuaryScroll, StHarvestScroll,
                     StHealScroll, LastStuff;
FROM NPC IMPORT InitNPCs, MaterializeNPCs, TalkToNPC, LookAtNPC,
               FindNearestNPC, GiveToNPC, ResetMaterialized,
               MerchantWizardRace, ScrollPriestRace, AppleRangerRace,
               UpdateTownNPCs, IsTownTrader;
FROM Assets IMPORT InitAssets, PreloadAll, LoadHUD, currentRegion,
                   CheckRegionSwitch, SwitchRegion, DetectRegion,
                   GetTerrainAt, GetSectorByte, GetMapSector;
FROM Menu IMPORT HandleMenuKey, SetOptions, cmode, menus, realOptions,
                 optionCount, MItems, MBuy, MGive, MGame, MSave, MFile, MSell,
                 MSpells, MStudy, MHerbs, MTrade, MDo,
                 MHerbBuy, MHerbSell, MScrollBuy, MAppleBuy,
                 GoMenu, SetTradeFilters,
                 PanelX, PanelY, BtnW, BtnH;
FROM Music IMPORT SetMood, StopMusic, ResumeMusic, IsPlaying,
                  MoodDay, MoodNight, MoodIndoor, MoodSpec, SetCaveWave,
                  MoodBattle, MoodDeath;
FROM Doors IMPORT InitDoors, CheckDoor, OpenDoorTile, RestoreDoorTiles,
                  CheckCloseDoors, UseKeyOnDoor;
FROM WorldObj IMPORT CheckObjectPickup, objects, objCount, revealHidden,
                     AddObj, DistributeRegion, LeaveItem,
                     ObjMandrake, ObjWolfsbane, ObjMugwort, ObjYarrow,
                     ObjNightshade, ObjBloodroot,
                     ObjWardScroll, ObjFreezeScroll, ObjFireScroll,
                     ObjFearScroll, ObjLightScroll, ObjSanctuaryScroll,
                     ObjHarvestScroll, ObjHealScroll;
FROM HudLog IMPORT AddLogLine, SetStats, InitHudLog;
FROM Encounter IMPORT InitEncounters, UpdateEncounters, EnemiesNearby,
                      ActorsOnScreen, MoveExtent, xtype;
FROM Carrier IMPORT InitCarriers, UpdateCarriers, SpawnTurtle,
               TalkToCarrier, riding, turtleEggs, turtleEggsDone,
               UpdateDragon, dragonFire, swanDismount, dismountResult,
               swanCooldown, RideSwan;
FROM Quest IMPORT CheckRescue, CheckWinCondition, ShowWinScreen,
               SaveGame, LoadGame;
FROM Missile IMPORT InitMissiles, UpdateMissiles, FireMissile;
FROM Narration IMPORT InitPlace, UpdatePlace, Event;

CONST
  LootGold = -1;
  LootMaxKinds = 32;

VAR
  input: InputState;
  potionCooldown: INTEGER;
  hungerTimer: INTEGER;
  prevRegion: INTEGER;
  deathTimer: INTEGER;
  doorCooldown: INTEGER;
  battleFlag: BOOLEAN;
  prevBattle: BOOLEAN;
  dayPeriod: INTEGER;
  aftermathDone: BOOLEAN;  (* prevents repeated aftermath for same encounter *)
  fatigue: INTEGER;
  hunger: INTEGER;
  sleepWait: INTEGER;
  sleepInBed: BOOLEAN;
  containerRng: INTEGER;
  witchRng: INTEGER;
  saveMode: BOOLEAN;  (* TRUE=saving, FALSE=loading for File menu *)
  cheatGod: BOOLEAN;   (* invincibility *)
  cheatSpeed: BOOLEAN;  (* 5x movement speed *)
  lightTimer: INTEGER;
  freezeTimer: INTEGER;
  sanctuaryTimer: INTEGER;
  nameBuf: ARRAY [0..31] OF CHAR;
  msgBuf: ARRAY [0..79] OF CHAR;
  lootActive: BOOLEAN;
  lootCodes: ARRAY [0..31] OF INTEGER;
  lootQty: ARRAY [0..31] OF INTEGER;
  lootKinds: INTEGER;

  (* Treasure probability table *)
  treasureProbs: ARRAY [0..39] OF INTEGER;

PROCEDURE InitTreasureProbs;
BEGIN
  treasureProbs[0] := 0; treasureProbs[1] := 0; treasureProbs[2] := 0;
  treasureProbs[3] := 0; treasureProbs[4] := 0; treasureProbs[5] := 0;
  treasureProbs[6] := 0; treasureProbs[7] := 0;
  treasureProbs[8] := 9; treasureProbs[9] := 11; treasureProbs[10] := 13;
  treasureProbs[11] := 31; treasureProbs[12] := 31; treasureProbs[13] := 17;
  treasureProbs[14] := 17; treasureProbs[15] := 32;
  treasureProbs[16] := 12; treasureProbs[17] := 14; treasureProbs[18] := 20;
  treasureProbs[19] := 20; treasureProbs[20] := 20; treasureProbs[21] := 31;
  treasureProbs[22] := 33; treasureProbs[23] := 31;
  treasureProbs[24] := 10; treasureProbs[25] := 10; treasureProbs[26] := 16;
  treasureProbs[27] := 16; treasureProbs[28] := 11; treasureProbs[29] := 17;
  treasureProbs[30] := 18; treasureProbs[31] := 19;
  treasureProbs[32] := 15; treasureProbs[33] := 21; treasureProbs[34] := 0;
  treasureProbs[35] := 0; treasureProbs[36] := 0; treasureProbs[37] := 0;
  treasureProbs[38] := 0; treasureProbs[39] := 0
END InitTreasureProbs;

PROCEDURE GoldValue(stuffIdx: INTEGER): INTEGER;
BEGIN
  CASE stuffIdx OF 31: RETURN 2 | 32: RETURN 5 | 33: RETURN 10 | 34: RETURN 100
  ELSE RETURN 0 END
END GoldValue;

PROCEDURE TreasureGroup(race: INTEGER): INTEGER;
BEGIN
  CASE race OF
    0: RETURN 2 | 1: RETURN 1 | 2: RETURN 4 | 3: RETURN 3
  ELSE RETURN 0 END
END TreasureGroup;

PROCEDURE IntToStr(n: INTEGER; VAR buf: ARRAY OF CHAR);
VAR i, len: INTEGER; tmp: ARRAY [0..7] OF CHAR;
BEGIN
  IF n < 0 THEN n := 0 END; len := 0;
  IF n = 0 THEN tmp[0] := '0'; len := 1
  ELSE WHILE n > 0 DO tmp[len] := CHR(ORD('0') + (n MOD 10)); n := n DIV 10; INC(len) END
  END;
  FOR i := 0 TO len - 1 DO buf[i] := tmp[len - 1 - i] END;
  buf[len] := 0C
END IntToStr;

PROCEDURE InitGame;
BEGIN
  running := TRUE;
  cycle := 0;
  dayNight := 12000;
  msgTimer := 0;
  msgText[0] := 0C;
  potionCooldown := 0;
  hungerTimer := 0;
  deathTimer := 0;
  doorCooldown := 0;
  battleFlag := FALSE;
  prevBattle := FALSE;
  viewStatus := ViewNormal;
  dayPeriod := 6;
  aftermathDone := FALSE;
  fatigue := 0;
  hunger := 0;
  sleepWait := 0;
  sleepInBed := FALSE;
  containerRng := 31337;
  witchRng := 54321;
  saveMode := FALSE;
  cheatGod := FALSE;
  cheatSpeed := FALSE;
  cheatKeys := FALSE;  (* in Platform *)
  lightTimer := 0;
  secretTimer := 0;
  freezeTimer := 0;
  wardTimer := 0;
  sanctuaryTimer := 0;
  lootActive := FALSE;
  lootKinds := 0;
  fairyActive := FALSE;
  fairyX := 0;
  colorPlayTimer := 0;
  witchFlag := FALSE;
  witchIndex := 0;
  witchDir := 1;
  witchS1 := 0; witchS2 := 0;

  InitWorld;
  InitAll;
  InitItems;
  InitDayNight;
  InitBrothers;
  InitDoors;
  InitHudLog;
  InitNPCs;
  InitEncounters;
  InitCarriers;
  InitMissiles;
  InitTreasureProbs;
  InitBuyTable;
  InitStoneList;

  RestoreBrotherState;
  actorCount := 1;

  InitAssets;
  IF PreloadAll() THEN
    IF NOT LoadHUD(ScreenW * Scale, TextH * Scale) THEN
      WriteString("*** HUD LOAD FAILED ***"); WriteLn
    END;
    SwitchRegion(3);
    DistributeRegion(3);
    actors[0].absX := 19036;
    actors[0].absY := 15755;
    InitPlace(actors[0].absX, actors[0].absY, 3);
    Event(9);
    Event(30);
    SetOptions
  ELSE
    ShowMessage("Welcome! (placeholder mode)")
  END
END InitGame;

PROCEDURE ShowMessage(msg: ARRAY OF CHAR);
VAR buf: ARRAY [0..255] OF CHAR;
    si, di, ni: INTEGER;
    name: ARRAY [0..15] OF CHAR;
BEGIN
  ActiveName(name);
  si := 0; di := 0;
  WHILE (si <= HIGH(msg)) AND (msg[si] # 0C) AND (di < 255) DO
    IF msg[si] = '%' THEN
      ni := 0;
      WHILE (ni <= HIGH(name)) AND (name[ni] # 0C) AND (di < 255) DO
        buf[di] := name[ni]; INC(di); INC(ni)
      END;
      INC(si)
    ELSE
      buf[di] := msg[si]; INC(di); INC(si)
    END
  END;
  buf[di] := 0C;
  Assign(buf, msgText);
  msgTimer := 180;
  AddLogLine(buf)
END ShowMessage;

PROCEDURE ShowInventory;
BEGIN viewStatus := ViewInventory END ShowInventory;

PROCEDURE HandleLook;
VAR i, dx, dy, found: INTEGER;
BEGIN
  found := 0;
  FOR i := 0 TO objCount - 1 DO
    IF ((objects[i].status = 0) OR (objects[i].status = 5)) AND
       ((objects[i].region = currentRegion) OR (objects[i].region = -1)) THEN
      dx := actors[0].absX - objects[i].x;
      dy := actors[0].absY - objects[i].y;
      IF (dx > -40) AND (dx < 40) AND (dy > -40) AND (dy < 40) THEN
        objects[i].status := 1; found := 1
      END
    END
  END;
  IF found > 0 THEN Event(38)
  ELSE
    IF LookAtNPC(actors[0].absX, actors[0].absY, nameBuf) THEN
      Assign("% sees ", msgBuf);
      Concat(msgBuf, nameBuf, msgBuf);
      Concat(msgBuf, ".", msgBuf);
      ShowMessage(msgBuf)
    ELSE Event(20) END
  END
END HandleLook;

PROCEDURE TogglePause;
BEGIN
  IF BAND(CARDINAL(menus[MGame].enabled[5]), 1) = 0 THEN
    menus[MGame].enabled[5] := BOR(INTEGER(CARDINAL(menus[MGame].enabled[5])), 1);
    ShowMessage("Game paused.")
  ELSE
    menus[MGame].enabled[5] := BAND(CARDINAL(menus[MGame].enabled[5]), 14);
    ShowMessage("Game resumed.")
  END
END TogglePause;

PROCEDURE ToggleMusic;
BEGIN
  IF BAND(CARDINAL(menus[MGame].enabled[6]), 1) = 0 THEN
    menus[MGame].enabled[6] := BOR(INTEGER(CARDINAL(menus[MGame].enabled[6])), 1);
    ResumeMusic
  ELSE
    menus[MGame].enabled[6] := BAND(CARDINAL(menus[MGame].enabled[6]), 14);
    StopMusic
  END
END ToggleMusic;

PROCEDURE WeaponName(w: INTEGER; VAR name: ARRAY OF CHAR);
BEGIN
  CASE w OF
    1: Assign("a dagger", name) | 2: Assign("a mace", name) |
    3: Assign("a sword", name) | 4: Assign("a bow", name) |
    5: Assign("a wand", name)
  ELSE Assign("a weapon", name) END
END WeaponName;

PROCEDURE TreasureName(ti: INTEGER; VAR name: ARRAY OF CHAR);
BEGIN
  CASE ti OF
     9: Assign("a Blue Stone", name) | 10: Assign("a Green Jewel", name) |
    11: Assign("a Glass Vial", name) | 12: Assign("a Crystal Orb", name) |
    13: Assign("a Bird Totem", name) | 14: Assign("a Gold Ring", name) |
    15: Assign("a Jade Skull", name) | 16: Assign("a Gold Key", name) |
    17: Assign("a Green Key", name) | 18: Assign("a Blue Key", name) |
    19: Assign("a Red Key", name) | 20: Assign("a Grey Key", name) |
    21: Assign("a White Key", name) |
    StMandrake: Assign("Mandrake", name) |
    StWolfsbane: Assign("Wolfsbane", name) |
    StMugwort: Assign("Mugwort", name) |
    StYarrow: Assign("Yarrow", name) |
    StNightshade: Assign("Nightshade", name) |
    StBloodroot: Assign("Bloodroot", name) |
    StWardScroll: Assign("a Ward Scroll", name) |
    StFreezeScroll: Assign("a Freeze Scroll", name) |
    StFireScroll: Assign("a Fire Scroll", name) |
    StFearScroll: Assign("a Fear Scroll", name) |
    StLightScroll: Assign("a Light Scroll", name) |
    StSanctuaryScroll: Assign("a Sanctuary Scroll", name) |
    StHarvestScroll: Assign("a Harvest Scroll", name) |
    StHealScroll: Assign("a Heal Scroll", name)
  ELSE Assign("a treasure", name) END
END TreasureName;

PROCEDURE LootName(code: INTEGER; VAR name: ARRAY OF CHAR);
BEGIN
  CASE code OF
    LootGold: Assign("Gold", name) |
     0: Assign("Dagger", name) | 1: Assign("Mace", name) |
     2: Assign("Sword", name) | 3: Assign("Bow", name) |
     4: Assign("Wand", name) | 5: Assign("Golden Lasso", name) |
     6: Assign("Sea Shell", name) | 7: Assign("Sun Stone", name) |
     8: Assign("Arrows", name) | 9: Assign("Blue Stone", name) |
    10: Assign("Green Jewel", name) | 11: Assign("Glass Vial", name) |
    12: Assign("Crystal Orb", name) | 13: Assign("Bird Totem", name) |
    14: Assign("Gold Ring", name) | 15: Assign("Jade Skull", name) |
    16: Assign("Gold Key", name) | 17: Assign("Green Key", name) |
    18: Assign("Blue Key", name) | 19: Assign("Red Key", name) |
    20: Assign("Grey Key", name) | 21: Assign("White Key", name) |
    22: Assign("Talisman", name) | 24: Assign("Apple", name) |
    25: Assign("Gold Statue", name) | 29: Assign("King's Bone", name) |
    30: Assign("Shard", name) |
    StMandrake: Assign("Mandrake", name) |
    StWolfsbane: Assign("Wolfsbane", name) |
    StMugwort: Assign("Mugwort", name) |
    StYarrow: Assign("Yarrow", name) |
    StNightshade: Assign("Nightshade", name) |
    StBloodroot: Assign("Bloodroot", name) |
    StWardScroll: Assign("Ward Scroll", name) |
    StFreezeScroll: Assign("Freeze Scroll", name) |
    StFireScroll: Assign("Fire Scroll", name) |
    StFearScroll: Assign("Fear Scroll", name) |
    StLightScroll: Assign("Light Scroll", name) |
    StSanctuaryScroll: Assign("Sanctuary Scroll", name) |
    StHarvestScroll: Assign("Harvest Scroll", name) |
    StHealScroll: Assign("Heal Scroll", name)
  ELSE Assign("Item", name) END
END LootName;

PROCEDURE BeginLootSummary;
VAR i: INTEGER;
BEGIN
  lootActive := TRUE; lootKinds := 0;
  FOR i := 0 TO LootMaxKinds - 1 DO
    lootCodes[i] := 0; lootQty[i] := 0
  END
END BeginLootSummary;

PROCEDURE EndLootSummary;
BEGIN
  lootActive := FALSE
END EndLootSummary;

PROCEDURE AddLoot(code, qty: INTEGER);
VAR i: INTEGER;
BEGIN
  IF (NOT lootActive) OR (qty <= 0) THEN RETURN END;
  FOR i := 0 TO lootKinds - 1 DO
    IF lootCodes[i] = code THEN
      INC(lootQty[i], qty); RETURN
    END
  END;
  IF lootKinds < LootMaxKinds THEN
    lootCodes[lootKinds] := code;
    lootQty[lootKinds] := qty;
    INC(lootKinds)
  END
END AddLoot;

PROCEDURE BuildLootEntry(code, qty: INTEGER; VAR entry: ARRAY OF CHAR);
VAR numStr, itemName: ARRAY [0..31] OF CHAR;
BEGIN
  IntToStr(qty, numStr);
  Assign(numStr, entry); Concat(entry, " ", entry);
  LootName(code, itemName); Concat(entry, itemName, entry)
END BuildLootEntry;

PROCEDURE ShowLootSummary(prefix, emptyMsg: ARRAY OF CHAR);
VAR i, hidden, shown: INTEGER;
    entry, more: ARRAY [0..47] OF CHAR;
BEGIN
  IF lootKinds = 0 THEN
    ShowMessage(emptyMsg); RETURN
  END;
  Assign(prefix, msgBuf);
  hidden := 0; shown := 0;
  FOR i := 0 TO lootKinds - 1 DO
    BuildLootEntry(lootCodes[i], lootQty[i], entry);
    IF Length(msgBuf) + Length(entry) + 3 < 76 THEN
      IF shown > 0 THEN Concat(msgBuf, ", ", msgBuf) END;
      Concat(msgBuf, entry, msgBuf);
      INC(shown)
    ELSE
      INC(hidden, lootQty[i])
    END
  END;
  IF hidden > 0 THEN
    IntToStr(hidden, more);
    IF Length(msgBuf) + Length(more) + 8 < 76 THEN
      IF shown > 0 THEN Concat(msgBuf, ", ", msgBuf) END;
      Concat(msgBuf, "+", msgBuf); Concat(msgBuf, more, msgBuf);
      Concat(msgBuf, " more", msgBuf)
    END
  END;
  Concat(msgBuf, ".", msgBuf);
  ShowMessage(msgBuf)
END ShowLootSummary;

PROCEDURE TakeEnemyWeapon(enemyIdx: INTEGER; VAR name: ARRAY OF CHAR): BOOLEAN;
VAR w, arrows: INTEGER;
BEGIN
  w := actors[enemyIdx].weapon;
  IF (w < 1) OR (w > 5) THEN RETURN FALSE END;
  GiveStuff(w - 1);
  AddLoot(w - 1, 1);
  WeaponName(w, name);
  IF w > actors[0].weapon THEN actors[0].weapon := w END;
  IF w = 4 THEN
    arrows := (cycle MOD 8) + 2;
    AddStuffN(8, arrows);
    AddLoot(8, arrows)
  END;
  actors[enemyIdx].weapon := 0;
  RETURN TRUE
END TakeEnemyWeapon;

PROCEDURE TakeEnemyTreasure(enemyIdx: INTEGER; VAR name: ARRAY OF CHAR): BOOLEAN;
VAR race, tg, ti, gv, roll: INTEGER;
BEGIN
  IF actors[enemyIdx].looted THEN RETURN FALSE END;
  actors[enemyIdx].looted := TRUE;
  race := actors[enemyIdx].race; ti := 0;
  IF race = 2 THEN
    (* Wraiths are the main magical supply source: ingredients are
       frequent, while scrolls are deliberately rare. *)
    roll := (cycle + actors[enemyIdx].absX + actors[enemyIdx].absY) MOD 20;
    IF roll < 3 THEN
      ti := StWardScroll + ((cycle + actors[enemyIdx].absX) MOD 8)
    ELSE
      ti := StMandrake + ((cycle + actors[enemyIdx].absY) MOD 6)
    END
  ELSIF race < 128 THEN
    tg := TreasureGroup(race);
    ti := tg * 8 + (cycle MOD 8);
    IF (ti >= 0) AND (ti <= 39) THEN ti := treasureProbs[ti] ELSE ti := 0 END
  END;
  IF ti <= 0 THEN RETURN FALSE END;
  IF race = 2 THEN
    GiveStuff(ti); AddLoot(ti, 1); TreasureName(ti, name)
  ELSIF ti >= 31 THEN
    gv := GoldValue(ti); AddWealth(gv);
    AddLoot(LootGold, gv);
    IntToStr(gv, name); Concat(name, " Gold Pieces", name)
  ELSE
    GiveStuff(ti); AddLoot(ti, 1); TreasureName(ti, name)
  END;
  RETURN TRUE
END TakeEnemyTreasure;

PROCEDURE SearchNearbyCorpses;
VAR i, dx, dy, bodyCount, lootCount: INTEGER;
    wname, tname: ARRAY [0..31] OF CHAR;
    hasWeapon, hasTreasure: BOOLEAN;
BEGIN
  bodyCount := 0; lootCount := 0;
  BeginLootSummary;
  FOR i := 1 TO actorCount - 1 DO
    IF (actors[i].actorType = TypeEnemy) AND (actors[i].state = StDead) THEN
      dx := actors[0].absX - actors[i].absX;
      dy := actors[0].absY - actors[i].absY;
      IF dx < 0 THEN dx := -dx END;
      IF dy < 0 THEN dy := -dy END;
      IF (dx < 20) AND (dy < 20) THEN
        INC(bodyCount);
        hasWeapon := TakeEnemyWeapon(i, wname);
        hasTreasure := TakeEnemyTreasure(i, tname);
        IF hasWeapon THEN INC(lootCount) END;
        IF hasTreasure THEN INC(lootCount) END
      END
    END
  END;
  EndLootSummary;
  IF lootCount > 0 THEN
    ShowLootSummary("% found ", "Nothing to take.");
    SetOptions
  ELSIF bodyCount > 0 THEN
    ShowMessage("% searched nearby bodies and found nothing.");
    SetOptions
  ELSE
    ShowMessage("Nothing to take.")
  END
END SearchNearbyCorpses;

PROCEDURE RandContainer(limit: INTEGER): INTEGER;
BEGIN
  containerRng := containerRng * 1103515245 + 12345;
  IF containerRng < 0 THEN containerRng := -containerRng END;
  IF limit <= 0 THEN RETURN 0 END;
  RETURN (containerRng DIV 65536) MOD limit
END RandContainer;

PROCEDURE PickContainerItem(): INTEGER;
VAR i: INTEGER;
BEGIN
  i := RandContainer(8) + 8;
  IF i = 8 THEN i := 9 END;
  RETURN i
END PickContainerItem;

PROCEDURE ContainerLoot;
VAR k, i, j, gv: INTEGER;
    tname, numStr: ARRAY [0..31] OF CHAR;
BEGIN
  k := RandContainer(4);
  IF k = 0 THEN ShowMessage("nothing.")
  ELSIF k = 1 THEN
    i := PickContainerItem();
    GiveStuff(i); AddLoot(i, 1); TreasureName(i, tname);
    Assign(tname, msgBuf); Concat(msgBuf, ".", msgBuf);
    ShowMessage(msgBuf)
  ELSIF k = 2 THEN
    i := PickContainerItem();
    IF i = 8 THEN
      gv := 100; AddWealth(gv);
      AddLoot(LootGold, gv);
      IntToStr(gv, numStr); Assign(numStr, msgBuf); Concat(msgBuf, " Gold Pieces", msgBuf)
    ELSE GiveStuff(i); AddLoot(i, 1); TreasureName(i, tname); Assign(tname, msgBuf)
    END;
    j := PickContainerItem();
    WHILE j = i DO j := PickContainerItem() END;
    GiveStuff(j); AddLoot(j, 1); TreasureName(j, tname);
    Concat(msgBuf, " and ", msgBuf); Concat(msgBuf, tname, msgBuf);
    Concat(msgBuf, ".", msgBuf); ShowMessage(msgBuf)
  ELSE
    (* Original: roll rand8()+8 first. Only if result=8 (12.5%) → 3 keys.
       Otherwise → 3 of that item. Total key chance = 25% × 12.5% = 3.125% *)
    i := RandContainer(8) + 8;
    IF i = 8 THEN
      ShowMessage("3 keys.");
      FOR j := 0 TO 2 DO
        k := RandContainer(8) + 16;
        (* Original: remap 22→16 (Gold), 23→20 (Grey) *)
        IF k = 22 THEN k := 16
        ELSIF k = 23 THEN k := 20
        END;
        GiveStuff(k); AddLoot(k, 1)
      END
    ELSE
      GiveStuff(i); GiveStuff(i); GiveStuff(i);
      AddLoot(i, 3);
      TreasureName(i, tname);
      Assign("3 ", msgBuf); Concat(msgBuf, tname, msgBuf);
      Concat(msgBuf, "s.", msgBuf); ShowMessage(msgBuf)
    END
  END
END ContainerLoot;

(* Buy price table: pairs of (stuff_index, cost).
   Menu slots 5-11 map to indices 0-6 in this table.
   Original: jtrans[] = { 0,3, 8,10, 11,15, 1,30, 2,45, 3,75, 13,20 } *)
CONST
  BuyItems = 7;
VAR
  buyStuff: ARRAY [0..6] OF INTEGER;
  buyCost:  ARRAY [0..6] OF INTEGER;

PROCEDURE InitBuyTable;
BEGIN
  buyStuff[0] :=  0; buyCost[0] :=  3;   (* Food *)
  buyStuff[1] :=  8; buyCost[1] := 10;   (* Arrows *)
  buyStuff[2] := 11; buyCost[2] := 15;   (* Vial *)
  buyStuff[3] :=  1; buyCost[3] := 30;   (* Mace *)
  buyStuff[4] :=  2; buyCost[4] := 45;   (* Sword *)
  buyStuff[5] :=  3; buyCost[5] := 75;   (* Bow *)
  buyStuff[6] := 13; buyCost[6] := 20    (* Totem *)
END InitBuyTable;

PROCEDURE TownSells(race, optIdx: INTEGER): BOOLEAN;
BEGIN
  CASE race OF
    19: RETURN (optIdx = 6) OR (optIdx = 8) |
    20: RETURN optIdx = 5 |
    22: RETURN (optIdx = 6) OR (optIdx = 8) OR (optIdx = 9) |
    24: RETURN optIdx = 7 |
    26: RETURN optIdx = 7 |
    28: RETURN optIdx = 5 |
    30: RETURN (optIdx = 6) OR (optIdx = 8) |
    31: RETURN optIdx = 7 |
    33: RETURN (optIdx = 7) OR (optIdx = 11)
  ELSE
    RETURN FALSE
  END
END TownSells;

PROCEDURE TownBuys(race, optIdx: INTEGER): BOOLEAN;
BEGIN
  CASE race OF
    19: RETURN optIdx = 11 |              (* Mace *)
    20: RETURN optIdx = 5 |               (* Apple *)
    21: RETURN optIdx = 6 |               (* Grey key *)
    22: RETURN optIdx = 7 |               (* Potion *)
    24: RETURN optIdx = 10 |              (* Gem *)
    26: RETURN optIdx = 9 |               (* Mandrake *)
    28: RETURN optIdx = 5 |               (* Apple *)
    30: RETURN optIdx = 11 |              (* Mace *)
    31: RETURN (optIdx = 8) OR (optIdx = 10) |  (* Vial, Gem *)
    33: RETURN (optIdx = 6) OR (optIdx = 8)    (* Grey key, Vial *)
  ELSE
    RETURN FALSE
  END
END TownBuys;

PROCEDURE TownSellPrice(optIdx: INTEGER): INTEGER;
BEGIN
  CASE optIdx OF
     5: RETURN 5  |   (* Apple *)
     6: RETURN 50 |   (* Grey key *)
     7: RETURN 12 |   (* Potion *)
     8: RETURN 8  |   (* Vial *)
     9: RETURN 15 |   (* Mandrake *)
    10: RETURN 25 |   (* Gem *)
    11: RETURN 15     (* Mace *)
  ELSE
    RETURN 0
  END
END TownSellPrice;

PROCEDURE TradeSlotBit(slot: INTEGER): INTEGER;
BEGIN
  CASE slot OF
     5: RETURN 32 |
     6: RETURN 64 |
     7: RETURN 128 |
     8: RETURN 256 |
     9: RETURN 512 |
    10: RETURN 1024 |
    11: RETURN 2048
  ELSE
    RETURN 0
  END
END TradeSlotBit;

PROCEDURE BuildTownBuyMask(race: INTEGER): INTEGER;
VAR i, mask: INTEGER;
BEGIN
  mask := 0;
  FOR i := 5 TO 11 DO
    IF TownSells(race, i) THEN INC(mask, TradeSlotBit(i)) END
  END;
  RETURN mask
END BuildTownBuyMask;

PROCEDURE BuildTownSellMask(race: INTEGER): INTEGER;
VAR i, mask: INTEGER;
BEGIN
  mask := 0;
  FOR i := 5 TO 11 DO
    IF TownBuys(race, i) THEN INC(mask, TradeSlotBit(i)) END
  END;
  RETURN mask
END BuildTownSellMask;

PROCEDURE HandleBuy(optIdx: INTEGER);
VAR npc, slot, si, cost: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF npc < 0 THEN ShowMessage("Nobody to buy from."); RETURN END;
  IF (actors[npc].race # 8) AND (NOT IsTownTrader(actors[npc].race)) THEN
    ShowMessage("Nobody to buy from."); RETURN
  END;
  IF (optIdx < 5) OR (optIdx > 11) THEN RETURN END;
  IF IsTownTrader(actors[npc].race) AND (NOT TownSells(actors[npc].race, optIdx)) THEN
    ShowMessage("They are not selling that."); RETURN
  END;
  slot := optIdx - 5;
  si := buyStuff[slot];
  cost := buyCost[slot];
  IF brothers[activeBrother].wealth > cost THEN
    AddWealth(-cost);
    IF si = 0 THEN
      (* Food: original order is event(22) then eat(50) *)
      Event(22);
      DEC(hunger, 50);
      IF hunger < 0 THEN hunger := 0; Event(13)
      ELSE ShowMessage("Yum!")
      END
    ELSIF si = 8 THEN
      (* Arrows: buy 10 *)
      AddStuffN(8, 10);
      Event(23)
    ELSE
      GiveStuff(si);
      Assign("% bought a ", msgBuf);
      TreasureName(si, nameBuf);
      Concat(msgBuf, nameBuf, msgBuf);
      Concat(msgBuf, ".", msgBuf);
      ShowMessage(msgBuf)
    END;
    SetOptions
  ELSE
    ShowMessage("Not enough money!")
  END
END HandleBuy;

PROCEDURE HandleSell(optIdx: INTEGER);
VAR npc, price: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF (npc < 0) OR ((actors[npc].race # 8) AND (NOT IsTownTrader(actors[npc].race))) THEN
    ShowMessage("You can only sell items in a tavern.");
    GoMenu(0);
    RETURN
  END;
  IF (actors[npc].race = 8) AND (optIdx > 6) THEN
    ShowMessage("The tavern is not buying that."); RETURN
  END;
  IF IsTownTrader(actors[npc].race) AND (NOT TownBuys(actors[npc].race, optIdx)) THEN
    ShowMessage("They are not buying that."); RETURN
  END;
  price := TownSellPrice(optIdx);
  CASE optIdx OF
    5:  (* Apple *)
      IF brothers[activeBrother].stuff[24] > 0 THEN
        DEC(brothers[activeBrother].stuff[24]);
        AddWealth(price);
        ShowMessage("% sold an apple.")
      ELSE ShowMessage("% doesn't have an apple.") END |
    6:  (* Grey key *)
      IF brothers[activeBrother].stuff[20] > 0 THEN
        DEC(brothers[activeBrother].stuff[20]);
        AddWealth(price);
        ShowMessage("% sold a grey key.")
      ELSE ShowMessage("% doesn't have a grey key.") END |
    7:  (* Potion *)
      IF UseItem(ItemPotion) THEN
        AddWealth(price);
        ShowMessage("% sold a potion.")
      ELSE ShowMessage("% doesn't have a potion.") END |
    8:  (* Vial *)
      IF brothers[activeBrother].stuff[11] > 0 THEN
        DEC(brothers[activeBrother].stuff[11]);
        AddWealth(price);
        ShowMessage("% sold a vial.")
      ELSE ShowMessage("% doesn't have a vial.") END |
    9:  (* Mandrake *)
      IF brothers[activeBrother].stuff[StMandrake] > 0 THEN
        DEC(brothers[activeBrother].stuff[StMandrake]);
        AddWealth(price);
        ShowMessage("% sold Mandrake.")
      ELSE ShowMessage("% doesn't have Mandrake.") END |
   10:  (* Gem *)
      IF UseItem(ItemGem) THEN
        AddWealth(price);
        ShowMessage("% sold a gem.")
      ELSE ShowMessage("% doesn't have a gem.") END |
   11:  (* Mace *)
      IF brothers[activeBrother].stuff[1] > 0 THEN
        DEC(brothers[activeBrother].stuff[1]);
        AddWealth(price);
        ShowMessage("% sold a mace.")
      ELSE ShowMessage("% doesn't have a mace.") END
  ELSE
    RETURN
  END;
  SetOptions
END HandleSell;

PROCEDURE HerbPrice(si: INTEGER): INTEGER;
BEGIN
  CASE si OF
    StMandrake: RETURN 30 |
    StWolfsbane: RETURN 40 |
    StMugwort: RETURN 25 |
    StYarrow: RETURN 20 |
    StNightshade: RETURN 45 |
    StBloodroot: RETURN 50
  ELSE
    RETURN 0
  END
END HerbPrice;

PROCEDURE HerbStuff(optIdx: INTEGER): INTEGER;
BEGIN
  IF (optIdx < 5) OR (optIdx > 10) THEN RETURN -1 END;
  RETURN StMandrake + optIdx - 5
END HerbStuff;

PROCEDURE HandleHerbBuy(optIdx: INTEGER);
VAR npc, si, cost: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF (npc < 0) OR (actors[npc].race # MerchantWizardRace) THEN
    ShowMessage("The herb wizard is not nearby."); GoMenu(0); RETURN
  END;
  si := HerbStuff(optIdx);
  IF si < 0 THEN RETURN END;
  cost := HerbPrice(si);
  IF brothers[activeBrother].wealth < cost THEN
    ShowMessage("Not enough money!"); RETURN
  END;
  AddWealth(-cost);
  GiveStuff(si);
  TreasureName(si, nameBuf);
  Assign("% bought ", msgBuf); Concat(msgBuf, nameBuf, msgBuf);
  Concat(msgBuf, ".", msgBuf); ShowMessage(msgBuf);
  SetOptions
END HandleHerbBuy;

PROCEDURE HandleHerbSell(optIdx: INTEGER);
VAR npc, si, price: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF (npc < 0) OR (actors[npc].race # MerchantWizardRace) THEN
    ShowMessage("The herb wizard is not nearby."); GoMenu(0); RETURN
  END;
  si := HerbStuff(optIdx);
  IF si < 0 THEN RETURN END;
  IF brothers[activeBrother].stuff[si] <= 0 THEN
    ShowMessage("% doesn't have that ingredient."); RETURN
  END;
  DEC(brothers[activeBrother].stuff[si]);
  price := HerbPrice(si) DIV 2;
  AddWealth(price);
  TreasureName(si, nameBuf);
  Assign("% sold ", msgBuf); Concat(msgBuf, nameBuf, msgBuf);
  Concat(msgBuf, ".", msgBuf); ShowMessage(msgBuf);
  SetOptions
END HandleHerbSell;

PROCEDURE ScrollPrice(si: INTEGER): INTEGER;
BEGIN
  CASE si OF
    StWardScroll: RETURN 30 |
    StFreezeScroll: RETURN 50 |
    StFireScroll: RETURN 60 |
    StFearScroll: RETURN 40 |
    StLightScroll: RETURN 20 |
    StSanctuaryScroll: RETURN 100 |
    StHarvestScroll: RETURN 70 |
    StHealScroll: RETURN 40
  ELSE
    RETURN 0
  END
END ScrollPrice;

PROCEDURE HandleScrollBuy(optIdx: INTEGER);
VAR npc, si, cost: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF (npc < 0) OR (actors[npc].race # ScrollPriestRace) THEN
    ShowMessage("The scroll priest is not nearby."); GoMenu(0); RETURN
  END;
  IF (optIdx < 5) OR (optIdx > 12) THEN RETURN END;
  si := StWardScroll + optIdx - 5;
  cost := ScrollPrice(si);
  IF brothers[activeBrother].wealth < cost THEN
    ShowMessage("Not enough money!"); RETURN
  END;
  AddWealth(-cost);
  GiveStuff(si);
  TreasureName(si, nameBuf);
  Assign("% bought ", msgBuf); Concat(msgBuf, nameBuf, msgBuf);
  Concat(msgBuf, ".", msgBuf); ShowMessage(msgBuf);
  SetOptions
END HandleScrollBuy;

PROCEDURE HandleAppleBuy(optIdx: INTEGER);
CONST AppleCost = 5;
VAR npc: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF (npc < 0) OR (actors[npc].race # AppleRangerRace) THEN
    ShowMessage("The apple ranger is not nearby."); GoMenu(0); RETURN
  END;
  IF optIdx # 5 THEN RETURN END;
  IF brothers[activeBrother].wealth < AppleCost THEN
    ShowMessage("Not enough money!"); RETURN
  END;
  AddWealth(-AppleCost);
  GiveStuff(24);
  ShowMessage("% bought an apple.");
  SetOptions
END HandleAppleBuy;

PROCEDURE OpenBuyMenu;
VAR npc: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF (npc >= 0) AND (actors[npc].race = MerchantWizardRace) THEN
    GoMenu(MHerbBuy)
  ELSIF (npc >= 0) AND (actors[npc].race = ScrollPriestRace) THEN
    GoMenu(MScrollBuy)
  ELSIF (npc >= 0) AND (actors[npc].race = AppleRangerRace) THEN
    GoMenu(MAppleBuy)
  ELSIF (npc >= 0) AND IsTownTrader(actors[npc].race) THEN
    SetTradeFilters(BuildTownBuyMask(actors[npc].race), BuildTownSellMask(actors[npc].race));
    GoMenu(MBuy)
  ELSIF (npc >= 0) AND (actors[npc].race = 8) THEN
    SetTradeFilters(4064, 96);
    GoMenu(MBuy)
  ELSE
    SetTradeFilters(0, 0);
    GoMenu(MBuy)
  END
END OpenBuyMenu;

PROCEDURE OpenSellMenu;
VAR npc: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF (npc >= 0) AND (actors[npc].race = MerchantWizardRace) THEN
    GoMenu(MHerbSell)
  ELSIF (npc >= 0) AND IsTownTrader(actors[npc].race) THEN
    SetTradeFilters(BuildTownBuyMask(actors[npc].race), BuildTownSellMask(actors[npc].race));
    GoMenu(MSell)
  ELSIF (npc >= 0) AND (actors[npc].race = 8) THEN
    SetTradeFilters(4064, 96);
    GoMenu(MSell)
  ELSE
    SetTradeFilters(0, 0);
    GoMenu(MSell)
  END
END OpenSellMenu;

PROCEDURE HandleGive(optIdx: INTEGER);
VAR resp: ARRAY [0..127] OF CHAR;
    npc: INTEGER;
BEGIN
  npc := FindNearestNPC(actors[0].absX, actors[0].absY);
  IF npc < 0 THEN ShowMessage("Nobody here."); GoMenu(0); RETURN END;
  IF GiveToNPC(actors[0].absX, actors[0].absY, optIdx - 5, resp) THEN
    IF resp[0] # 0C THEN ShowMessage(resp) END
  END;
  SetOptions;
  GoMenu(0)
END HandleGive;

PROCEDURE InheritBrotherItems;
VAR prev, k: INTEGER;
BEGIN
  (* Original: inherit from the specific previous brother.
     brother=1 (Philip active) → inherit from 0 (Julian).
     brother=2 (Kevin active) → inherit from 1 (Philip). *)
  prev := activeBrother - 1;
  IF (prev >= 0) AND (prev <= 2) AND (NOT brothers[prev].alive) THEN
    FOR k := 0 TO LastStuff DO
      INC(brothers[activeBrother].stuff[k], brothers[prev].stuff[k]);
      brothers[prev].stuff[k] := 0
    END
  END;
  (* Hide ghost brothers — original: ob_listg[3].ob_stat = ob_listg[4].ob_stat = 0 *)
  objects[0].status := 0;
  objects[1].status := 0
END InheritBrotherItems;

PROCEDURE HandleKeys(optIdx: INTEGER);
VAR keyIdx: INTEGER;
BEGIN
  keyIdx := optIdx - 5;  (* 0=Gold,1=Green,2=Blue,3=Red,4=Grey,5=White *)
  IF (keyIdx < 0) OR (keyIdx > 5) THEN RETURN END;
  IF brothers[activeBrother].stuff[16 + keyIdx] <= 0 THEN
    ShowMessage("You don't have that key."); GoMenu(0); RETURN
  END;
  IF UseKeyOnDoor(actors[0].absX, actors[0].absY, keyIdx + 1) THEN
    DEC(brothers[activeBrother].stuff[16 + keyIdx]);
    ShowMessage("It opened.")
  ELSE
    TreasureName(16 + keyIdx, nameBuf);
    Assign("% tried ", msgBuf);
    Concat(msgBuf, nameBuf, msgBuf);
    Concat(msgBuf, " but it didn't fit.", msgBuf);
    ShowMessage(msgBuf)
  END;
  GoMenu(0)
END HandleKeys;

(* Stonehenge teleport table — 11 stone circles *)
VAR
  stoneX: ARRAY [0..10] OF INTEGER;
  stoneY: ARRAY [0..10] OF INTEGER;

PROCEDURE InitStoneList;
BEGIN
  stoneX[0] := 54; stoneY[0] := 43;
  stoneX[1] := 71; stoneY[1] := 77;
  stoneX[2] := 78; stoneY[2] := 102;
  stoneX[3] := 66; stoneY[3] := 121;
  stoneX[4] := 12; stoneY[4] := 85;
  stoneX[5] := 79; stoneY[5] := 40;
  stoneX[6] := 107; stoneY[6] := 38;
  stoneX[7] := 73; stoneY[7] := 21;
  stoneX[8] := 12; stoneY[8] := 26;
  stoneX[9] := 26; stoneY[9] := 53;
  stoneX[10] := 84; stoneY[10] := 60
END InitStoneList;

PROCEDURE GetStoneCircle(i: INTEGER; VAR x, y: INTEGER);
BEGIN
  IF (i < 0) OR (i >= NumStoneCircles) THEN
    x := 0; y := 0;
    RETURN
  END;
  x := stoneX[i] * 256 + 128;
  y := stoneY[i] * 256 + 128
END GetStoneCircle;

PROCEDURE HandleStoneTeleport;
VAR sx, sy, i, dest, newX, newY: INTEGER;
BEGIN
  (* Must be on a stone circle — sector 144 *)
  sx := actors[0].absX DIV 256;
  sy := actors[0].absY DIV 256;
  FOR i := 0 TO 10 DO
    IF (stoneX[i] = sx) AND (stoneY[i] = sy) THEN
      (* Found current stone — teleport to next based on facing *)
      dest := (i + actors[0].facing + 1) MOD 11;
      newX := stoneX[dest] * 256 + BAND(CARDINAL(actors[0].absX), 255);
      newY := stoneY[dest] * 256 + BAND(CARDINAL(actors[0].absY), 255);
      colorPlayTimer := 32;  (* triggers palette cycling in render loop *)
      actors[0].absX := newX;
      actors[0].absY := newY;
      DEC(brothers[activeBrother].stuff[9]);  (* consume blue stone *)
      ShowMessage("The stone transports you!");
      SetOptions;
      RETURN
    END
  END;
  ShowMessage("The ring pulses but nothing happens.")
END HandleStoneTeleport;

PROCEDURE KillWeakEnemies;
VAR i: INTEGER;
BEGIN
  FOR i := 1 TO actorCount - 1 DO
    IF (actors[i].vitality > 0) AND
       (actors[i].actorType = TypeEnemy) AND
       (actors[i].race < 7) THEN
      actors[i].vitality := 0;
      actors[i].state := StDying;
      DEC(brothers[activeBrother].brave)
    END
  END;
  IF battleFlag THEN Event(34) END;
  ShowMessage("Dark energy destroys the enemies!")
END KillWeakEnemies;

PROCEDURE HandleMagic(optIdx: INTEGER);
VAR itemIdx, si, v: INTEGER;
    used: BOOLEAN;
BEGIN
  IF optIdx = 12 THEN GoMenu(MSpells); RETURN
  ELSIF optIdx = 13 THEN GoMenu(MStudy); RETURN
  ELSIF optIdx = 14 THEN GoMenu(MHerbs); RETURN
  END;
  itemIdx := optIdx - 5;  (* 0=Stone,1=Jewel,2=Vial,3=Orb,4=Totem,5=Ring,6=Skull *)
  IF (itemIdx < 0) OR (itemIdx > 6) THEN RETURN END;
  si := 9 + itemIdx;  (* stuff[9..15] *)
  IF brothers[activeBrother].stuff[si] <= 0 THEN
    Event(21); GoMenu(0); RETURN  (* "If only I had some magic!" *)
  END;
  used := TRUE;
  CASE itemIdx OF
    0: (* Blue Stone — teleport at stone rings *)
      HandleStoneTeleport;
      used := FALSE |  (* consumed inside if successful *)
    1: (* Green Jewel — night vision / light *)
      INC(lightTimer, 760);
      ShowMessage("Everything is bathed in green light!") |
    2: (* Glass Vial — restore vitality 4-11 pts *)
      v := (cycle MOD 8) + 4;  (* 4-11, matching rand8()+4 *)
      INC(actors[0].vitality, v);
      IF actors[0].vitality > (15 + brothers[activeBrother].brave DIV 4) THEN
        actors[0].vitality := 15 + brothers[activeBrother].brave DIV 4
      ELSE
        ShowMessage("That feels a lot better!")
      END |
    3: (* Crystal Orb — reveal hidden objects *)
      INC(secretTimer, 360);
      ShowMessage("Hidden things shimmer into view!") |
    4: (* Bird Totem — overhead map view *)
      viewStatus := ViewBird;
      ShowMessage("A bird's eye view!") |
    5: (* Gold Ring — freeze enemies ~5 sec *)
      INC(freezeTimer, 250);
      ShowMessage("Time stands still!") |
    6: (* Jade Skull — kill all enemies with race < 7 *)
      KillWeakEnemies
  ELSE
    GoMenu(0); RETURN
  END;
  IF used THEN
    DEC(brothers[activeBrother].stuff[si])
  END;
  SetOptions;
  GoMenu(0)
END HandleMagic;

PROCEDURE DamageNearbyEnemies(amount, radius: INTEGER);
VAR i, dx, dy: INTEGER;
BEGIN
  FOR i := 1 TO actorCount - 1 DO
    dx := actors[i].absX - actors[0].absX;
    dy := actors[i].absY - actors[0].absY;
    IF dx < 0 THEN dx := -dx END;
    IF dy < 0 THEN dy := -dy END;
    IF (actors[i].actorType = TypeEnemy) AND (actors[i].vitality > 0) AND
       (dx < radius) AND (dy < radius) THEN
      DEC(actors[i].vitality, amount);
      IF actors[i].vitality <= 0 THEN
        actors[i].vitality := 0;
        actors[i].state := StDying;
        actors[i].tactic := 7
      END
    END
  END
END DamageNearbyEnemies;

PROCEDURE FrightenNearbyEnemies(radius: INTEGER);
VAR i, dx, dy: INTEGER;
BEGIN
  FOR i := 1 TO actorCount - 1 DO
    dx := actors[i].absX - actors[0].absX;
    dy := actors[i].absY - actors[0].absY;
    IF dx < 0 THEN dx := -dx END;
    IF dy < 0 THEN dy := -dy END;
    IF (actors[i].actorType = TypeEnemy) AND (actors[i].vitality > 0) AND
       (actors[i].race < 7) AND (dx < radius) AND (dy < radius) THEN
      actors[i].goal := GoalFlee
    END
  END
END FrightenNearbyEnemies;

PROCEDURE HarvestNearby;
VAR i, id, dx, dy, count: INTEGER;
    picked: BOOLEAN;
    itemName: ARRAY [0..31] OF CHAR;
BEGIN
  count := 0;
  BeginLootSummary;
  FOR i := 0 TO objCount - 1 DO
    IF ((objects[i].status = 1) OR (objects[i].status = 5)) AND
       ((objects[i].region = currentRegion) OR (objects[i].region = -1)) THEN
      dx := objects[i].x - actors[0].absX;
      dy := objects[i].y - actors[0].absY;
      IF dx < 0 THEN dx := -dx END;
      IF dy < 0 THEN dy := -dy END;
      IF (dx < 100) AND (dy < 100) THEN
        id := objects[i].objId; picked := TRUE;
        CASE id OF
          13: AddWealth(50); AddLoot(LootGold, 50) |
          14, 15, 16: ContainerLoot |
          17: GiveStuff(14); AddLoot(14, 1) |
          18: GiveStuff(9); AddLoot(9, 1) |
          19: GiveStuff(10); AddLoot(10, 1) |
          22: GiveStuff(11); AddLoot(11, 1) |
          23: GiveStuff(13); AddLoot(13, 1) |
          24: GiveStuff(15); AddLoot(15, 1) |
          25: GiveStuff(16); AddLoot(16, 1) |
          26: GiveStuff(20); AddLoot(20, 1) |
          11: AddStuffN(8, 10); AddLoot(8, 10) |
           8: GiveStuff(2); AddLoot(2, 1) |
           9: GiveStuff(1); AddLoot(1, 1) |
          10: GiveStuff(3); AddLoot(3, 1) |
          12: GiveStuff(0); AddLoot(0, 1) |
         114: GiveStuff(18); AddLoot(18, 1) |
         145: GiveStuff(4); AddLoot(4, 1) |
         148: GiveStuff(24); AddLoot(24, 1) |
         149: GiveStuff(25); AddLoot(25, 1) |
         151: GiveStuff(6); AddLoot(6, 1) |
         153: GiveStuff(17); AddLoot(17, 1) |
         154: GiveStuff(21); AddLoot(21, 1) |
         242: GiveStuff(19); AddLoot(19, 1) |
          27: SetStuff(5, 1); AddLoot(5, 1) |
         138: SetStuff(29, 1); AddLoot(29, 1) |
         139: SetStuff(22, 1); AddLoot(22, 1) |
         140: SetStuff(30, 1); AddLoot(30, 1) |
         155: SetStuff(7, 1); AddLoot(7, 1) |
         ObjMandrake: GiveStuff(StMandrake); AddLoot(StMandrake, 1) |
         ObjWolfsbane: GiveStuff(StWolfsbane); AddLoot(StWolfsbane, 1) |
         ObjMugwort: GiveStuff(StMugwort); AddLoot(StMugwort, 1) |
         ObjYarrow: GiveStuff(StYarrow); AddLoot(StYarrow, 1) |
         ObjNightshade: GiveStuff(StNightshade); AddLoot(StNightshade, 1) |
         ObjBloodroot: GiveStuff(StBloodroot); AddLoot(StBloodroot, 1) |
         ObjWardScroll: GiveStuff(StWardScroll); AddLoot(StWardScroll, 1) |
         ObjFreezeScroll: GiveStuff(StFreezeScroll); AddLoot(StFreezeScroll, 1) |
         ObjFireScroll: GiveStuff(StFireScroll); AddLoot(StFireScroll, 1) |
         ObjFearScroll: GiveStuff(StFearScroll); AddLoot(StFearScroll, 1) |
         ObjLightScroll: GiveStuff(StLightScroll); AddLoot(StLightScroll, 1) |
         ObjSanctuaryScroll: GiveStuff(StSanctuaryScroll); AddLoot(StSanctuaryScroll, 1) |
         ObjHarvestScroll: GiveStuff(StHarvestScroll); AddLoot(StHarvestScroll, 1) |
         ObjHealScroll: GiveStuff(StHealScroll); AddLoot(StHealScroll, 1)
        ELSE
          picked := FALSE
        END;
        IF picked THEN objects[i].status := 2; INC(count) END
      END
    END
  END;
  FOR i := 1 TO actorCount - 1 DO
    IF actors[i].actorType = TypeEnemy THEN
      dx := actors[i].absX - actors[0].absX;
      dy := actors[i].absY - actors[0].absY;
      IF dx < 0 THEN dx := -dx END;
      IF dy < 0 THEN dy := -dy END;
      IF (dx < 100) AND (dy < 100) THEN
        IF TakeEnemyWeapon(i, itemName) THEN INC(count) END;
        IF TakeEnemyTreasure(i, itemName) THEN INC(count) END
      END
    END
  END;
  SetOptions;
  EndLootSummary;
  ShowLootSummary("Harvest gathered: ", "Harvest gathered nothing.")
END HarvestNearby;

PROCEDURE HandleSpell(optIdx: INTEGER);
BEGIN
  IF (optIdx < 5) OR (optIdx > 12) THEN GoMenu(0); RETURN END;
  IF NOT HasStuff(StWardScroll + optIdx - 5) THEN
    ShowMessage("You do not have that spell scroll."); GoMenu(0); RETURN
  END;
  CASE optIdx OF
    5: IF (brothers[activeBrother].stuff[StWolfsbane] < 1) OR
          (brothers[activeBrother].stuff[StYarrow] < 1) THEN
         ShowMessage("Ward requires Wolfsbane and Yarrow.")
       ELSE DEC(brothers[activeBrother].stuff[StWolfsbane]);
         DEC(brothers[activeBrother].stuff[StYarrow]); INC(wardTimer, 600);
         ShowMessage("A protective ward surrounds you.") END |
    6: IF (brothers[activeBrother].stuff[StWolfsbane] < 2) OR
          (brothers[activeBrother].stuff[StMugwort] < 1) THEN
         ShowMessage("Freeze requires 2 Wolfsbane and Mugwort.")
       ELSE DEC(brothers[activeBrother].stuff[StWolfsbane], 2);
         DEC(brothers[activeBrother].stuff[StMugwort]); INC(freezeTimer, 250);
         ShowMessage("The enemies are frozen in time.") END |
    7: IF (brothers[activeBrother].stuff[StBloodroot] < 1) OR
          (brothers[activeBrother].stuff[StNightshade] < 1) THEN
         ShowMessage("Fire requires Bloodroot and Nightshade.")
       ELSE DEC(brothers[activeBrother].stuff[StBloodroot]);
         DEC(brothers[activeBrother].stuff[StNightshade]);
         DamageNearbyEnemies(12, 120); ShowMessage("Fire strikes nearby enemies.") END |
    8: IF (brothers[activeBrother].stuff[StNightshade] < 1) OR
          (brothers[activeBrother].stuff[StBloodroot] < 1) THEN
         ShowMessage("Fear requires Nightshade and Bloodroot.")
       ELSE DEC(brothers[activeBrother].stuff[StNightshade]);
         DEC(brothers[activeBrother].stuff[StBloodroot]);
         FrightenNearbyEnemies(160); ShowMessage("Weaker enemies flee in fear.") END |
    9: IF brothers[activeBrother].stuff[StYarrow] < 1 THEN
         ShowMessage("Light requires Yarrow.")
       ELSE DEC(brothers[activeBrother].stuff[StYarrow]); INC(lightTimer, 760);
         ShowMessage("Everything is bathed in light.") END |
   10: IF (brothers[activeBrother].stuff[StWolfsbane] < 2) OR
          (brothers[activeBrother].stuff[StYarrow] < 2) THEN
         ShowMessage("Sanctuary requires 2 Wolfsbane and 2 Yarrow.")
       ELSE DEC(brothers[activeBrother].stuff[StWolfsbane], 2);
         DEC(brothers[activeBrother].stuff[StYarrow], 2); INC(sanctuaryTimer, 900);
         ShowMessage("Sanctuary prevents new enemy encounters.") END |
   11: IF (brothers[activeBrother].stuff[StMandrake] < 1) OR
          (brothers[activeBrother].stuff[StMugwort] < 1) THEN
         ShowMessage("Harvest requires Mandrake and Mugwort.")
       ELSE DEC(brothers[activeBrother].stuff[StMandrake]);
         DEC(brothers[activeBrother].stuff[StMugwort]); HarvestNearby END |
   12: IF (brothers[activeBrother].stuff[StWolfsbane] < 1) OR
          (brothers[activeBrother].stuff[StMandrake] < 1) THEN
         ShowMessage("Heal requires Wolfsbane and Mandrake.")
       ELSE DEC(brothers[activeBrother].stuff[StWolfsbane]);
         DEC(brothers[activeBrother].stuff[StMandrake]);
         INC(actors[0].vitality, 15); ShowMessage("Heal restores 15 health.") END
  ELSE GoMenu(0); RETURN
  END;
  SetOptions; GoMenu(0)
END HandleSpell;

PROCEDURE HandleStudy(optIdx: INTEGER);
BEGIN
  CASE optIdx OF
     5: ShowMessage("Ward: Wolfsbane + Yarrow. Reduces incoming damage.") |
     6: ShowMessage("Freeze: 2 Wolfsbane + Mugwort. Briefly stops enemies.") |
     7: ShowMessage("Fire: Bloodroot + Nightshade. Damages nearby enemies.") |
     8: ShowMessage("Fear: Nightshade + Bloodroot. Makes weaker enemies flee.") |
     9: ShowMessage("Light: Yarrow. Illuminates dark areas.") |
    10: ShowMessage("Sanctuary: 2 Wolfsbane + 2 Yarrow. Prevents new spawns.") |
    11: ShowMessage("Harvest: Mandrake + Mugwort. Takes nearby collectibles.") |
    12: ShowMessage("Heal: Wolfsbane + Mandrake. Restores 15 health.")
  ELSE END
END HandleStudy;

PROCEDURE ShowHerbCount(name, properties: ARRAY OF CHAR; stuffIdx: INTEGER);
VAR numStr: ARRAY [0..15] OF CHAR;
BEGIN
  Assign(name, msgBuf);
  Concat(msgBuf, ": ", msgBuf);
  IntToStr(brothers[activeBrother].stuff[stuffIdx], numStr);
  Concat(msgBuf, numStr, msgBuf); Concat(msgBuf, ". ", msgBuf);
  Concat(msgBuf, properties, msgBuf);
  ShowMessage(msgBuf)
END ShowHerbCount;

PROCEDURE HandleHerbs(optIdx: INTEGER);
BEGIN
  CASE optIdx OF
     5: ShowHerbCount("Mandrake", "Healing, growth, vitality.", StMandrake) |
     6: ShowHerbCount("Wolfsbane", "Protection, suppression.", StWolfsbane) |
     7: ShowHerbCount("Mugwort", "Perception, dreams, movement.", StMugwort) |
     8: ShowHerbCount("Yarrow", "Light, safety, navigation.", StYarrow) |
     9: ShowHerbCount("Nightshade", "Poison, fear, curses.", StNightshade) |
    10: ShowHerbCount("Bloodroot", "Direct damage, aggressive magic.", StBloodroot)
  ELSE
  END
END HandleHerbs;

PROCEDURE HandleCamp;
BEGIN
  IF currentRegion >= 8 THEN
    ShowMessage("You can only camp in the wild.")
  ELSIF battleFlag THEN
    ShowMessage("No time for that now!")
  ELSIF fatigue < 50 THEN
    ShowMessage("% is not sleepy.")
  ELSIF brothers[activeBrother].stuff[24] < 2 THEN
    ShowMessage("You need two apples to camp.")
  ELSE
    DEC(brothers[activeBrother].stuff[24], 2);
    sleepInBed := FALSE;
    actors[0].state := StSleep;
    ShowMessage("% makes camp and settles down to sleep.");
    SetOptions
  END;
  GoMenu(0)
END HandleCamp;

PROCEDURE HandleEat;
BEGIN
  IF potionCooldown # 0 THEN RETURN END;
  IF brothers[activeBrother].stuff[24] > 0 THEN
    DEC(brothers[activeBrother].stuff[24]);  (* consume apple *)
    IF hunger > 30 THEN DEC(hunger, 30) ELSE hunger := 0 END;
    Event(37);  (* "% ate one of his apples." *)
    SetOptions;
    potionCooldown := 30
  ELSIF UseItem(ItemFood) THEN
    INC(actors[0].vitality, 10);
    IF actors[0].vitality > (15 + brothers[activeBrother].brave DIV 4) THEN
      actors[0].vitality := 15 + brothers[activeBrother].brave DIV 4 END;
    IF hunger > 30 THEN DEC(hunger, 30) ELSE hunger := 0 END;
    ShowMessage("You eat some food."); potionCooldown := 30
  ELSE
    ShowMessage("No food!"); potionCooldown := 30
  END
END HandleEat;

PROCEDURE HandleMenuClick(mx, my: INTEGER);
CONST HudW = 640;
VAR hx, hy, col, row, itemIdx, optIdx: INTEGER;
BEGIN
  hx := mx * HudW DIV (ScreenW * Scale);
  hy := (my - PlayH * Scale) DIV Scale;
  IF hy < PanelY THEN RETURN END;
  IF hx < PanelX THEN RETURN END;
  IF hx >= PanelX + BtnW * 2 THEN RETURN END;
  col := (hx - PanelX) DIV BtnW;
  row := (hy - PanelY) DIV BtnH;
  IF row < 0 THEN RETURN END;
  IF row > 5 THEN RETURN END;
  itemIdx := row * 2 + col;
  IF itemIdx >= optionCount THEN RETURN END;
  optIdx := realOptions[itemIdx];
  IF optIdx < 0 THEN RETURN END;
  IF optIdx < 5 THEN
    IF optIdx = 3 THEN GoMenu(MTrade)
    ELSE GoMenu(optIdx) END;
    RETURN
  END;
  CASE cmode OF
    0: CASE optIdx OF
        5: ShowInventory | 6: HandleWorldPickup | 7: HandleLook |
        8: GoMenu(8) | 9: GoMenu(MDo)
      ELSE END |
    1: HandleMagic(optIdx) |
    2: CASE optIdx OF
        5: HandleYell | 6: HandleTalk | 7: HandleTalk
      ELSE END |
    3: HandleBuy(optIdx) |
    4: CASE optIdx OF
        5: TogglePause | 6: ToggleMusic | 7: |
        8: saveMode := TRUE; GoMenu(MFile) |  (* Save *)
        9: saveMode := FALSE; GoMenu(MFile) | (* Load *)
       10: running := FALSE                  (* Exit *)
      ELSE END |
    6: HandleKeys(optIdx) |
    7: HandleGive(optIdx) |
    8: CASE optIdx OF
        5, 6, 7, 8, 9:  (* Dirk, Mace, Sword, Bow, Wand — equip weapon *)
          IF HasWeapon(optIdx - 4) THEN
            actors[0].weapon := optIdx - 4;
            WeaponName(optIdx - 4, nameBuf);
            Assign("Equipped ", msgBuf); Concat(msgBuf, nameBuf, msgBuf);
            Concat(msgBuf, ".", msgBuf); ShowMessage(msgBuf)
          ELSE ShowMessage("% doesn't have one.") END;
          GoMenu(0) |
        10: (* Lasso — no USE effect, riding happens automatically *)
          GoMenu(0) |
        11: (* Shell — call turtle carrier.
              Original: blocked inside manor region (11194-21373, 10205-16208) *)
          IF HasStuff(6) THEN
            IF (actors[0].absX > 11194) AND (actors[0].absX < 21373) AND
               (actors[0].absY > 10205) AND (actors[0].absY < 16208) THEN
              ShowMessage("Nothing happens here.")
            ELSE
              SpawnTurtle;
              MoveExtent(1, actors[3].absX, actors[3].absY);
              ShowMessage("The turtle hears your call!")
            END
          ELSE ShowMessage("% doesn't have one.") END;
          GoMenu(0) |
        12: (* Key → Keys sub-menu *)
          GoMenu(6) |
        13: (* Sun Stone — only effective near witch *)
          GoMenu(0) |
        14: (* Book — no USE effect *)
          GoMenu(0)
      ELSE
        GoMenu(0)
      END |
    5: (* Save menu: Save=5, Exit=6 *)
      IF optIdx = 5 THEN
        saveMode := TRUE; GoMenu(9)  (* Save → File menu *)
      ELSIF optIdx = 6 THEN
        running := FALSE  (* Exit *)
      ELSE GoMenu(0) END |
    9: (* File menu: slots A-H = optIdx 5-12 *)
      IF (optIdx >= 5) AND (optIdx <= 12) THEN
        IF saveMode THEN
          SaveBrotherState;
          IF SaveGame(optIdx - 5, dayNight, fatigue, hunger, cycle,
                      lightTimer, secretTimer, freezeTimer, wardTimer,
                      sanctuaryTimer) THEN END
        ELSE
          IF LoadGame(optIdx - 5, dayNight, fatigue, hunger, cycle,
                      lightTimer, secretTimer, freezeTimer, wardTimer,
                      sanctuaryTimer) THEN
            dayPeriod := dayNight DIV 2000;
            viewStatus := ViewNormal;
            battleFlag := FALSE;
            prevBattle := FALSE;
            aftermathDone := FALSE;
            revealHidden := (secretTimer > 0);
            SetOptions
          END
        END
      END;
      GoMenu(0) |
   10: HandleSell(optIdx) |
   11: HandleSpell(optIdx) |
   12: HandleStudy(optIdx) |
   13: HandleHerbs(optIdx) |
   14: CASE optIdx OF
        5: OpenBuyMenu | 6: OpenSellMenu | 7: GoMenu(MGive)
      ELSE END |
   15: CASE optIdx OF
        5: HandleCamp | 6: HandleEat; GoMenu(0)
      ELSE END |
   16: HandleHerbBuy(optIdx) |
   17: HandleHerbSell(optIdx) |
   18: HandleScrollBuy(optIdx) |
   19: HandleAppleBuy(optIdx)
  ELSE END
END HandleMenuClick;

PROCEDURE HandleWorldPickup;
VAR id: INTEGER;
BEGIN
  id := CheckObjectPickup(actors[0].absX, actors[0].absY);
  IF id >= 0 THEN
    CASE id OF
      13: ShowMessage("Found 50 gold pieces!"); AddWealth(50) |
      14: ShowMessage("Opened a brass urn."); ContainerLoot |
      15: ShowMessage("Opened a chest."); ContainerLoot |
      16: ShowMessage("Opened some sacks."); ContainerLoot |
      17: ShowMessage("Found a gold ring!"); GiveStuff(14) |
      18: ShowMessage("Found a blue stone!"); GiveStuff(9) |
      19: ShowMessage("Found a green jewel!"); GiveStuff(10) |
      20: Event(17);
          IF currentRegion > 7 THEN Event(19)
          ELSE Event(18) END |
      22: ShowMessage("Found a vial!"); GiveStuff(11) |
      23: ShowMessage("Found a totem!"); GiveStuff(13) |
      24: ShowMessage("Found a skull!"); GiveStuff(15) |
      25: ShowMessage("Found a gold key!"); GiveStuff(16) |
      26: ShowMessage("Found a grey key!"); GiveStuff(20) |
      11: ShowMessage("Found a quiver of arrows!"); AddStuffN(8, 10) |
       8: ShowMessage("Found a sword!"); GiveStuff(2) |
       9: ShowMessage("Found a mace!"); GiveStuff(1) |
      10: ShowMessage("Found a bow!"); GiveStuff(3) |
      12: ShowMessage("Found a dirk!"); GiveStuff(0) |
     102: (* Turtle eggs — not pickable, part of quest scenery *) |
     114: ShowMessage("Found a blue key!"); GiveStuff(18) |
     145: ShowMessage("Found a magic wand!"); GiveStuff(4) |
     148: ShowMessage("Found some fruit!"); GiveStuff(24) |
     151: ShowMessage("Found a shell!"); GiveStuff(6) |
     153: ShowMessage("Found a green key!"); GiveStuff(17) |
     154: ShowMessage("Found a white key!"); GiveStuff(21) |
     ObjMandrake: ShowMessage("Found Mandrake!"); GiveStuff(StMandrake) |
     ObjWolfsbane: ShowMessage("Found Wolfsbane!"); GiveStuff(StWolfsbane) |
     ObjMugwort: ShowMessage("Found Mugwort!"); GiveStuff(StMugwort) |
     ObjYarrow: ShowMessage("Found Yarrow!"); GiveStuff(StYarrow) |
     ObjNightshade: ShowMessage("Found Nightshade!"); GiveStuff(StNightshade) |
     ObjBloodroot: ShowMessage("Found Bloodroot!"); GiveStuff(StBloodroot) |
     ObjWardScroll: ShowMessage("Found the Ward scroll!"); GiveStuff(StWardScroll) |
     ObjFreezeScroll: ShowMessage("Found the Freeze scroll!"); GiveStuff(StFreezeScroll) |
     ObjFireScroll: ShowMessage("Found the Fire scroll!"); GiveStuff(StFireScroll) |
     ObjFearScroll: ShowMessage("Found the Fear scroll!"); GiveStuff(StFearScroll) |
     ObjLightScroll: ShowMessage("Found the Light scroll!"); GiveStuff(StLightScroll) |
     ObjSanctuaryScroll: ShowMessage("Found the Sanctuary scroll!"); GiveStuff(StSanctuaryScroll) |
     ObjHarvestScroll: ShowMessage("Found the Harvest scroll!"); GiveStuff(StHarvestScroll) |
     ObjHealScroll: ShowMessage("Found the Heal scroll!"); GiveStuff(StHealScroll) |
     242: ShowMessage("Found a red key!"); GiveStuff(19) |
      27: ShowMessage("% found the Golden Lasso!"); SetStuff(5, 1) |
     138: ShowMessage("% found the King's Bone!"); SetStuff(29, 1) |
     139: ShowMessage("% found the Talisman!"); SetStuff(22, 1);
          ShowMessage("The quest is complete!") |
     140: ShowMessage("% found a Shard!"); SetStuff(30, 1) |
     155: ShowMessage("% found the Sun Stone!"); SetStuff(7, 1) |
     149: ShowMessage("Found a gold statue!"); GiveStuff(25) |
      28: ShowMessage("% found his brother's bones.");
          InheritBrotherItems
    ELSE ShowMessage("Found something!") END;
    SetOptions
  ELSE SearchNearbyCorpses END
END HandleWorldPickup;

PROCEDURE HandleTalk;
VAR speech: ARRAY [0..127] OF CHAR;
BEGIN
  IF TalkToCarrier(speech) THEN ShowMessage(speech)
  ELSIF TalkToNPC(actors[0].absX, actors[0].absY, speech) THEN
    IF speech[0] # 0C THEN ShowMessage(speech) END
  ELSE ShowMessage("Nobody to talk to here.") END
END HandleTalk;

PROCEDURE HandleYell;
BEGIN
  (* Original: yelling doesn't interact with NPCs.
     Just a generic shout — no side effects. *)
  ShowMessage("Nobody seems to hear.")
END HandleYell;

(* --- Witch eye beam update ---
   Original: fmain.c lines 3270-3287. Beam sweeps toward player,
   deals 1-2 damage when player is inside the beam arc and within 100px. *)

(* Direction vectors for swan velocity — original xDir/yDir values *)
PROCEDURE NewXDir(d: INTEGER): INTEGER;
BEGIN
  CASE d OF
    0: RETURN  0 | 1: RETURN  2 | 2: RETURN  2 | 3: RETURN  2 |
    4: RETURN  0 | 5: RETURN -2 | 6: RETURN -2 | 7: RETURN -2
  ELSE RETURN 0
  END
END NewXDir;

PROCEDURE NewYDir(d: INTEGER): INTEGER;
BEGIN
  CASE d OF
    0: RETURN -2 | 1: RETURN -2 | 2: RETURN  0 | 3: RETURN  2 |
    4: RETURN  2 | 5: RETURN  2 | 6: RETURN  0 | 7: RETURN -2
  ELSE RETURN 0
  END
END NewYDir;

PROCEDURE UpdateWitch;
VAR i, dx, dy, dist, rng, pAngle, aDiff: INTEGER;
BEGIN
  (* Check if witch (race 9, TypeSetfig) is on screen *)
  witchFlag := FALSE;
  FOR i := 1 TO actorCount - 1 DO
    IF (actors[i].actorType = TypeSetfig) AND
       (actors[i].race = 9) AND
       (actors[i].state # StDead) AND
       (actors[i].state # StDying) THEN
      witchFlag := TRUE;
      dx := actors[0].absX - actors[i].absX;
      dy := actors[0].absY - actors[i].absY;
      IF dx < 0 THEN dist := -dx ELSE dist := dx END;
      IF dy < 0 THEN
        IF -dy > dist THEN dist := -dy END
      ELSE
        IF dy > dist THEN dist := dy END
      END;

      (* Face the player — original set_course(i, hero_x, hero_y, 0) *)
      IF (dx > 3) AND (dy < -3) THEN actors[i].facing := 1
      ELSIF (dx > 3) AND (dy > 3) THEN actors[i].facing := 3
      ELSIF (dx < -3) AND (dy > 3) THEN actors[i].facing := 5
      ELSIF (dx < -3) AND (dy < -3) THEN actors[i].facing := 7
      ELSIF dx > 3 THEN actors[i].facing := 2
      ELSIF dx < -3 THEN actors[i].facing := 6
      ELSIF dy < -3 THEN actors[i].facing := 0
      ELSIF dy > 3 THEN actors[i].facing := 4
      END;

      (* Rotate beam toward player — change direction ~12% of frames.
         Original: rand4()==0 (25%) but our frame rate is higher so slow it down. *)
      witchRng := witchRng * 1103515245 + 12345;
      IF witchRng < 0 THEN witchRng := -witchRng END;
      IF (witchRng DIV 65536) MOD 8 = 0 THEN
        IF witchS1 > 0 THEN witchDir := -1
        ELSE witchDir := 1 END
      END;
      (* Rotate every other frame for slower sweep *)
      IF BAND(CARDINAL(cycle), 1) = 0 THEN
        INC(witchIndex, witchDir);
        IF witchIndex > 63 THEN witchIndex := 0
        ELSIF witchIndex < 0 THEN witchIndex := 63
        END
      END;

      (* Damage if player inside beam arc and within 100px.
         Compute player angle from witch and compare to beam angle.
         Each witchIndex step = 360/64 = 5.6 degrees. Allow +-2 steps. *)
      IF dist < 100 THEN
        (* Approximate player angle from witch, mapped to 0-63.
           witchpoints[0]=(0,100)=S, [8]=(70,70)=SE, [16]=(100,0)=E,
           [24]=(70,-71)=NE, [32]=(0,-100)=N, [40]=(-71,-71)=NW,
           [48]=(-100,0)=W, [56]=(-71,70)=SW *)
        IF (dx = 0) AND (dy = 0) THEN
          pAngle := witchIndex  (* on top of witch = always hit *)
        ELSIF (dx > 3) AND (dy > 3) THEN pAngle := 8    (* SE *)
        ELSIF (dx > 3) AND (dy < -3) THEN pAngle := 24  (* NE *)
        ELSIF (dx < -3) AND (dy < -3) THEN pAngle := 40 (* NW *)
        ELSIF (dx < -3) AND (dy > 3) THEN pAngle := 56  (* SW *)
        ELSIF dx > 3 THEN pAngle := 16   (* E *)
        ELSIF dx < -3 THEN pAngle := 48  (* W *)
        ELSIF dy > 3 THEN pAngle := 0    (* S *)
        ELSE pAngle := 32                (* N *)
        END;
        (* Check if beam angle is within +-4 steps of player angle *)
        aDiff := (witchIndex - pAngle + 64) MOD 64;
        IF aDiff > 32 THEN aDiff := 64 - aDiff END;
        IF aDiff < 5 THEN
          IF BAND(CARDINAL(cycle), 7) = 0 THEN
            witchRng := witchRng * 1103515245 + 12345;
            IF witchRng < 0 THEN witchRng := -witchRng END;
            rng := (witchRng DIV 65536) MOD 2 + 1;
            IF wardTimer > 0 THEN rng := (rng + 1) DIV 2 END;
            DEC(actors[0].vitality, rng);
            IF actors[0].vitality <= 0 THEN
              actors[0].vitality := 0;
              actors[0].state := StDying;
              actors[0].tactic := 7
            END
          END
        END
      END;
      RETURN  (* only one witch *)
    END
  END
END UpdateWitch;

PROCEDURE CheckEnvironment;
VAR sec, terrain: INTEGER;
BEGIN
  (* Skip if already dying/dead — original checkdead handles transition *)
  IF (actors[0].state = StDying) OR (actors[0].state = StDead) THEN RETURN END;

  (* Fall state processing — original fmain.c:1732-1739.
     Tactic counts up, velocity dampens 75%/frame.
     NO vitality loss. After 30 frames, fairy revives to StStill.
     Only the initial -2 luck on entry. *)
  IF actors[0].state = StFall THEN
    IF actors[0].tactic < 30 THEN
      INC(actors[0].tactic);
      actors[0].velX := (actors[0].velX * 3) DIV 4;
      actors[0].velY := (actors[0].velY * 3) DIV 4;
      actors[0].absX := actors[0].absX + actors[0].velX DIV 4;
      actors[0].absY := actors[0].absY + actors[0].velY DIV 4
    ELSE
      (* Fall animation complete — fairy revives on nearest PATH tile.
         Path tiles have terrain 6 (fast), 7 (slippery), or 8 (backward).
         Void returns 0 or 9. Must find an actual path tile. *)
      actors[0].state := StStill;
      actors[0].velX := 0;
      actors[0].velY := 0;
      FOR sec := 1 TO 64 DO
        terrain := GetTerrainAt(actors[0].absX, actors[0].absY - sec);
        IF (terrain >= 6) AND (terrain <= 8) THEN
          DEC(actors[0].absY, sec); sec := 64
        ELSE
          terrain := GetTerrainAt(actors[0].absX, actors[0].absY + sec);
          IF (terrain >= 6) AND (terrain <= 8) THEN
            INC(actors[0].absY, sec); sec := 64
          ELSE
            terrain := GetTerrainAt(actors[0].absX + sec, actors[0].absY);
            IF (terrain >= 6) AND (terrain <= 8) THEN
              INC(actors[0].absX, sec); sec := 64
            ELSE
              terrain := GetTerrainAt(actors[0].absX - sec, actors[0].absY);
              IF (terrain >= 6) AND (terrain <= 8) THEN
                DEC(actors[0].absX, sec); sec := 64
              END
            END
          END
        END
      END;
      ShowMessage("The good fairy catches you!")
    END;
    RETURN
  END;

  (* Terrain 9 falling — original fmain.c:2311-2322.
     Only in astral plane (xtype=52). Player falls into void. *)
  IF (xtype = 52) AND (currentRegion >= 8) THEN
    terrain := GetTerrainAt(actors[0].absX, actors[0].absY);
    IF terrain = 9 THEN
      IF actors[0].state # StFall THEN
        actors[0].state := StFall;
        actors[0].tactic := 0;
        DecLuck(2)
        (* No music change — astral MoodSpec persists during fall.
           Original: setmood(TRUE) runs priority check, astral coords
           take priority over battleflag. *)
      END
    END
  END;

  (* Original fmain.c:2340-2353 — sector 181 quicksand/whirlpool.
     When environ reaches 30 in sector 181, teleport to underground
     instead of drowning. xfer(0x1080, 34950, FALSE) = (4224, 34950). *)
  IF actors[0].environ = 30 THEN
    sec := GetMapSector(actors[0].absX, actors[0].absY);
    IF sec = 181 THEN
      actors[0].environ := 0;
      actors[0].absX := 4224;
      actors[0].absY := 34950;
      SwitchRegion(9);
      actorCount := 1;
      ResetMaterialized;
      ShowMessage("The ground swallows you up!");
      RETURN
    END
  END;

  (* Original fmain.c:2434-2442 — lava/fire damage.
     fiery_death zone: cam 8802-13562, 24744-29544.
     environ 3-15: -1 vit/frame. environ > 15: instant death.
     stuff[23] (Fruit) protects completely. *)
  IF (camX > 8802) AND (camX < 13562) AND
     (camY > 24744) AND (camY < 29544) AND
     (NOT cheatGod) THEN
    IF brothers[activeBrother].stuff[23] > 0 THEN  (* Rose = stuff[23] *)
      (* Rose protects from lava — original: stuff[23] *)
      actors[0].environ := 0
    ELSIF actors[0].environ > 15 THEN
      actors[0].vitality := 0;
      actors[0].state := StDying;
      actors[0].tactic := 7;
      Event(27)  (* "% perished in the hot lava!" *)
    ELSIF actors[0].environ > 2 THEN
      DEC(actors[0].vitality);
      IF actors[0].vitality <= 0 THEN
        actors[0].vitality := 0;
        actors[0].state := StDying;
        actors[0].tactic := 7;
        Event(27)
      END
    END
  END;

  (* Original fmain.c:2444-2451 — drowning at environ 30.
     Every 8 frames, decrement vitality. Event 6. *)
  IF (actors[0].environ = 30) AND (BAND(CARDINAL(cycle), 7) = 0) AND
     (NOT cheatGod) THEN
    DEC(actors[0].vitality);
    IF actors[0].vitality <= 0 THEN
      actors[0].vitality := 0;
      actors[0].state := StDying;
      actors[0].tactic := 7;
      Event(6)  (* "% was drowned in the water!" *)
    ELSIF BAND(CARDINAL(cycle), 31) = 0 THEN
      ShowMessage("% is drowning!")
    END
  END
END CheckEnvironment;

(* --- Sleep/Fatigue system --- *)

PROCEDURE CheckBedTile;
VAR sec: INTEGER;
BEGIN
  IF currentRegion # 8 THEN sleepWait := 0; RETURN END;
  sec := GetSectorByte(actors[0].absX, actors[0].absY);
  IF (sec = 161) OR (sec = 52) OR (sec = 162) OR (sec = 53) THEN
    INC(sleepWait);
    IF sleepWait = 30 THEN
      IF fatigue < 50 THEN Event(25)
      ELSE
        Event(26);
        actors[0].absY := BOR(INTEGER(CARDINAL(actors[0].absY)), 31);
        sleepInBed := TRUE;
        actors[0].state := StSleep
      END
    END
  ELSE sleepWait := 0 END
END CheckBedTile;

PROCEDURE UpdateSleep;
BEGIN
  IF actors[0].state # StSleep THEN RETURN END;
  INC(dayNight, 63);
  IF dayNight >= 24000 THEN DEC(dayNight, 24000) END;
  IF fatigue > 0 THEN DEC(fatigue) END;
  IF (fatigue = 0) OR
     ((fatigue < 30) AND (dayNight > 9000) AND (dayNight < 10000)) OR
     (battleFlag AND (cycle MOD 64 = 0)) THEN
    actors[0].state := StStill;
    IF sleepInBed THEN
      actors[0].absY := BAND(CARDINAL(actors[0].absY), 65504)
    END;
    sleepInBed := FALSE;
    Event(14)
  END
END UpdateSleep;

PROCEDURE UpdateFatigue;
BEGIN
  IF actors[0].state = StSleep THEN RETURN END;
  IF actors[0].vitality < 1 THEN RETURN END;
  IF BAND(CARDINAL(dayNight), 127) # 0 THEN RETURN END;
  INC(hunger); INC(fatigue);
  IF hunger = 35 THEN Event(0)
  ELSIF hunger = 60 THEN Event(1)
  ELSIF BAND(CARDINAL(hunger), 7) = 0 THEN
    IF actors[0].vitality > 5 THEN
      IF (hunger > 100) OR (fatigue > 160) THEN DEC(actors[0].vitality, 2) END;
      IF hunger > 90 THEN Event(2) END
    ELSIF fatigue > 170 THEN
      Event(12); sleepInBed := FALSE; actors[0].state := StSleep
    ELSIF hunger > 140 THEN
      Event(24); hunger := 130; sleepInBed := FALSE; actors[0].state := StSleep
    END
  END;
  IF fatigue = 70 THEN Event(3)
  ELSIF fatigue = 90 THEN Event(4) END
END UpdateFatigue;

(* --- Battle aftermath --- *)

PROCEDURE BattleAftermath;
VAR i, dead, flee: INTEGER;
    numStr: ARRAY [0..7] OF CHAR;
BEGIN
  IF actors[0].vitality < 1 THEN RETURN END;
  dead := 0; flee := 0;
  (* Count dead AND dying — dying enemies are effectively defeated *)
  FOR i := 4 TO actorCount - 1 DO
    IF actors[i].actorType = TypeEnemy THEN
      IF (actors[i].state = StDead) OR (actors[i].state = StDying) THEN INC(dead)
      ELSIF actors[i].goal = GoalFlee THEN INC(flee) END
    END
  END;
  IF (actors[0].vitality < 5) AND (dead > 0) THEN ShowMessage("Bravely done!")
  ELSE
    IF dead > 0 THEN
      IntToStr(dead, numStr); Assign(numStr, msgBuf);
      Concat(msgBuf, " foes were defeated in battle.", msgBuf); ShowMessage(msgBuf)
    END;
    IF flee > 0 THEN
      IntToStr(flee, numStr); Assign(numStr, msgBuf);
      Concat(msgBuf, " foes fled in retreat.", msgBuf); ShowMessage(msgBuf)
    END
  END;
  (* Turtle eggs: spawn turtle after guards defeated *)
  IF turtleEggs THEN
    SpawnTurtle;
    MoveExtent(1, actors[3].absX, actors[3].absY);
    turtleEggs := FALSE;
    turtleEggsDone := TRUE;
    ShowMessage("The turtle appears, grateful you saved its eggs!")
  END
END BattleAftermath;

PROCEDURE PointerDirection(mx, my: INTEGER): INTEGER;
VAR dx, dy, ax, ay: INTEGER;
BEGIN
  dx := mx - (actors[0].absX - camX) * Scale;
  dy := my - (actors[0].absY - camY) * Scale;
  ax := dx; ay := dy;
  IF ax < 0 THEN ax := -ax END;
  IF ay < 0 THEN ay := -ay END;
  IF (ax <= 4 * Scale) AND (ay <= 4 * Scale) THEN RETURN DirNone END;
  IF ax > ay * 2 THEN
    IF dx < 0 THEN RETURN DirW ELSE RETURN DirE END
  ELSIF ay > ax * 2 THEN
    IF dy < 0 THEN RETURN DirN ELSE RETURN DirS END
  ELSIF dx < 0 THEN
    IF dy < 0 THEN RETURN DirNW ELSE RETURN DirSW END
  ELSE
    IF dy < 0 THEN RETURN DirNE ELSE RETURN DirSE END
  END
END PointerDirection;

PROCEDURE UpdatePlayer;
VAR newX, newY: INTEGER;
BEGIN
  IF input.quit THEN running := FALSE; RETURN END;
  IF actors[0].state = StSleep THEN RETURN END;
  IF actors[0].state = StFall THEN RETURN END;  (* no input during fall *)
  (* Player DYING → DEAD transition: tactic counts down from 7 *)
  IF actors[0].state = StDying THEN
    IF actors[0].tactic = 7 THEN
      SetMood(MoodDeath)  (* death music starts immediately, replaces battle *)
    END;
    DEC(actors[0].tactic);
    IF actors[0].tactic <= 0 THEN
      actors[0].state := StDead;
      deathTimer := 0;  (* counts UP, maps to goodfairy 255→1 *)
      ActiveName(nameBuf); Assign(nameBuf, msgBuf);
      Concat(msgBuf, " has fallen!", msgBuf); ShowMessage(msgBuf);
      DecLuck(5)
    END;
    RETURN
  END;
  IF actors[0].state = StDead THEN
    INC(deathTimer);
    (* Map to original goodfairy countdown: gf = 255 - deathTimer *)
    (* Fairy flies in from right, stops just right of dead hero,
       then hovers there until brother switch.
       Death music is ~15 seconds. At 33ms/frame that's ~450 frames. *)
    IF (deathTimer > 150) AND (deathTimer <= 400) THEN
      (* Flying in *)
      fairyActive := TRUE;
      fairyX := actors[0].absX + 16 + (400 - deathTimer)
    ELSIF deathTimer > 400 THEN
      (* Hovering at final position *)
      fairyActive := TRUE;
      fairyX := actors[0].absX + 16
    ELSE
      fairyActive := FALSE
    END;
    (* Revive after fairy sequence completes — minimum 400 frames for fairy,
       then wait for death music to finish, or skip with key press *)
    IF (deathTimer > 9000) OR ((deathTimer > 400) AND (NOT IsPlaying())) THEN
      StopMusic;
      fairyActive := FALSE;

      IF brothers[activeBrother].luck >= 1 THEN
        (* SAME CHARACTER RESPAWN — luck still positive.
           Original: revive(FALSE) — respawn at safe location,
           restore vitality, reset hunger/fatigue. *)
        actors[0].state := StStill;
        actors[0].vitality := 15 + brothers[activeBrother].brave DIV 4;
        actors[0].environ := 0;
        hunger := 0; fatigue := 0;
        dayNight := 8000;
        dayPeriod := 4;  (* sync period so morning announcement doesn't fire *)
        lightTimer := 0; secretTimer := 0; freezeTimer := 0;
        wardTimer := 0; sanctuaryTimer := 0;
        actorCount := 1;  (* clear enemies and materialized NPC actors *)
        ResetMaterialized;
        deathTimer := 0;
        ShowMessage("The good fairy has revived you!")
      ELSE
        (* BROTHER SWITCH — luck exhausted.
           Original: revive(TRUE) — new brother at Tambry.
           Place bones at death location, make ghost visible at stone ring. *)
        AddObj(actors[0].absX, actors[0].absY, 28, 1, -1);
        (* Ghost brother appears — original: ob_listg[brother+2].ob_stat = 3.
           Our ghost objects are at indices 0 (Julian) and 1 (Philip). *)
        IF activeBrother <= 1 THEN
          objects[activeBrother].status := 3
        END;
        IF SwitchToNext() THEN
          actors[0].absX := 19036; actors[0].absY := 15755;
          actors[0].state := StStill;
          actors[0].vitality := 15 + brothers[activeBrother].brave DIV 4;
          actors[0].weapon := 1;
          actors[0].environ := 0;
          actors[0].facing := 4;
          hunger := 0; fatigue := 0;
          dayNight := 8000;
          dayPeriod := 4;
          lightTimer := 0; secretTimer := 0; freezeTimer := 0;
          wardTimer := 0; sanctuaryTimer := 0;
          RestoreDoorTiles;
          SwitchRegion(3);
          InitPlace(actors[0].absX, actors[0].absY, 3);
          actorCount := 1;
          ResetMaterialized;
          Event(9);
          IF activeBrother = 1 THEN Event(10)
          ELSIF activeBrother = 2 THEN Event(11) END;
          deathTimer := 0;
          SetOptions
        ELSE
          ShowMessage("All brothers have fallen... Game Over.");
          deathTimer := -1
        END
      END
    END;
    RETURN
  END;
  IF potionCooldown > 0 THEN DEC(potionCooldown) END;
  IF input.usePotion AND (potionCooldown = 0) THEN
    IF UseItem(ItemPotion) THEN
      INC(actors[0].vitality, 30);
      IF actors[0].vitality > (15 + brothers[activeBrother].brave DIV 4) THEN
        actors[0].vitality := 15 + brothers[activeBrother].brave DIV 4 END;
      ShowMessage("Potion restores your health!"); potionCooldown := 30
    ELSE ShowMessage("No potions!"); potionCooldown := 30 END
  END;
  IF input.useFood THEN HandleEat END;
  IF input.talk AND (potionCooldown = 0) THEN HandleTalk; potionCooldown := 30 END;
  (* Slippery/void terrain (environ=-2): velocity-based movement.
     Original fmain.c:1580-1598. Same system as swan but with proxcheck
     and lower speed cap. Used in astral plane. *)
  IF (actors[0].environ = -2) AND (riding # RideSwan) THEN
    IF input.dirKey # DirNone THEN
      actors[0].facing := input.dirKey;
      INC(actors[0].velX, NewXDir(input.dirKey));
      INC(actors[0].velY, NewYDir(input.dirKey));
      IF actors[0].velX > 34 THEN actors[0].velX := 34
      ELSIF actors[0].velX < -34 THEN actors[0].velX := -34 END;
      IF actors[0].velY > 42 THEN actors[0].velY := 42
      ELSIF actors[0].velY < -42 THEN actors[0].velY := -42 END;
      actors[0].state := StWalking
    END;
    (* Apply velocity with proxcheck *)
    newX := actors[0].absX + actors[0].velX DIV 4;
    newY := actors[0].absY + actors[0].velY DIV 4;
    IF ProxCheck(newX, newY, 0) = 0 THEN
      actors[0].absX := newX;
      actors[0].absY := newY
    ELSE
      actors[0].velX := 0; actors[0].velY := 0;
      actors[0].environ := 0  (* stop sliding — original: k=0 *)
    END;
    RETURN
  ELSE
    (* Not on slippery terrain — clear any residual velocity *)
    actors[0].velX := 0; actors[0].velY := 0
  END;

  (* Swan flight — velocity-based movement, bypasses all terrain.
     Original fmain.c:2031-2053: environ==-2 path. *)
  IF riding = RideSwan THEN
    actors[0].environ := -2;  (* airborne — set every frame *)
    IF input.attack THEN
      swanDismount := TRUE
    ELSIF input.dirKey # DirNone THEN
      (* Accelerate in facing direction.
         Original: nvx = vel_x + newx(20,d,2)-20 = vel_x + xDir[d]
         Velocity capped at +-32 (X) and +-40 (Y). *)
      actors[0].facing := input.dirKey;
      INC(actors[0].velX, NewXDir(input.dirKey));
      INC(actors[0].velY, NewYDir(input.dirKey));
      IF actors[0].velX > 32 THEN actors[0].velX := 32
      ELSIF actors[0].velX < -32 THEN actors[0].velX := -32 END;
      IF actors[0].velY > 40 THEN actors[0].velY := 40
      ELSIF actors[0].velY < -40 THEN actors[0].velY := -40 END;
      actors[0].state := StWalking
    END;
    (* Apply velocity — no terrain check, fly over everything *)
    actors[0].absX := actors[0].absX + actors[0].velX DIV 4;
    actors[0].absY := actors[0].absY + actors[0].velY DIV 4;
    (* Clamp to world bounds *)
    IF actors[0].absX < 0 THEN actors[0].absX := 0; actors[0].velX := 0 END;
    IF actors[0].absY < 0 THEN actors[0].absY := 0; actors[0].velY := 0 END;
    IF actors[0].absX > 32767 THEN actors[0].absX := 32767; actors[0].velX := 0 END;
    IF actors[0].absY > 40959 THEN actors[0].absY := 40959; actors[0].velY := 0 END;
    RETURN
  END;
  IF input.attack AND (swanCooldown = 0) THEN
    IF (actors[0].weapon >= 4) AND (actors[0].state # StShoot1) THEN
      IF (actors[0].weapon = 4) AND
         (brothers[activeBrother].stuff[8] <= 0) THEN
        ShowMessage("No Arrows!")
      ELSE
        actors[0].state := StShoot1; FireMissile(0);
        (* Deplete arrow for bow, not for wand *)
        IF actors[0].weapon = 4 THEN
          DEC(brothers[activeBrother].stuff[8])
        END
      END;
      actors[0].velX := 0; actors[0].velY := 0
    ELSIF actors[0].weapon >= 4 THEN
      actors[0].velX := 0; actors[0].velY := 0
    ELSE
      actors[0].state := StFighting; actors[0].velX := 0; actors[0].velY := 0
    END
  ELSIF (actors[0].state = StFighting) OR (actors[0].state = StShoot1) THEN
    actors[0].state := StStill; actors[0].velX := 0; actors[0].velY := 0
  ELSIF input.dirKey # DirNone THEN
    (* Terrain 8 (environ=-3): reversed controls — original e=-2, dex^=7 *)
    IF actors[0].environ = -3 THEN
      actors[0].facing := BAND(CARDINAL(input.dirKey + 4), 7);
      IF MoveActor(0, actors[0].facing, 2) THEN actors[0].state := StWalking
      ELSE actors[0].state := StStill END
    ELSE
      actors[0].facing := input.dirKey;
      IF MoveActor(0, input.dirKey, 1) THEN actors[0].state := StWalking
      ELSE actors[0].state := StStill END
    END
  ELSE
    actors[0].state := StStill; actors[0].velX := 0; actors[0].velY := 0
  END
END UpdatePlayer;

PROCEDURE CheckDoors;
VAR newX, newY, newReg: INTEGER;
    onDoor: BOOLEAN;
BEGIN
  IF doorCooldown > 0 THEN DEC(doorCooldown); RETURN END;
  onDoor := FALSE;
  IF currentRegion < 8 THEN
    IF (GetTerrainAt(actors[0].absX, actors[0].absY) = 15) OR
       (GetTerrainAt(actors[0].absX + 4, actors[0].absY) = 15) OR
       (GetTerrainAt(actors[0].absX - 4, actors[0].absY) = 15) OR
       (GetTerrainAt(actors[0].absX, actors[0].absY + 4) = 15) OR
       (GetTerrainAt(actors[0].absX, actors[0].absY - 4) = 15) THEN
      onDoor := TRUE
    END
  ELSE onDoor := TRUE END;
  CheckCloseDoors(actors[0].absX, actors[0].absY);
  IF CheckDoor(actors[0].absX, actors[0].absY, currentRegion,
               newX, newY, newReg) THEN
    actors[0].absX := newX; actors[0].absY := newY;
    IF newReg >= 0 THEN RestoreDoorTiles; SwitchRegion(newReg)
    ELSE RestoreDoorTiles; SwitchRegion(DetectRegion(newX, newY)) END;
    (* Nudge player out of impassable terrain — try all 4 directions.
       Terrain 1 = rock/trees, >= 10 = walls/doors. *)
    newReg := GetTerrainAt(actors[0].absX, actors[0].absY);
    IF (newReg = 1) OR (newReg >= 10) THEN
      FOR newX := 1 TO 16 DO
        newReg := GetTerrainAt(actors[0].absX, actors[0].absY - newX);
        IF (newReg # 1) AND (newReg < 10) THEN
          DEC(actors[0].absY, newX); newX := 16
        ELSE
          newReg := GetTerrainAt(actors[0].absX, actors[0].absY + newX);
          IF (newReg # 1) AND (newReg < 10) THEN
            INC(actors[0].absY, newX); newX := 16
          ELSE
            newReg := GetTerrainAt(actors[0].absX + newX, actors[0].absY);
            IF (newReg # 1) AND (newReg < 10) THEN
              INC(actors[0].absX, newX); newX := 16
            ELSE
              newReg := GetTerrainAt(actors[0].absX - newX, actors[0].absY);
              IF (newReg # 1) AND (newReg < 10) THEN
                DEC(actors[0].absX, newX); newX := 16
              END
            END
          END
        END
      END
    END;
    InitPlace(actors[0].absX, actors[0].absY, currentRegion);
    doorCooldown := 20  (* reduced from 60 — prevents same-door bounce *)
  END
END CheckDoors;

PROCEDURE UpdateGame;
BEGIN
  input.quit := FALSE; input.dirKey := DirNone;
  input.attack := FALSE; input.usePotion := FALSE;
  input.useFood := FALSE; input.talk := FALSE; input.toggleMap := FALSE;
  PollInput(input);
  IF input.menuKey = CHR(27) THEN
    WriteString("Player coordinates: x=");
    WriteInt(actors[0].absX, 0);
    WriteString(" y=");
    WriteInt(actors[0].absY, 0);
    WriteString(" region=");
    WriteInt(currentRegion, 0);
    WriteLn
  END;
  IF (input.dirKey = DirNone) AND input.mouseMove THEN
    input.dirKey := PointerDirection(input.mouseX, input.mouseY)
  END;
  IF viewStatus # ViewNormal THEN
    IF input.quit THEN running := FALSE; RETURN END;
    IF input.attack OR (input.menuKey # 0C) OR
       input.mouseClick OR (input.dirKey # DirNone) THEN viewStatus := ViewNormal END;
    RETURN
  END;
  (* Block menus during death — any key skips to brother switch *)
  IF (actors[0].state = StDead) OR (actors[0].state = StDying) THEN
    IF input.attack OR (input.menuKey # 0C) OR
       (input.dirKey # DirNone) THEN
      IF actors[0].state = StDead THEN
        deathTimer := 9999  (* force immediate brother switch *)
      END
    END
  ELSE
    mapToggled := input.toggleMap;
    IF input.menuKey = '0' THEN
    cheatGod := NOT cheatGod;
    IF cheatGod THEN ShowMessage("GOD MODE ON")
    ELSE ShowMessage("GOD MODE OFF") END
  ELSIF input.menuKey = '9' THEN
    cheatSpeed := NOT cheatSpeed;
    IF cheatSpeed THEN ShowMessage("SPEED MODE ON")
    ELSE ShowMessage("SPEED MODE OFF") END
  ELSIF input.menuKey = '8' THEN
    cheatKeys := NOT cheatKeys;
    IF cheatKeys THEN ShowMessage("NO KEYS MODE ON")
    ELSE ShowMessage("NO KEYS MODE OFF") END
  ELSIF input.menuKey = 'E' THEN
    HandleWorldPickup
  ELSIF input.menuKey # 0C THEN HandleMenuKey(input.menuKey) END;
    IF input.mouseClick THEN HandleMenuClick(input.mouseX, input.mouseY) END
  END;
  IF input.quit THEN running := FALSE; RETURN END;
  IF BAND(CARDINAL(menus[MGame].enabled[5]), 1) # 0 THEN
    IF msgTimer > 0 THEN DEC(msgTimer) END; RETURN
  END;
  UpdatePlayer;
  (* Cheat: god mode — keep vitality maxed *)
  IF cheatGod AND (actors[0].state # StDead) AND (actors[0].state # StDying) THEN
    actors[0].vitality := 15 + brothers[activeBrother].brave DIV 4
  END;
  (* Cheat: speed mode — move 4 extra times per frame *)
  IF cheatSpeed AND (actors[0].state = StWalking) THEN
    IF MoveActor(0, actors[0].facing, 1) THEN END;
    IF MoveActor(0, actors[0].facing, 1) THEN END;
    IF MoveActor(0, actors[0].facing, 1) THEN END;
    IF MoveActor(0, actors[0].facing, 1) THEN END
  END;
  CheckEnvironment;
  IF freezeTimer = 0 THEN
    UpdateEnemies;
    IF (actors[0].state # StDead) AND (actors[0].state # StDying) THEN
      UpdateCombat;
      IF sanctuaryTimer = 0 THEN
        UpdateEncounters(actors[0].absX, actors[0].absY, currentRegion)
      END
    END;
    UpdateMissiles
  END;
  UpdateCarriers;
  (* Swan dismount messages — Carrier can't import Narration. *)
  IF dismountResult = 1 THEN Event(33)
  ELSIF dismountResult = 2 THEN Event(32)
  END;
  dismountResult := 0;
  UpdateDragon;
  IF dragonFire THEN
    FireMissile(3);  (* CarrierSlot = 3 *)
    dragonFire := FALSE
  END;
  CheckRescue(actors[0].absX, actors[0].absY);
  IF CheckWinCondition() THEN ShowWinScreen; running := FALSE END;
  CheckDoors;
  UpdateCamera(actors[0].absX, actors[0].absY);
  prevRegion := currentRegion;
  CheckRegionSwitch(camX, camY);
  IF currentRegion # prevRegion THEN
    DistributeRegion(currentRegion)
  END;
  UpdatePlace(actors[0].absX, actors[0].absY, currentRegion);
  MaterializeNPCs(actors[0].absX, actors[0].absY, currentRegion);
  UpdateTownNPCs(actors[0].absX, actors[0].absY, currentRegion);
  UpdateWitch;
  UpdateDayNight;
  UpdateSleep;
  CheckBedTile;
  UpdateFatigue;

  (* Spectre visibility: only at night (lightlevel < 40).
     Original: ob_listg[5].ob_stat = 3 if dark, 2 if light *)
  IF lightlevel < 40 THEN
    IF (objCount > 2) AND (objects[2].objId = 10) THEN
      objects[2].status := 3
    END
  ELSE
    IF (objCount > 2) AND (objects[2].objId = 10) THEN
      objects[2].status := 2
    END
  END;

  IF dayNight DIV 2000 # dayPeriod THEN
    dayPeriod := dayNight DIV 2000;
    CASE dayPeriod OF
      0: Event(28) | 4: Event(29) | 6: Event(30) | 9: Event(31) ELSE END
  END;

  IF (actors[0].state # StDead) AND (actors[0].state # StDying) THEN
    prevBattle := battleFlag;  (* save BEFORE updating — like original battle2 *)
    battleFlag := EnemiesNearby(actors[0].absX, actors[0].absY);
    IF battleFlag AND (NOT prevBattle) THEN aftermathDone := FALSE END;
    (* Battle music START — immediate, but NOT in astral plane.
       Original: setmood checks astral coordinates before battleflag. *)
    IF battleFlag AND (NOT prevBattle) THEN
      IF NOT ((actors[0].absX > 9216) AND (actors[0].absX < 12544) AND
              (actors[0].absY > 33280) AND (actors[0].absY < 35328)) THEN
        SetMood(MoodBattle)
      END
    END;
    (* Battle END — aftermath *)
    IF (NOT battleFlag) AND prevBattle AND (NOT aftermathDone) THEN
      BattleAftermath;
      aftermathDone := TRUE
    END
  END;
  (* Normal music resume — only every 8 frames, queued not immediate *)
  IF MusicTickDue() AND
     (actors[0].state # StDead) AND (actors[0].state # StDying) THEN
    (* Original setmood order (fmain.c:2938-2949):
       1. Dead → death music
       2. Astral coordinates → MoodSpec (BEFORE battleflag!)
       3. Battleflag → battle music
       4. Region > 7 → indoor/cave
       5. Day/night *)
    IF (actors[0].absX > 9216) AND (actors[0].absX < 12544) AND
       (actors[0].absY > 33280) AND (actors[0].absY < 35328) THEN
      SetMood(MoodSpec)
    ELSIF battleFlag THEN
      SetMood(MoodBattle)
    ELSIF currentRegion >= 8 THEN
      SetCaveWave(currentRegion = 9);
      SetMood(MoodIndoor)
    ELSIF lightlevel > 120 THEN SetMood(MoodDay)
    ELSE SetMood(MoodNight) END
  END;

  IF currentRegion >= 8 THEN brightness := 100; isNight := FALSE
  ELSIF lightTimer > 0 THEN brightness := 100; isNight := FALSE
  END;
  SaveBrotherState;
  SetStats(brothers[activeBrother].brave,
           brothers[activeBrother].luck,
           brothers[activeBrother].kind,
           brothers[activeBrother].wealth,
           actors[0].vitality);
  (* Magic timers *)
  IF lightTimer > 0 THEN DEC(lightTimer) END;
  IF secretTimer > 0 THEN DEC(secretTimer) END;
  IF freezeTimer > 0 THEN DEC(freezeTimer) END;
  IF wardTimer > 0 THEN DEC(wardTimer) END;
  IF sanctuaryTimer > 0 THEN DEC(sanctuaryTimer) END;
  revealHidden := (secretTimer > 0);

  IF msgTimer > 0 THEN DEC(msgTimer) END;
  INC(cycle);
  IF freezeTimer = 0 THEN INC(dayNight) END  (* freeze stops time *)
END UpdateGame;

END GameState.
