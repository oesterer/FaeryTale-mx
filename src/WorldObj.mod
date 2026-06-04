IMPLEMENTATION MODULE WorldObj;

FROM SYSTEM IMPORT ADDRESS;
FROM Platform IMPORT ren, Scale, PlayW, PlayH, DrawTexRegion,
                    LoadBMPKeyedTexture;
FROM Canvas IMPORT SetClip, ClearClip;
FROM World IMPORT camX, camY;
FROM Assets IMPORT currentRegion, AssetPath, GetTerrainAt;
FROM InOut IMPORT WriteString, WriteInt, WriteLn;

CONST
  ObjSprW = 16;
  ObjSprH = 16;
  ObjSheetH = 2000;

PROCEDURE S(v: INTEGER): INTEGER;
BEGIN RETURN v * Scale END S;

PROCEDURE AddObj(x, y, id, stat, reg: INTEGER);
BEGIN
  IF objCount >= MaxWorldObjs THEN RETURN END;
  objects[objCount].x := x;
  objects[objCount].y := y;
  objects[objCount].objId := id;
  objects[objCount].status := stat;
  objects[objCount].region := reg;
  INC(objCount)
END AddObj;

PROCEDURE InitWorldObjects;
VAR r: INTEGER;
BEGIN
  objCount := 0;
  objTex := NIL;
  rng := 31337;
  FOR r := 0 TO 9 DO distributed[r] := FALSE END;

  (* === Global objects (region -1) === *)
  AddObj(19316, 15747, 11, 0, -1);   (* ghost brother 1 *)
  AddObj(18196, 15735, 11, 0, -1);   (* ghost brother 2 *)
  AddObj(12439, 36202, 10, 3, -1);   (* spectre *)
  AddObj(11092, 38526, 149, 1, -1);  (* gold statue *)
  AddObj(25737, 10662, 149, 1, -1);  (* gold statue *)
  AddObj( 2910, 39023, 149, 1, -1);  (* gold statue *)
  AddObj(12025, 37639, 149, 0, -1);  (* gold statue *)
  AddObj( 6700, 33766, 149, 0, -1);  (* gold statue *)

  (* === Region 0 — Snow Land === *)
  AddObj( 3340,  6735, 12, 3, 0);
  AddObj( 9678,  7035, 12, 3, 0);
  AddObj( 4981,  6306, 12, 3, 0);

  (* === Region 1 — Maze Forest North === *)
  AddObj(23087,  5667, 102, 1, 1);   (* turtle eggs — visible, pickup refused *)

  (* === Region 2 — Swamp Land === *)
  AddObj(13668, 15000,  0, 3, 2);
  AddObj(10627, 13154,  0, 3, 2);
  AddObj( 4981, 10056, 12, 3, 2);
  AddObj(13950, 11087, 16, 1, 2);    (* sacks *)
  AddObj(10344, 36171, 151, 1, 2);   (* shell *)

  (* === Region 3 — Tambry / Manor / Maze South === *)
  AddObj(19298, 16128, 15, 1, 3);    (* chest *)
  AddObj(18310, 15969, 13, 3, 3);    (* beggar *)
  AddObj(20033, 14401,  0, 3, 3);    (* wizard *)
  AddObj(19386, 15750, 16, 3, 3);    (* herb merchant at Tambry entrance *)
  AddObj(19358, 15750, 17, 3, 3);    (* scroll priest *)
  AddObj(19414, 15750, 18, 3, 3);    (* apple ranger *)
  AddObj(18784, 15617, 19, 3, 3);    (* Brann Oakhand *)
  AddObj(18824, 15617, 20, 3, 3);    (* Mara Caskwell *)
  AddObj(18864, 15617, 21, 3, 3);    (* Orruk the Gentle *)
  AddObj(18904, 15617, 22, 3, 3);    (* Kett Redcap *)
  AddObj(18942, 15617, 23, 3, 3);    (* Nella Hearth *)
  AddObj(18784, 15681, 24, 3, 3);    (* Tovin Reed *)
  AddObj(18824, 15681, 25, 3, 3);    (* Borga Stoneback *)
  AddObj(18864, 15681, 26, 3, 3);    (* Elsa Vine *)
  AddObj(18904, 15681, 27, 3, 3);    (* Perrin Quill *)
  AddObj(18942, 15681, 28, 3, 3);    (* Grum Barley *)
  AddObj(18784, 15745, 29, 3, 3);    (* Lysa Doorward *)
  AddObj(18824, 15745, 30, 3, 3);    (* Hobb Flint *)
  AddObj(18864, 15745, 31, 3, 3);    (* Anka Blueglass *)
  AddObj(18904, 15745, 32, 3, 3);    (* Rusk Fen *)
  AddObj(18942, 15745, 33, 3, 3);    (* Maud Ash *)
  AddObj(24794, 13102, 13, 3, 3);    (* beggar *)
  AddObj(21626, 15446, 18, 1, 3);    (* blue stone *)
  AddObj(21616, 15456, 13, 1, 3);    (* money *)
  AddObj(21636, 15456, 17, 1, 3);    (* gold ring *)
  AddObj(20117, 14222, 19, 1, 3);    (* green jewel *)
  AddObj(24185,  9840, 16, 1, 3);    (* sacks *)
  AddObj(25769, 10617, 13, 1, 3);    (* money *)
  AddObj(25678, 10703, 18, 1, 3);    (* blue stone *)
  AddObj(17177, 10599, 20, 1, 3);    (* scrap *)
  AddObj(19026, 15750, 148, 1, 3);   (* starting fruit stash *)
  AddObj(19031, 15750, 148, 1, 3);
  AddObj(19036, 15750, 148, 1, 3);
  AddObj(19041, 15750, 148, 1, 3);
  AddObj(19046, 15750, 148, 1, 3);
  AddObj(19026, 15755, 148, 1, 3);
  AddObj(19031, 15755, 148, 1, 3);
  AddObj(19036, 15755, 148, 1, 3);
  AddObj(19041, 15755, 148, 1, 3);
  AddObj(19046, 15755, 148, 1, 3);
  (* Spell testing stash near the starting point. *)
  AddObj(18972, 15712, ObjWardScroll, 1, 3);
  AddObj(18988, 15712, ObjFreezeScroll, 1, 3);
  AddObj(19004, 15712, ObjFireScroll, 1, 3);
  AddObj(19020, 15712, ObjFearScroll, 1, 3);
  AddObj(19036, 15712, ObjLightScroll, 1, 3);
  AddObj(19052, 15712, ObjSanctuaryScroll, 1, 3);
  AddObj(19068, 15712, ObjHarvestScroll, 1, 3);
  AddObj(19084, 15712, ObjHealScroll, 1, 3);
  AddObj(18972, 15740, ObjMandrake, 1, 3);
  AddObj(18988, 15740, ObjMandrake, 1, 3);
  AddObj(19004, 15740, ObjMandrake, 1, 3);
  AddObj(19020, 15740, ObjMandrake, 1, 3);
  AddObj(19036, 15740, ObjWolfsbane, 1, 3);
  AddObj(19052, 15740, ObjWolfsbane, 1, 3);
  AddObj(19068, 15740, ObjWolfsbane, 1, 3);
  AddObj(19084, 15740, ObjWolfsbane, 1, 3);
  AddObj(18972, 15768, ObjMugwort, 1, 3);
  AddObj(18988, 15768, ObjMugwort, 1, 3);
  AddObj(19004, 15768, ObjMugwort, 1, 3);
  AddObj(19020, 15768, ObjMugwort, 1, 3);
  AddObj(19036, 15768, ObjYarrow, 1, 3);
  AddObj(19052, 15768, ObjYarrow, 1, 3);
  AddObj(19068, 15768, ObjYarrow, 1, 3);
  AddObj(19084, 15768, ObjYarrow, 1, 3);
  AddObj(18972, 15796, ObjNightshade, 1, 3);
  AddObj(18988, 15796, ObjNightshade, 1, 3);
  AddObj(19004, 15796, ObjNightshade, 1, 3);
  AddObj(19020, 15796, ObjNightshade, 1, 3);
  AddObj(19036, 15796, ObjBloodroot, 1, 3);
  AddObj(19052, 15796, ObjBloodroot, 1, 3);
  AddObj(19068, 15796, ObjBloodroot, 1, 3);
  AddObj(19084, 15796, ObjBloodroot, 1, 3);
  (* Prayer skeletons and their dark priest at the nearby stone circle. *)
  AddObj(21480, 15360, 14, 3, 3);
  AddObj(21528, 15360, 14, 3, 3);
  AddObj(21504, 15336, 14, 3, 3);
  AddObj(21504, 15384, 14, 3, 3);
  AddObj(21487, 15343, 14, 3, 3);
  AddObj(21521, 15377, 14, 3, 3);
  AddObj(21504, 15360, 15, 3, 3);    (* dark priest *)

  (* === Region 4 — Desert === *)
  AddObj( 6817, 19693, 13, 3, 4);    (* beggar *)

  (* === Region 5 — Farm and City === *)
  AddObj(22184, 21156, 13, 3, 5);    (* beggar *)
  AddObj(18734, 17595, 17, 1, 5);    (* gold ring *)
  AddObj(21294, 22648, 15, 1, 5);    (* chest *)
  AddObj(22956, 19955,  0, 3, 5);    (* wizard *)
  AddObj(28342, 22613,  0, 3, 5);    (* wizard *)

  (* === Region 6 — Lava Plain === *)
  AddObj(24794, 13102, 13, 3, 6);

  (* === Region 7 — Southern Mountain === *)
  AddObj(23297,  5797, 102, 1, 7);   (* turtle eggs — visible, pickup refused *)

  (* === Region 8 — Building Interiors === *)
  (* NPCs *)
  AddObj( 6700, 33756,  1, 3, 8);    (* priest *)
  AddObj( 5491, 33780,  5, 3, 8);    (* king *)
  AddObj( 5592, 33764,  6, 3, 8);    (* noble *)
  AddObj( 5514, 33668,  2, 3, 8);    (* guard *)
  AddObj( 5574, 33668,  2, 3, 8);    (* guard *)
  AddObj( 8878, 38995,  0, 3, 8);    (* wizard *)
  AddObj( 7776, 34084,  0, 3, 8);    (* wizard *)
  AddObj( 5514, 33881,  3, 3, 8);    (* guard *)
  AddObj( 5574, 33881,  3, 3, 8);    (* guard *)
  AddObj(10853, 35656,  4, 3, 8);    (* princess *)
  AddObj(12037, 37614,  7, 3, 8);    (* sorceress *)
  AddObj(11013, 36804,  9, 3, 8);    (* witch *)
  AddObj( 9631, 38953,  8, 3, 8);    (* bartender *)
  AddObj(10191, 38953,  8, 3, 8);    (* bartender *)
  AddObj(10649, 38953,  8, 3, 8);    (* bartender *)
  AddObj( 2966, 33964,  8, 3, 8);    (* bartender *)
  (* Footstools *)
  AddObj( 9532, 40002, 31, 1, 8);    (* footstool *)
  AddObj( 6747, 33751, 31, 1, 8);    (* footstool *)
  AddObj(11855, 36206, 31, 1, 8);    (* footstool *)
  AddObj(10427, 39977, 31, 1, 8);    (* footstool *)
  (* Collectible items *)
  AddObj(11410, 36169, 155, 1, 8);   (* sunstone *)
  AddObj( 9550, 39964, 23, 1, 8);    (* totem — cabinet *)
  AddObj( 9552, 39964, 23, 1, 8);    (* totem — cabinet *)
  AddObj( 9682, 39964, 23, 1, 8);    (* totem — cabinet *)
  AddObj( 9684, 39964, 23, 1, 8);    (* totem — cabinet *)
  AddObj( 9532, 40119, 23, 1, 8);    (* totem — on table *)
  AddObj( 9575, 39459, 14, 1, 8);    (* urn *)
  AddObj( 9590, 39459, 14, 1, 8);    (* urn *)
  AddObj( 9605, 39459, 14, 1, 8);    (* urn *)
  AddObj( 9680, 39453, 22, 1, 8);    (* vial *)
  AddObj( 9682, 39453, 22, 1, 8);    (* vial *)
  AddObj( 9784, 39453, 22, 1, 8);    (* vial *)
  AddObj( 9668, 39554, 15, 1, 8);    (* chest *)
  AddObj(11090, 39462, 13, 1, 8);    (* money *)
  AddObj(11108, 39458, 23, 1, 8);    (* totem *)
  AddObj(11118, 39459, 23, 1, 8);    (* totem *)
  AddObj(11128, 39459, 23, 1, 8);    (* totem *)
  AddObj(11138, 39458, 23, 1, 8);    (* totem *)
  AddObj(11148, 39459, 23, 1, 8);    (* totem *)
  AddObj(11158, 39459, 23, 1, 8);    (* totem *)
  AddObj(11909, 36198, 15, 1, 8);    (* chest *)
  AddObj(11918, 36246, 23, 1, 8);    (* totem — cabinet *)
  AddObj(11928, 36246, 23, 1, 8);    (* totem — cabinet *)
  AddObj(11938, 36246, 23, 1, 8);    (* totem — cabinet *)
  AddObj(12212, 38481, 15, 1, 8);    (* chest *)
  AddObj(11652, 38481, 242, 1, 8);   (* red key *)
  AddObj(10323, 40071, 14, 1, 8);    (* urn *)
  AddObj(10059, 38472, 16, 1, 8);    (* sacks *)
  AddObj(10344, 36171, 151, 1, 8);   (* shell *)
  AddObj(11936, 36207, 20, 1, 8);    (* scrap/note *)
  AddObj( 9674, 35687, 14, 1, 8);    (* urn *)
  (* Food and gifts *)
  AddObj( 5473, 38699, 147, 1, 8);   (* rose *)
  AddObj( 7185, 34342, 148, 1, 8);   (* fruit *)
  AddObj( 7190, 34342, 148, 1, 8);   (* fruit *)
  AddObj( 7195, 34342, 148, 1, 8);   (* fruit *)
  AddObj( 7185, 34347, 148, 1, 8);   (* fruit *)
  AddObj( 7190, 34347, 148, 1, 8);   (* fruit *)
  AddObj( 7195, 34347, 148, 1, 8);   (* fruit *)
  AddObj( 6593, 34085, 148, 1, 8);   (* fruit *)
  AddObj( 6598, 34085, 148, 1, 8);   (* fruit *)
  AddObj( 6593, 34090, 148, 1, 8);   (* fruit *)
  AddObj( 6598, 34090, 148, 1, 8);   (* fruit *)

  (* === Region 8 — Hidden 'look' items (ob_stat=5) === *)
  AddObj( 3872, 33546, 25, 5, 8);    (* gold key *)
  AddObj( 3887, 33510, 23, 5, 8);    (* totem *)
  AddObj( 4495, 33510, 22, 5, 8);    (* vial *)
  AddObj( 3327, 33383, 24, 5, 8);    (* jade skull *)
  AddObj( 4221, 34119, 11, 5, 8);    (* quiver *)
  AddObj( 7610, 33604, 22, 5, 8);    (* vial *)
  AddObj( 7616, 33522, 13, 5, 8);    (* money *)
  AddObj( 9570, 35768, 18, 5, 8);    (* blue stone *)
  AddObj( 9668, 35769, 11, 5, 8);    (* quiver *)
  AddObj( 9553, 38951, 17, 5, 8);    (* gold ring *)
  AddObj(10062, 39005, 24, 5, 8);    (* jade skull *)
  AddObj(10577, 38951, 22, 5, 8);    (* vial *)
  AddObj(11062, 39514, 13, 5, 8);    (* money *)
  AddObj( 8845, 39494,154, 5, 8);    (* white key *)
  AddObj( 6542, 39494, 19, 5, 8);    (* green jewel *)
  AddObj( 7313, 38992,242, 5, 8);    (* red key *)

  (* === Region 9 — Underground === *)
  AddObj( 7540, 38528, 145, 1, 9);   (* magic wand *)
  AddObj( 9624, 36559, 145, 1, 9);   (* magic wand *)
  AddObj( 9624, 37459, 145, 1, 9);   (* magic wand *)
  AddObj( 8337, 36719, 145, 1, 9);   (* magic wand *)
  AddObj( 8154, 34890, 15, 1, 9);    (* chest *)
  AddObj( 7826, 35741, 15, 1, 9);    (* chest *)
  AddObj( 3460, 37260,  0, 3, 9);    (* wizard *)
  AddObj( 8485, 35725, 13, 1, 9);    (* money *)
  AddObj( 3723, 39340, 138, 1, 9);   (* king's bone *)

  WriteString("World: "); WriteInt(objCount, 1);
  WriteString(" objects placed"); WriteLn
END InitWorldObjects;

PROCEDURE LoadObjectSprites;
VAR p: ARRAY [0..127] OF CHAR;
BEGIN
  AssetPath("objects.bmp", p);
  objTex := LoadBMPKeyedTexture(p, 255, 0, 255);
  IF objTex = NIL THEN
    WriteString("World: object sprites failed"); WriteLn
  ELSE
    WriteString("World: object sprites loaded"); WriteLn
  END
END LoadObjectSprites;

(* --- Dynamic region scattering — original set_objects lines 1727-1745 ---
   On first visit to an outdoor region, scatter 10 random treasure items.
   Original: dstobs[region]==0 && new_region>=10 → generate items.
   rand_treasure = {SACKS×4, CHEST, MONEY, GOLD_KEY, QUIVER,
                    GREY_KEY×3, RED_KEY, B_TOTEM, VIAL, WHITE_KEY, CHEST} *)

CONST
  ApplesPerOutdoorRegion = 125;  (* 8 outdoor regions = 1000 apples *)

VAR
  rng: INTEGER;
  distributed: ARRAY [0..9] OF BOOLEAN;

PROCEDURE BitRand(mask: INTEGER): INTEGER;
BEGIN
  rng := rng * 1103515245 + 12345;
  IF rng < 0 THEN rng := -rng END;
  RETURN BAND(CARDINAL(rng DIV 65536), CARDINAL(mask))
END BitRand;

PROCEDURE RandTreasureId(): INTEGER;
VAR r: INTEGER;
BEGIN
  r := BitRand(15);
  (* rand_treasure[16]: SACKS,SACKS,SACKS,SACKS,CHEST,MONEY,
     GOLD_KEY,QUIVER,GREY_KEY,GREY_KEY,GREY_KEY,RED_KEY,
     B_TOTEM,VIAL,WHITE_KEY,CHEST *)
  IF    r = 0  THEN RETURN 16   (* sacks *)
  ELSIF r = 1  THEN RETURN 16
  ELSIF r = 2  THEN RETURN 16
  ELSIF r = 3  THEN RETURN 16
  ELSIF r = 4  THEN RETURN 15   (* chest *)
  ELSIF r = 5  THEN RETURN 13   (* money *)
  ELSIF r = 6  THEN RETURN 25   (* gold key *)
  ELSIF r = 7  THEN RETURN 11   (* quiver *)
  ELSIF r = 8  THEN RETURN 26   (* grey key *)
  ELSIF r = 9  THEN RETURN 26
  ELSIF r = 10 THEN RETURN 26
  ELSIF r = 11 THEN RETURN 242  (* red key *)
  ELSIF r = 12 THEN RETURN 23   (* bird totem *)
  ELSIF r = 13 THEN RETURN 22   (* vial *)
  ELSIF r = 14 THEN RETURN 154  (* white key *)
  ELSE              RETURN 15   (* chest *)
  END
END RandTreasureId;

PROCEDURE NearTree(x, y: INTEGER): BOOLEAN;
BEGIN
  RETURN (GetTerrainAt(x - 16, y) = 15) OR
         (GetTerrainAt(x + 16, y) = 15) OR
         (GetTerrainAt(x, y - 16) = 15) OR
         (GetTerrainAt(x, y + 16) = 15)
END NearTree;

PROCEDURE AddRegionApples(region: INTEGER);
VAR i, tries, x, y: INTEGER;
BEGIN
  FOR i := 1 TO ApplesPerOutdoorRegion DO
    tries := 0;
    REPEAT
      x := BitRand(16383) + BAND(CARDINAL(region), 1) * 16384;
      y := BitRand(8191)  + BAND(CARDINAL(region), 6) * 4096;
      INC(tries)
    UNTIL ((GetTerrainAt(x, y) = 0) AND NearTree(x, y)) OR
          (tries >= 64);

    (* Some regions have little forest. Always place the remainder on grass. *)
    WHILE GetTerrainAt(x, y) # 0 DO
      x := BitRand(16383) + BAND(CARDINAL(region), 1) * 16384;
      y := BitRand(8191)  + BAND(CARDINAL(region), 6) * 4096
    END;
    AddObj(x, y, ObjFruit, 1, region)
  END
END AddRegionApples;

PROCEDURE DistributeRegion(region: INTEGER);
VAR i, x, y, terrain: INTEGER;
BEGIN
  IF (region < 0) OR (region > 9) THEN RETURN END;
  IF distributed[region] THEN RETURN END;
  (* Regions 8,9 (interiors/underground) are pre-distributed *)
  IF region >= 8 THEN distributed[region] := TRUE; RETURN END;

  distributed[region] := TRUE;
  WriteString("World: distributing region "); WriteInt(region, 1); WriteLn;

  FOR i := 0 TO 9 DO
    (* Original: random coords within region bounds
       x = bitrand(0x3FFF) + (region & 1) * 0x4000
       y = bitrand(0x1FFF) + (region & 6) * 0x1000 *)
    REPEAT
      x := BitRand(16383) + BAND(CARDINAL(region), 1) * 16384;
      y := BitRand(8191)  + BAND(CARDINAL(region), 6) * 4096;
      terrain := GetTerrainAt(x, y)
    UNTIL terrain = 0;  (* only on passable land *)
    AddObj(x, y, RandTreasureId(), 1, region)
  END;
  AddRegionApples(region)
END DistributeRegion;

PROCEDURE IsRegionDistributed(region: INTEGER): BOOLEAN;
BEGIN
  IF (region < 0) OR (region > 9) THEN RETURN FALSE END;
  RETURN distributed[region]
END IsRegionDistributed;

PROCEDURE SetRegionDistributed(region: INTEGER; value: BOOLEAN);
BEGIN
  IF (region < 0) OR (region > 9) THEN RETURN END;
  distributed[region] := value
END SetRegionDistributed;

(* --- Leave item on ground — original leave_item, uses ob_listg[0] slot ---
   Places a single object at given coordinates as a ground item. *)

PROCEDURE LeaveItem(x, y, id: INTEGER);
BEGIN
  AddObj(x, y + 10, id, 1, currentRegion)
END LeaveItem;

PROCEDURE DrawWorldObjects;
VAR i, sx, sy, sprY, ht, id: INTEGER;
BEGIN
  IF objTex = NIL THEN RETURN END;

  SetClip(ren, 0, 0, S(PlayW), S(PlayH));

  FOR i := 0 TO objCount - 1 DO
    IF ((objects[i].status = 1) OR
        (revealHidden AND (objects[i].status = 5))) AND
       ((objects[i].region = currentRegion) OR
        (objects[i].region = -1)) THEN
      sx := (objects[i].x - camX) * Scale;
      sy := (objects[i].y - camY) * Scale;
      IF (sx > -S(20)) AND (sx < S(PlayW) + 20) AND
         (sy > -S(20)) AND (sy < S(PlayH) + 20) THEN
        (* Original: objects with ID >= 128 use (id & 0x7F) for sprite frame
           and render from the bottom half (+8 offset, 8px tall).
           Same frame is shared — top half is one object, bottom half another. *)
        id := objects[i].objId;
        IF (id >= ObjMandrake) AND (id <= ObjBloodroot) THEN
          sprY := (116 + id - ObjMandrake) * ObjSprH
        ELSIF (id >= ObjWardScroll) AND (id <= ObjHealScroll) THEN
          sprY := (122 + (id - ObjWardScroll) MOD 3) * ObjSprH
        ELSIF BAND(CARDINAL(id), 128) # 0 THEN
          sprY := INTEGER(BAND(CARDINAL(id), 127)) * ObjSprH + 8
        ELSE
          sprY := id * ObjSprH
        END;
        (* Original: certain objects render at half height (8px).
           if inum==27 || (inum>=8 && inum<=12) || inum==25 || inum==26 ||
              (inum>16 && inum<24) || (inum & 128)  → ysize=8 *)
        ht := ObjSprH;
        IF (id = 27) OR ((id >= 8) AND (id <= 12)) OR
           (id = 25) OR (id = 26) OR
           ((id > 16) AND (id < 24)) OR
           ((BAND(CARDINAL(id), 128) # 0) AND
            ((id < ObjMandrake) OR (id > ObjHealScroll))) THEN
          ht := 8
        END;
        IF sprY + ht <= ObjSheetH THEN
          DrawTexRegion(objTex,
                        0, sprY, ObjSprW, ht,
                        sx - S(8), sy - S(ht DIV 2),
                        S(ObjSprW), S(ht))
        END
      END
    END
  END;

  ClearClip(ren)
END DrawWorldObjects;

PROCEDURE CheckObjectPickup(heroX, heroY: INTEGER): INTEGER;
VAR i, dx, dy, id: INTEGER;
BEGIN
  FOR i := 0 TO objCount - 1 DO
    IF ((objects[i].status = 1) OR
        (revealHidden AND (objects[i].status = 5))) AND
       ((objects[i].region = currentRegion) OR
        (objects[i].region = -1)) THEN
      dx := heroX - objects[i].x;
      dy := heroY - objects[i].y;
      IF (dx < 16) AND (dx > -16) AND (dy < 16) AND (dy > -16) THEN
        id := objects[i].objId;
        objects[i].status := 2;  (* picked up *)
        RETURN id
      END
    END
  END;
  RETURN -1
END CheckObjectPickup;

END WorldObj.
