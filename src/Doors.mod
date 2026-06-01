IMPLEMENTATION MODULE Doors;

FROM InOut IMPORT WriteString, WriteInt, WriteLn;
FROM Assets IMPORT GetSectorByte, SetSectorByte, GetTerrainAt, regions,
                   currentRegion;
FROM Brothers IMPORT brothers, activeBrother;
FROM HudLog IMPORT AddLogLine;
FROM Platform IMPORT cheatKeys;

CONST
  CAVE  = 18;
  STAIR = 15;

TYPE
  DoorRec = RECORD
    xc1, yc1: INTEGER;
    xc2, yc2: INTEGER;
    dtype:    INTEGER;
    secs:     INTEGER
  END;

VAR
  doors: ARRAY [0..85] OF DoorRec;

PROCEDURE SetDoor(i, x1, y1, x2, y2, dt, sc: INTEGER);
BEGIN
  doors[i].xc1 := x1;  doors[i].yc1 := y1;
  doors[i].xc2 := x2;  doors[i].yc2 := y2;
  doors[i].dtype := dt; doors[i].secs := sc
END SetDoor;

PROCEDURE InitDoors;
BEGIN
  SetDoor( 0,  4464, 20576, 10352, 35680,  1, 1);
  SetDoor( 1,  4464, 20576, 10352, 35680,  1, 1);
  SetDoor( 2,  4464, 20576, 10352, 35680,  1, 1);
  SetDoor( 3,  4464, 20576, 10352, 35680,  1, 1);
  SetDoor( 4,  5008,  7008,  6528, 35936, 18, 2);
  SetDoor( 5,  6000, 27296,  8816, 38560,  9, 1);
  SetDoor( 6,  6512, 25248,  8048, 38560,  9, 1);
  SetDoor( 7,  6816, 19360,  5024, 38304, 17, 1);
  SetDoor( 8,  6816, 19552,  5024, 38752, 17, 1);
  SetDoor( 9,  6944, 19296,  5920, 38240, 17, 1);
  SetDoor(10,  7040, 19328,  5504, 38272, 17, 1);
  SetDoor(11,  7040, 19520,  5504, 38720, 17, 1);
  SetDoor(12,  7792, 15200, 10368, 40032,  3, 1);
  SetDoor(13,  9344, 13216, 11904, 36256,  1, 1);
  SetDoor(14, 10592, 34656, 11008, 37568, 15, 1);
  SetDoor(15, 11008, 37568, 10592, 34688, 15, 2);
  SetDoor(16, 11264, 29024, 10992, 37728,  9, 1);
  SetDoor(17, 12144, 11872, 12672, 39520,  3, 1);
  SetDoor(18, 12144, 25504,  7280, 38560,  9, 1);
  SetDoor(19, 12672, 14528, 10112, 39104,  1, 1);
  SetDoor(20, 13424, 19296,  1136, 36576, 15, 2);
  SetDoor(21, 15840,  7104, 12000, 37824,  7, 1);
  SetDoor(22, 15872,  7104, 12032, 37824,  7, 1);
  SetDoor(23, 17008,  9568, 11904, 39520,  3, 1);
  SetDoor(24, 17024, 15296, 10624, 39104,  1, 1);
  SetDoor(25, 17888, 21376,  9680, 38528, 10, 1);
  SetDoor(26, 18304, 12224,  9600, 39104,  1, 1);
  SetDoor(27, 18528, 26176,  7264, 39488, 18, 1);
  SetDoor(28, 18576, 26272,  7312, 39584, 11, 1);
  SetDoor(29, 18784, 23360,  8800, 39488, 18, 1);
  SetDoor(30, 18832, 23456,  8848, 39584, 11, 1);
  SetDoor(31, 18848, 15552,  2976, 33472,  2, 1);
  SetDoor(32, 18896, 15808,  3024, 33984,  2, 1);
  SetDoor(33, 18896, 15872,  3024, 34048,  2, 1);
  SetDoor(34, 18960, 15488,  3344, 33408,  1, 1);
  SetDoor(35, 18960, 15680,  3856, 33600,  1, 1);
  SetDoor(36, 18992, 15808,  3632, 34240,  1, 1);
  SetDoor(37, 19040, 16000,  4192, 34176,  1, 1);
  SetDoor(38, 19056, 15488,  4976, 33408,  1, 1);
  SetDoor(39, 19072, 15680,  4496, 33600,  1, 1);
  SetDoor(40, 19568, 12896,  9600, 40032,  3, 1);
  SetDoor(41, 19808, 21568,  8032, 40000, 18, 1);
  SetDoor(42, 19856, 17280, 12416, 36224, 13, 1);
  SetDoor(43, 19856, 21664,  8080, 40096, 11, 1);
  SetDoor(44, 19936, 27520, 10704, 38528, 10, 1);
  SetDoor(45, 21344, 22592,  8800, 38976, 18, 1);
  SetDoor(46, 21392, 22688,  8848, 39072, 11, 1);
  SetDoor(47, 21600, 17728,  7264, 38976, 18, 1);
  SetDoor(48, 21616, 25728, 11392, 36224,  3, 1);
  SetDoor(49, 21648, 17824,  7312, 39072, 11, 1);
  SetDoor(50, 22000, 21216,  5856, 33760, 10, 1);
  SetDoor(51, 22208, 21440,  7104, 33984, 13, 1);
  SetDoor(52, 22208, 21568,  6592, 34112, 13, 1);
  SetDoor(53, 22256, 20896,  6640, 33440, 13, 1);
  SetDoor(54, 22272, 21056,  7664, 33600, 14, 1);
  SetDoor(55, 22288, 21568,  7184, 34368, 13, 1);
  SetDoor(56, 22320, 21248,  6736, 33792, 13, 1);
  SetDoor(57, 22320, 21376,  7216, 33920, 14, 1);
  SetDoor(58, 22352, 20896,  7264, 33440, 13, 1);
  SetDoor(59, 22352, 21088,  8272, 33632, 13, 1);
  SetDoor(60, 22368, 21440,  8288, 33984, 13, 1);
  SetDoor(61, 22368, 21568,  7776, 34112, 13, 1);
  SetDoor(62, 22624, 23872,  7264, 39488, 18, 1);
  SetDoor(63, 22672, 23968,  7312, 40096, 11, 1);
  SetDoor(64, 22720, 11872,  2752, 34912, 18, 2);
  SetDoor(65, 22880, 28480,  8800, 39488, 18, 1);
  SetDoor(66, 22928, 28576,  8848, 40096, 11, 1);
  SetDoor(67, 22944, 26464, 10912, 35680, 15, 1);
  SetDoor(68, 23008, 22656, 10192, 38528, 10, 1);
  SetDoor(69, 24176,  6752,  9600, 39520,  3, 1);
  SetDoor(70, 24256, 10592,  4544, 35680, 18, 2);
  SetDoor(71, 24672, 29248,  6496, 40000, 18, 1);
  SetDoor(72, 24720, 29344,  6544, 40096, 11, 1);
  SetDoor(73, 24816, 12992,  9712, 35776,  3, 1);
  SetDoor(74, 25792,  6240,   960, 34400, 18, 2);
  SetDoor(75, 25952, 23872,  8032, 39488, 18, 1);
  SetDoor(76, 26000, 23968,  8080, 39072, 11, 1);
  SetDoor(77, 26048,  6688,  1200, 34880,  9, 2);
  SetDoor(78, 26224, 10848, 11136, 39520,  3, 1);
  SetDoor(79, 26624,  7008, 10992, 36960,  9, 1);
  SetDoor(80, 27472, 17280, 10320, 36224, 13, 1);
  SetDoor(81, 27616, 31872, 11216, 38528, 10, 1);
  SetDoor(82, 27760, 11872, 10368, 39520,  3, 1);
  SetDoor(83, 28000, 26688,  8032, 39488, 18, 1);
  SetDoor(84, 28048, 26784,  8080, 39584, 11, 1);
  SetDoor(85, 28384, 21120, 12752, 38528, 10, 1)
END InitDoors;

PROCEDURE CheckDoor(heroX, heroY, regionNum: INTEGER;
                    VAR newX, newY, newRegion: INTEGER): BOOLEAN;
VAR i, k, j, xtest, ytest, dt: INTEGER;
    d: DoorRec;
BEGIN
  xtest := (heroX DIV 16) * 16;
  ytest := (heroY DIV 32) * 32;

  IF regionNum < 8 THEN
    (* Outdoor to Indoor: binary search by xc1 *)
    i := 0;
    k := DoorCount - 1;
    WHILE k >= i DO
      j := (k + i) DIV 2;
      d := doors[j];
      IF d.xc1 > xtest THEN
        k := j - 1
      ELSIF d.xc1 + 16 < xtest THEN
        i := j + 1
      ELSIF (d.xc1 < xtest) AND ((d.dtype MOD 2) = 0) THEN
        i := j + 1
      ELSIF d.yc1 > ytest THEN
        k := j - 1
      ELSIF d.yc1 < ytest THEN
        i := j + 1
      ELSE
        dt := d.dtype;
        (* Direction check from original fmain.c:2532-2538:
           Horizontal door (type & 1): only enter when hero_y bit 4 = 0
           Vertical door: only enter when (hero_x & 15) <= 6 *)
        IF (dt MOD 2) = 1 THEN
          IF BAND(CARDINAL(heroY), 16) # 0 THEN
            RETURN FALSE
          END
        ELSIF (heroX MOD 16) > 6 THEN
          RETURN FALSE
        END;

        IF dt = CAVE THEN
          newX := d.xc2 + 24;
          newY := d.yc2 + 16
        ELSIF (dt MOD 2) = 1 THEN
          newX := d.xc2 + 16;
          newY := d.yc2
        ELSE
          newX := d.xc2 - 1;
          newY := d.yc2 + 16
        END;
        IF d.secs = 1 THEN
          newRegion := 8
        ELSE
          newRegion := 9
        END;
        WriteString("Door: entering region ");
        WriteInt(newRegion, 1);
        WriteString(" via door "); WriteInt(j, 1);
        WriteString(" at "); WriteInt(d.xc1, 1);
        WriteString(","); WriteInt(d.yc1, 1);
        WriteString(" -> "); WriteInt(newX, 1);
        WriteString(","); WriteInt(newY, 1); WriteLn;
        RETURN TRUE
      END;
      IF (i >= DoorCount) OR (k < 0) THEN
        RETURN FALSE
      END
    END
  ELSE
    (* Indoor to Outdoor: linear search by xc2/yc2 (indoor position).
       Original fmain.c:2572-2609. Includes direction check:
       horizontal (type&1): reject if (hero_y & 16) == 0
       vertical: reject if (hero_x & 15) < 2 *)
    FOR j := 0 TO DoorCount - 1 DO
      d := doors[j];
      IF (d.yc2 = ytest) AND
         ((d.xc2 = xtest) OR
          ((d.xc2 = xtest - 16) AND ((d.dtype MOD 2) = 1))) THEN
        dt := d.dtype;
        (* Direction check — must approach from correct side to exit *)
        IF (dt MOD 2) = 1 THEN
          IF BAND(CARDINAL(heroY), 16) = 0 THEN
            (* wrong side — skip *)
          ELSE
            IF dt = CAVE THEN
              newX := d.xc1 - 4; newY := d.yc1 + 16
            ELSE
              newX := d.xc1 + 16; newY := d.yc1 + 34
            END;
            newRegion := -1;
            RETURN TRUE
          END
        ELSE
          IF (heroX MOD 16) < 2 THEN
            (* wrong side *)
          ELSE
            IF dt = CAVE THEN
              newX := d.xc1 - 4; newY := d.yc1 + 16
            ELSE
              newX := d.xc1 + 20; newY := d.yc1 + 16
            END;
            newRegion := -1;
            RETURN TRUE
          END
        END
      END
    END
  END;

  RETURN FALSE
END CheckDoor;

(* --- Door tile opening ---
   When player steps on a door tile (terrain 15), change the map sector
   byte from the closed door tile to an open variant.
   Data from original open_list[17]. *)

CONST
  OpenCount = 17;
  MaxUnlockedDoors = 128;

TYPE
  OpenEntry = RECORD
    doorId:  INTEGER;  (* closed sector byte *)
    mapId:   INTEGER;  (* image bank to match *)
    new1:    INTEGER;  (* open tile primary *)
    new2:    INTEGER;  (* open tile secondary *)
    above:   INTEGER;  (* direction: 0=none, 1=above, 2=side, 3=back, 4=cabinet *)
    keyType: INTEGER   (* 0=none, required key type *)
  END;

  UnlockedDoor = RECORD
    px, py: INTEGER;
    region: INTEGER
  END;

VAR
  openList: ARRAY [0..16] OF OpenEntry;
  bumped: BOOLEAN;
  unlockedDoors: ARRAY [0..MaxUnlockedDoors - 1] OF UnlockedDoor;
  unlockedCount: INTEGER;

PROCEDURE IsUnlocked(px, py, region: INTEGER): BOOLEAN;
VAR i: INTEGER;
BEGIN
  FOR i := 0 TO unlockedCount - 1 DO
    IF (unlockedDoors[i].px = px) AND
       (unlockedDoors[i].py = py) AND
       (unlockedDoors[i].region = region) THEN
      RETURN TRUE
    END
  END;
  RETURN FALSE
END IsUnlocked;

PROCEDURE RememberUnlocked(px, py, region: INTEGER);
BEGIN
  IF IsUnlocked(px, py, region) THEN RETURN END;
  IF unlockedCount >= MaxUnlockedDoors THEN RETURN END;
  unlockedDoors[unlockedCount].px := px;
  unlockedDoors[unlockedCount].py := py;
  unlockedDoors[unlockedCount].region := region;
  INC(unlockedCount)
END RememberUnlocked;

PROCEDURE GetUnlockedCount(): INTEGER;
BEGIN
  RETURN unlockedCount
END GetUnlockedCount;

PROCEDURE GetUnlockedDoor(i: INTEGER; VAR px, py, region: INTEGER);
BEGIN
  IF (i < 0) OR (i >= unlockedCount) THEN
    px := 0; py := 0; region := -1;
    RETURN
  END;
  px := unlockedDoors[i].px;
  py := unlockedDoors[i].py;
  region := unlockedDoors[i].region
END GetUnlockedDoor;

PROCEDURE ClearUnlockedDoors;
BEGIN
  unlockedCount := 0
END ClearUnlockedDoors;

PROCEDURE AddUnlockedDoor(px, py, region: INTEGER);
BEGIN
  IF (region < 0) OR (region > 9) THEN RETURN END;
  RememberUnlocked(px, py, region)
END AddUnlockedDoor;

PROCEDURE InitOpenList;
  PROCEDURE O(i, did, mid, n1, n2, ab, kt: INTEGER);
  BEGIN
    openList[i].doorId := did;
    openList[i].mapId := mid;
    openList[i].new1 := n1;
    openList[i].new2 := n2;
    openList[i].above := ab;
    openList[i].keyType := kt
  END O;
BEGIN
  (* Key types: 0=none, 1=Gold, 2=Green, 3=Blue, 4=Red, 5=Grey, 6=White *)
  O( 0,  64, 360, 123, 124, 2, 2);  (* HSTONE — Green key *)
  O( 1, 120, 360, 125, 126, 2, 0);  (* HWOOD — no key *)
  O( 2, 122, 360, 127,   0, 0, 0);  (* VWOOD — no key *)
  O( 3,  64, 280, 124, 125, 2, 5);  (* HSTONE2 — Grey key *)
  O( 4,  77, 280, 126,   0, 0, 5);  (* VSTONE2 — Grey key *)
  O( 5,  82, 480,  84,  85, 2, 3);  (* CRYST — Blue key *)
  O( 6,  64, 480, 105, 106, 2, 2);  (* OASIS — Green key *)
  O( 7, 128, 240, 154, 155, 1, 6);  (* MARBLE — White key *)
  O( 8,  39, 680,  41,  42, 2, 1);  (* HGATE — Gold key *)
  O( 9,  25, 680,  27,  26, 3, 1);  (* VGATE — Gold key *)
  O(10, 114, 760, 116, 117, 1, 4);  (* SECRET — Red key *)
  O(11, 118, 760, 116, 117, 1, 5);  (* TUNNEL — Grey key *)
  O(12, 136, 800, 133, 134, 135, 1);  (* GOLDEN — Gold key *)
  O(13, 187, 800,  76,  77, 2, 0);  (* HSTON3 — no key *)
  O(14,  73, 720,  75,   0, 0, 0);  (* VSTON3 — no key *)
  O(15, 165, 800,  85,  86, 4, 2);  (* CABINET — Green key *)
  O(16, 210, 840, 208, 209, 2, 0)   (* BLUE — no key *)
END InitOpenList;

TYPE
  SavedTile = RECORD
    px, py: INTEGER;
    oldVal: INTEGER
  END;

VAR
  savedTiles: ARRAY [0..7] OF SavedTile;
  savedCount: INTEGER;

PROCEDURE SaveAndSet(px, py, newVal: INTEGER);
BEGIN
  IF savedCount < 8 THEN
    savedTiles[savedCount].px := px;
    savedTiles[savedCount].py := py;
    savedTiles[savedCount].oldVal := GetSectorByte(px, py);
    INC(savedCount)
  END;
  SetSectorByte(px, py, newVal)
END SaveAndSet;

PROCEDURE RestoreDoorTiles;
VAR i: INTEGER;
BEGIN
  FOR i := 0 TO savedCount - 1 DO
    SetSectorByte(savedTiles[i].px, savedTiles[i].py, savedTiles[i].oldVal)
  END;
  savedCount := 0;
  bumped := FALSE
END RestoreDoorTiles;

PROCEDURE OpenDoorTile(heroX, heroY: INTEGER): BOOLEAN;
VAR x, y, secId, regId, j, k: INTEGER;
    l: INTEGER;
BEGIN
  (* Find the door tile — scan nearby positions like the original *)
  x := heroX;
  y := heroY;
  IF GetTerrainAt(x, y) # 15 THEN
    x := heroX + 4;
    IF GetTerrainAt(x, y) # 15 THEN
      x := heroX - 4;
      IF GetTerrainAt(x, y) # 15 THEN
        RETURN FALSE
      END
    END
  END;

  (* Align to door start — walk left and down while still on door tiles.
     Original doorfind: finds the leftmost/bottommost tile of the door. *)
  IF GetTerrainAt(x - 16, y) = 15 THEN DEC(x, 16) END;
  IF GetTerrainAt(x - 16, y) = 15 THEN DEC(x, 16) END;
  IF GetTerrainAt(x, y + 32) = 15 THEN INC(y, 32) END;

  (* Convert to image coordinates (imx, imy) like original's x>>=4, y>>=5 *)
  x := x DIV 16;
  y := y DIV 32;

  (* Read sector tile byte at door position — same as original mapxy *)
  secId := GetSectorByte(x * 16, y * 32);

  (* Get the region image bank: top 2 bits of sector byte select bank *)
  IF (currentRegion >= 0) AND (currentRegion <= 9) THEN
    regId := regions[currentRegion].image[secId DIV 64]
  ELSE
    RETURN FALSE
  END;

  (* Search open_list for matching door *)
  FOR j := 0 TO OpenCount - 1 DO
    IF (openList[j].mapId = regId) AND (openList[j].doorId = secId) THEN
      k := openList[j].keyType;
      (* Keyed doors: block movement, show "locked" message.
         Player must use the correct key from KEYS menu.
         Cheat mode bypasses all locks. *)
      IF (k > 0) AND (NOT cheatKeys) AND
         (NOT IsUnlocked(x * 16, y * 32, currentRegion)) THEN
        IF NOT bumped THEN
          AddLogLine("It's locked.");
          bumped := TRUE
        END;
        RETURN FALSE
      END;
      (* No-key doors: use SaveAndSet (temporary open) *)
      savedCount := 0;
      SaveAndSet(x * 16, y * 32, openList[j].new1);
      k := openList[j].new2;
      IF k > 0 THEN
        l := openList[j].above;
        IF l = 1 THEN
          SaveAndSet(x * 16, (y - 1) * 32, k)
        ELSIF l = 3 THEN
          SaveAndSet((x - 1) * 16, y * 32, k)
        ELSIF l = 4 THEN
          SaveAndSet(x * 16, (y - 1) * 32, 87);
          SaveAndSet((x + 1) * 16, y * 32, 86);
          SaveAndSet((x + 1) * 16, (y - 1) * 32, 88)
        ELSE
          SaveAndSet((x + 1) * 16, y * 32, k);
          IF l # 2 THEN
            SaveAndSet((x + 2) * 16, y * 32, openList[j].above)
          END
        END
      END;
      RETURN TRUE
    END
  END;
  RETURN FALSE
END OpenDoorTile;

PROCEDURE CheckCloseDoors(heroX, heroY: INTEGER);
VAR dx, dy: INTEGER;
BEGIN
  IF savedCount = 0 THEN RETURN END;
  dx := heroX - savedTiles[0].px;
  dy := heroY - savedTiles[0].py;
  IF dx < 0 THEN dx := -dx END;
  IF dy < 0 THEN dy := -dy END;
  IF (dx > 64) OR (dy > 64) THEN
    RestoreDoorTiles
  END
END CheckCloseDoors;

(* Try to open a nearby door with a specific key type.
   Searches hero position + 8 directions at 16px offset.
   Returns TRUE if a door was opened. *)
PROCEDURE UseKeyOnDoor(heroX, heroY, keyType: INTEGER): BOOLEAN;
CONST
  Step = 16;
VAR
  dir, tx, ty, x, y, secId, regId, j, k, l: INTEGER;
  xd: ARRAY [0..8] OF INTEGER;
  yd: ARRAY [0..8] OF INTEGER;
BEGIN
  xd[0] := 0;    yd[0] := 0;
  xd[1] := 0;    yd[1] := -Step;
  xd[2] := Step; yd[2] := -Step;
  xd[3] := Step; yd[3] := 0;
  xd[4] := Step; yd[4] := Step;
  xd[5] := 0;    yd[5] := Step;
  xd[6] := -Step; yd[6] := Step;
  xd[7] := -Step; yd[7] := 0;
  xd[8] := -Step; yd[8] := -Step;

  FOR dir := 0 TO 8 DO
    tx := heroX + xd[dir];
    ty := heroY + yd[dir];
    IF GetTerrainAt(tx, ty) = 15 THEN
      x := tx; y := ty;
      (* Align to door origin *)
      IF GetTerrainAt(x - 16, y) = 15 THEN DEC(x, 16) END;
      IF GetTerrainAt(x - 16, y) = 15 THEN DEC(x, 16) END;
      IF GetTerrainAt(x, y + 32) = 15 THEN INC(y, 32) END;
      x := x DIV 16; y := y DIV 32;
      secId := GetSectorByte(x * 16, y * 32);
      IF (currentRegion >= 0) AND (currentRegion <= 9) THEN
        regId := regions[currentRegion].image[secId DIV 64];
        FOR j := 0 TO OpenCount - 1 DO
          IF (openList[j].mapId = regId) AND
             (openList[j].doorId = secId) AND
             (openList[j].keyType = keyType) THEN
            (* Match — open permanently (no SaveAndSet, direct write) *)
            RememberUnlocked(x * 16, y * 32, currentRegion);
            SetSectorByte(x * 16, y * 32, openList[j].new1);
            k := openList[j].new2;
            IF k > 0 THEN
              l := openList[j].above;
              IF l = 1 THEN
                SetSectorByte(x * 16, (y - 1) * 32, k)
              ELSIF l = 3 THEN
                SetSectorByte((x - 1) * 16, y * 32, k)
              ELSIF l = 4 THEN
                SetSectorByte(x * 16, (y - 1) * 32, 87);
                SetSectorByte((x + 1) * 16, y * 32, 86);
                SetSectorByte((x + 1) * 16, (y - 1) * 32, 88)
              ELSE
                SetSectorByte((x + 1) * 16, y * 32, k);
                IF l # 2 THEN
                  SetSectorByte((x + 2) * 16, y * 32, openList[j].above)
                END
              END
            END;
            AddLogLine("It opened.");
            bumped := FALSE;
            RETURN TRUE
          END
        END
      END
    END
  END;
  RETURN FALSE
END UseKeyOnDoor;

BEGIN
  savedCount := 0;
  unlockedCount := 0;
  bumped := FALSE;
  InitOpenList
END Doors.
