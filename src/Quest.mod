IMPLEMENTATION MODULE Quest;

(* Quest progression matching original FTA. *)

FROM Strings IMPORT Assign, Concat;
FROM Actor IMPORT actors, actorCount;
FROM Brothers IMPORT brothers, activeBrother, AddWealth, LastStuff;
FROM Assets IMPORT currentRegion, SwitchRegion, DetectRegion, GetTerrainAt;
FROM Narration IMPORT InitPlace;
FROM NPC IMPORT ResetMaterialized;
FROM WorldObj IMPORT objects, objCount, MaxWorldObjs,
                     IsRegionDistributed, SetRegionDistributed;
FROM Doors IMPORT GetUnlockedCount, GetUnlockedDoor,
                  ClearUnlockedDoors, AddUnlockedDoor;
FROM HudLog IMPORT AddLogLine;
FROM BinaryIO IMPORT OpenRead, OpenWrite, Close, ReadBytes, WriteBytes,
                     ReadByte, WriteByte, Done;
FROM Platform IMPORT ren, ScreenW, PlayH, TextH, Scale,
                    LoadBMPTexture, BeginFrame, EndFrame, DelayMs,
                    PollInput, InputState, DirNone;
FROM Texture IMPORT Tex, DrawRegion AS TexDrawRegion;
FROM Canvas IMPORT SetColor, Clear;
FROM Music IMPORT StopMusic;
FROM HudFont IMPORT DrawScreenStr, ScreenStrWidth, SetFontColor, ResetFontColor;
FROM InOut IMPORT WriteString, WriteInt, WriteLn;

VAR
  princessRescued: BOOLEAN;
  gameWon: BOOLEAN;

(* --- Princess Rescue --- *)

PROCEDURE CheckRescue(heroX, heroY: INTEGER);
VAR i: INTEGER;
BEGIN
  IF princessRescued THEN RETURN END;
  IF currentRegion # 8 THEN RETURN END;

  IF (heroX > 10820) AND (heroX < 10877) AND
     (heroY > 35646) AND (heroY < 35670) THEN
    princessRescued := TRUE;
    brothers[activeBrother].stuff[28] := 1;  (* Writ *)
    AddWealth(100);
    FOR i := 16 TO 21 DO
      INC(brothers[activeBrother].stuff[i], 3)
    END;
    AddLogLine("You have rescued the princess!");
    AddLogLine("The king gave you a writ and 100 gold.");
    WriteString("Quest: princess rescued!"); WriteLn
  END
END CheckRescue;

(* --- Win Condition --- *)

PROCEDURE CheckWinCondition(): BOOLEAN;
BEGIN
  IF brothers[activeBrother].stuff[22] > 0 THEN
    IF NOT gameWon THEN
      gameWon := TRUE;
      AddLogLine("You have recovered the Talisman!");
      AddLogLine("The quest is complete!");
      WriteString("Quest: TALISMAN RECOVERED — YOU WIN!"); WriteLn
    END;
    RETURN TRUE
  END;
  RETURN FALSE
END CheckWinCondition;

(* --- Save/Load Game --- *)

PROCEDURE MakePath(slot: INTEGER; VAR path: ARRAY OF CHAR);
BEGIN
  CASE slot OF
    0: Assign("A", path) |
    1: Assign("B", path) |
    2: Assign("C", path) |
    3: Assign("D", path) |
    4: Assign("E", path) |
    5: Assign("F", path) |
    6: Assign("G", path) |
    7: Assign("H", path)
  ELSE
    Assign("X", path)
  END
END MakePath;

PROCEDURE MakeLegacyPath(slot: INTEGER; VAR path: ARRAY OF CHAR);
BEGIN
  Assign("saves/save_", path);
  CASE slot OF
    0: Concat(path, "A.dat", path) |
    1: Concat(path, "B.dat", path) |
    2: Concat(path, "C.dat", path) |
    3: Concat(path, "D.dat", path) |
    4: Concat(path, "E.dat", path) |
    5: Concat(path, "F.dat", path) |
    6: Concat(path, "G.dat", path) |
    7: Concat(path, "H.dat", path)
  ELSE
    Concat(path, "X.dat", path)
  END
END MakeLegacyPath;

(* Serialize/deserialize integers as 4 little-endian bytes.
   Avoids ADR() which the C backend rejects. *)
PROCEDURE WriteInt4(fd: CARDINAL; val: INTEGER);
VAR buf: ARRAY [0..3] OF CHAR;
BEGIN
  buf[0] := CHR(BAND(CARDINAL(val), 255));
  buf[1] := CHR(BAND(CARDINAL(val DIV 256), 255));
  buf[2] := CHR(BAND(CARDINAL(val DIV 65536), 255));
  buf[3] := CHR(BAND(CARDINAL(val DIV 16777216), 255));
  WriteBytes(fd, buf, 4)
END WriteInt4;

PROCEDURE ReadInt4(fd: CARDINAL; VAR val: INTEGER);
VAR buf: ARRAY [0..3] OF CHAR;
    n: CARDINAL;
BEGIN
  ReadBytes(fd, buf, 4, n);
  IF n < 4 THEN val := 0; RETURN END;
  val := ORD(buf[0]) + ORD(buf[1]) * 256 +
         ORD(buf[2]) * 65536 + ORD(buf[3]) * 16777216
END ReadInt4;

PROCEDURE WriteBool4(fd: CARDINAL; b: BOOLEAN);
BEGIN
  IF b THEN WriteInt4(fd, 1) ELSE WriteInt4(fd, 0) END
END WriteBool4;

PROCEDURE ReadBool4(fd: CARDINAL; VAR b: BOOLEAN);
VAR v: INTEGER;
BEGIN
  ReadInt4(fd, v);
  b := (v # 0)
END ReadBool4;

PROCEDURE SaveGame(slot, savedDayNight, savedFatigue, savedHunger,
                   savedCycle, savedLightTimer, savedSecretTimer,
                   savedFreezeTimer: INTEGER): BOOLEAN;
VAR path: ARRAY [0..63] OF CHAR;
    fd: CARDINAL;
    i, k, px, py, region: INTEGER;
    buf: ARRAY [0..3] OF CHAR;
BEGIN
  MakePath(slot, path);
  WriteString("Save: writing to "); WriteString(path); WriteLn;
  OpenWrite(path, fd);
  IF fd = 0 THEN
    WriteString("Save: FAILED to open "); WriteString(path); WriteLn;
    AddLogLine("Save failed.");
    RETURN FALSE
  END;

  (* Header *)
  buf[0] := 'F'; buf[1] := 'T'; buf[2] := 'A'; buf[3] := '3';
  WriteBytes(fd, buf, 4);

  (* Active brother *)
  WriteInt4(fd, activeBrother);

  (* All 3 brothers' stats and inventory *)
  FOR i := 0 TO 2 DO
    WriteInt4(fd, brothers[i].vitality);
    WriteInt4(fd, brothers[i].weapon);
    WriteInt4(fd, brothers[i].brave);
    WriteInt4(fd, brothers[i].luck);
    WriteInt4(fd, brothers[i].kind);
    WriteInt4(fd, brothers[i].wealth);
    FOR k := 0 TO LastStuff DO WriteInt4(fd, brothers[i].stuff[k]) END;
    WriteBool4(fd, brothers[i].alive)
  END;

  (* Player position and state *)
  WriteInt4(fd, actors[0].absX);
  WriteInt4(fd, actors[0].absY);
  WriteInt4(fd, actors[0].weapon);
  WriteInt4(fd, actors[0].facing);

  (* Current region *)
  WriteInt4(fd, currentRegion);

  (* Princess state *)
  WriteBool4(fd, princessRescued);

  (* Doors previously unlocked with keys *)
  WriteInt4(fd, GetUnlockedCount());
  FOR i := 0 TO GetUnlockedCount() - 1 DO
    GetUnlockedDoor(i, px, py, region);
    WriteInt4(fd, px);
    WriteInt4(fd, py);
    WriteInt4(fd, region)
  END;

  (* Runtime state *)
  WriteInt4(fd, savedDayNight);
  WriteInt4(fd, savedFatigue);
  WriteInt4(fd, savedHunger);
  WriteInt4(fd, savedCycle);
  WriteInt4(fd, savedLightTimer);
  WriteInt4(fd, savedSecretTimer);
  WriteInt4(fd, savedFreezeTimer);
  WriteBool4(fd, gameWon);

  (* Exact world object state, including collected items and scattered apples. *)
  WriteInt4(fd, objCount);
  FOR i := 0 TO objCount - 1 DO
    WriteInt4(fd, objects[i].x);
    WriteInt4(fd, objects[i].y);
    WriteInt4(fd, objects[i].objId);
    WriteInt4(fd, objects[i].status);
    WriteInt4(fd, objects[i].region)
  END;
  FOR i := 0 TO 9 DO WriteBool4(fd, IsRegionDistributed(i)) END;

  Close(fd);
  AddLogLine("Game saved.");
  WriteString("Quest: saved to "); WriteString(path); WriteLn;
  RETURN TRUE
END SaveGame;

PROCEDURE LoadGame(slot: INTEGER;
                   VAR savedDayNight, savedFatigue, savedHunger,
                       savedCycle, savedLightTimer, savedSecretTimer,
                       savedFreezeTimer: INTEGER): BOOLEAN;
VAR path: ARRAY [0..63] OF CHAR;
    fd: CARDINAL;
    n, i, v, k, px, py, region, version: INTEGER;
    distributed: BOOLEAN;
    buf: ARRAY [0..3] OF CHAR;
BEGIN
  MakePath(slot, path);
  OpenRead(path, fd);
  IF fd = 0 THEN
    MakeLegacyPath(slot, path);
    OpenRead(path, fd);
    IF fd = 0 THEN
      AddLogLine("No save file found.");
      RETURN FALSE
    END
  END;

  (* Verify header *)
  ReadBytes(fd, buf, 4, n);
  IF (n < 4) OR (buf[0] # 'F') OR (buf[1] # 'T') OR
     (buf[2] # 'A') OR
     ((buf[3] # '1') AND (buf[3] # '2') AND (buf[3] # '3')) THEN
    AddLogLine("Invalid save file.");
    Close(fd);
    RETURN FALSE
  END;
  version := ORD(buf[3]) - ORD('0');

  (* Active brother *)
  ReadInt4(fd, v);
  activeBrother := v;
  IF activeBrother > 2 THEN activeBrother := 0 END;

  (* All 3 brothers *)
  FOR i := 0 TO 2 DO
    ReadInt4(fd, brothers[i].vitality);
    ReadInt4(fd, brothers[i].weapon);
    ReadInt4(fd, brothers[i].brave);
    ReadInt4(fd, brothers[i].luck);
    ReadInt4(fd, brothers[i].kind);
    ReadInt4(fd, brothers[i].wealth);
    IF version >= 3 THEN
      FOR k := 0 TO LastStuff DO ReadInt4(fd, brothers[i].stuff[k]) END
    ELSE
      FOR k := 0 TO 34 DO ReadInt4(fd, brothers[i].stuff[k]) END;
      FOR k := 35 TO LastStuff DO brothers[i].stuff[k] := 0 END
    END;
    ReadBool4(fd, brothers[i].alive)
  END;

  (* Player position *)
  ReadInt4(fd, actors[0].absX);
  ReadInt4(fd, actors[0].absY);
  ReadInt4(fd, actors[0].weapon);
  ReadInt4(fd, actors[0].facing);

  (* Current region *)
  ReadInt4(fd, v);
  IF (v >= 0) AND (v <= 9) THEN
    SwitchRegion(v)
  END;

  (* Princess state *)
  ReadBool4(fd, princessRescued);

  (* Optional appended state: old FTA1 saves end before this count. *)
  ClearUnlockedDoors;
  ReadInt4(fd, v);
  IF v < 0 THEN v := 0 END;
  IF v > 128 THEN v := 128 END;
  FOR i := 0 TO v - 1 DO
    ReadInt4(fd, px);
    ReadInt4(fd, py);
    ReadInt4(fd, region);
    AddUnlockedDoor(px, py, region)
  END;

  IF version >= 2 THEN
    ReadInt4(fd, savedDayNight);
    ReadInt4(fd, savedFatigue);
    ReadInt4(fd, savedHunger);
    ReadInt4(fd, savedCycle);
    ReadInt4(fd, savedLightTimer);
    ReadInt4(fd, savedSecretTimer);
    ReadInt4(fd, savedFreezeTimer);
    ReadBool4(fd, gameWon);

    ReadInt4(fd, v);
    IF v < 0 THEN v := 0 END;
    IF v > MaxWorldObjs THEN v := MaxWorldObjs END;
    objCount := v;
    FOR i := 0 TO objCount - 1 DO
      ReadInt4(fd, objects[i].x);
      ReadInt4(fd, objects[i].y);
      ReadInt4(fd, objects[i].objId);
      ReadInt4(fd, objects[i].status);
      ReadInt4(fd, objects[i].region)
    END;
    FOR i := 0 TO 9 DO
      ReadBool4(fd, distributed);
      SetRegionDistributed(i, distributed)
    END
  END;

  Close(fd);

  actors[0].state := 13;  (* StStill *)
  actors[0].vitality := brothers[activeBrother].vitality;
  actors[0].environ := 0;
  actorCount := 1;

  (* Reset NPC materialization flags — actors were wiped on load *)
  ResetMaterialized;
  (* Nudge out of blocked terrain if stuck — try small offsets.
     Terrain 1 = rock, >= 10 = walls/doors — both block movement. *)
  v := GetTerrainAt(actors[0].absX, actors[0].absY);
  IF (v = 1) OR (v >= 10) THEN
    FOR i := 1 TO 16 DO
      v := GetTerrainAt(actors[0].absX, actors[0].absY + i);
      IF (v # 1) AND (v < 10) THEN
        INC(actors[0].absY, i); i := 16
      ELSE
        v := GetTerrainAt(actors[0].absX, actors[0].absY - i);
        IF (v # 1) AND (v < 10) THEN
          DEC(actors[0].absY, i); i := 16
        ELSE
          v := GetTerrainAt(actors[0].absX + i, actors[0].absY);
          IF (v # 1) AND (v < 10) THEN
            INC(actors[0].absX, i); i := 16
          ELSE
            v := GetTerrainAt(actors[0].absX - i, actors[0].absY);
            IF (v # 1) AND (v < 10) THEN
              DEC(actors[0].absX, i); i := 16
            END
          END
        END
      END
    END
  END;
  InitPlace(actors[0].absX, actors[0].absY, currentRegion);

  AddLogLine("Game loaded.");
  WriteString("Quest: loaded from "); WriteString(path); WriteLn;
  RETURN TRUE
END LoadGame;

PROCEDURE CenterText(s: ARRAY OF CHAR; y, sc: INTEGER);
VAR w, x, sw: INTEGER;
BEGIN
  sw := ScreenW * Scale;
  w := ScreenStrWidth(s, sc);
  x := (sw - w) DIV 2;
  DrawScreenStr(ren, s, x, y, sc)
END CenterText;

PROCEDURE ShowWinScreen;
VAR winTex: Tex;
    p: ARRAY [0..127] OF CHAR;
    sw, sh, i: INTEGER;
    inp: InputState;
BEGIN
  sw := ScreenW * Scale;
  sh := (PlayH + TextH) * Scale;

  StopMusic;

  (* Show victory text placard first *)
  SetFontColor(204, 0, 0);  (* red text *)
  FOR i := 1 TO 300 DO
    PollInput(inp);
    BeginFrame;
    SetColor(ren, 0, 0, 0, 255);
    Clear(ren);
    CenterText("The Talisman has been recovered!", sh DIV 4, 2);
    CenterText("The village of Tambry is saved!", sh DIV 4 + 30, 2);
    CenterText("Congratulations!", sh * 2 DIV 4, 3);
    EndFrame;
    DelayMs(33);
    IF inp.attack OR (inp.dirKey # DirNone) THEN i := 300 END
  END;
  ResetFontColor;

  (* Load and show win picture *)
  Assign("assets/winpic.bmp", p);
  winTex := LoadBMPTexture(p);
  IF winTex # NIL THEN
    FOR i := 1 TO 300 DO
      PollInput(inp);
      BeginFrame;
      SetColor(ren, 0, 0, 0, 255);
      Clear(ren);
      TexDrawRegion(ren, winTex, 0, 0, 320, 200, 0, 0, sw, sh);
      EndFrame;
      DelayMs(33);
      IF inp.attack OR (inp.dirKey # DirNone) THEN i := 300 END
    END
  END
END ShowWinScreen;

BEGIN
  princessRescued := FALSE;
  gameWon := FALSE
END Quest.
