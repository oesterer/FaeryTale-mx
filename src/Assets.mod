IMPLEMENTATION MODULE Assets;

FROM SYSTEM IMPORT ADDRESS, ADR;
FROM Strings IMPORT Assign, Concat;
FROM Sys IMPORT m2sys_fopen, m2sys_fclose, m2sys_fread_bytes,
               m2sys_file_exists;
FROM InOut IMPORT WriteString, WriteInt, WriteLn;
FROM Platform IMPORT LoadBMPTexture, LoadBMPScaled, LoadBMPKeyedTexture;
FROM PixBuf IMPORT PBuf, LoadPNG AS PBLoadPNG,
                   LoadPNGPal AS PBLoadPNGPal,
                   Create AS PBCreate, SetPal AS PBSetPal;

CONST
  MaxImgs = 24;

VAR
  pathBuf: ARRAY [0..127] OF CHAR;
  numBuf: ARRAY [0..15] OF CHAR;
  modeBuf: ARRAY [0..3] OF CHAR;
  basePath: ARRAY [0..63] OF CHAR;

  (* All preloaded sector/map data *)
  sect032: ARRAY [0..32767] OF CHAR;
  sect096: ARRAY [0..32767] OF CHAR;
  map160, map168, map176, map184, map192: ARRAY [0..4095] OF CHAR;
  allTerr: ARRAY [0..10] OF ARRAY [0..511] OF CHAR;

  (* Texture cache *)
  cachedNum: ARRAY [0..23] OF INTEGER;
  cachedTex: ARRAY [0..23] OF ADDRESS;
  cachedCount: INTEGER;
  ovlNum: ARRAY [0..23] OF INTEGER;
  ovlTex: ARRAY [0..23] OF ADDRESS;
  ovlCount: INTEGER;

  (* Active pointers set by SwitchRegion — point into preloaded data *)
  activeSect: INTEGER;  (* 0 = sect032, 1 = sect096 *)
  activeMap: INTEGER;   (* 0..4 = which map file *)

PROCEDURE IntToStr3(val: INTEGER; VAR out: ARRAY OF CHAR);
BEGIN
  out[0] := CHR(ORD('0') + val DIV 100);
  out[1] := CHR(ORD('0') + (val MOD 100) DIV 10);
  out[2] := CHR(ORD('0') + val MOD 10);
  out[3] := 0C
END IntToStr3;

PROCEDURE IntToStr2(val: INTEGER; VAR out: ARRAY OF CHAR);
BEGIN
  out[0] := CHR(ORD('0') + val DIV 10);
  out[1] := CHR(ORD('0') + val MOD 10);
  out[2] := 0C
END IntToStr2;

PROCEDURE InitRegionTable;
BEGIN
  regions[0].image[0]:=320; regions[0].image[1]:=480; regions[0].image[2]:=520; regions[0].image[3]:=560;
  regions[0].terra1:=0; regions[0].terra2:=1; regions[0].sector:=32; regions[0].region:=160;
  Assign("Snowy Region", regions[0].name);
  regions[1].image[0]:=320; regions[1].image[1]:=360; regions[1].image[2]:=400; regions[1].image[3]:=440;
  regions[1].terra1:=2; regions[1].terra2:=3; regions[1].sector:=32; regions[1].region:=160;
  Assign("Witch Woods", regions[1].name);
  regions[2].image[0]:=320; regions[2].image[1]:=360; regions[2].image[2]:=520; regions[2].image[3]:=560;
  regions[2].terra1:=2; regions[2].terra2:=1; regions[2].sector:=32; regions[2].region:=168;
  Assign("Swamp Region", regions[2].name);
  regions[3].image[0]:=320; regions[3].image[1]:=360; regions[3].image[2]:=400; regions[3].image[3]:=440;
  regions[3].terra1:=2; regions[3].terra2:=3; regions[3].sector:=32; regions[3].region:=168;
  Assign("Plains Rocks", regions[3].name);
  regions[4].image[0]:=320; regions[4].image[1]:=480; regions[4].image[2]:=520; regions[4].image[3]:=600;
  regions[4].terra1:=0; regions[4].terra2:=4; regions[4].sector:=32; regions[4].region:=176;
  Assign("Desert Area", regions[4].name);
  regions[5].image[0]:=320; regions[5].image[1]:=280; regions[5].image[2]:=240; regions[5].image[3]:=200;
  regions[5].terra1:=5; regions[5].terra2:=6; regions[5].sector:=32; regions[5].region:=176;
  Assign("Bay City Farms", regions[5].name);
  regions[6].image[0]:=320; regions[6].image[1]:=640; regions[6].image[2]:=520; regions[6].image[3]:=600;
  regions[6].terra1:=7; regions[6].terra2:=4; regions[6].sector:=32; regions[6].region:=184;
  Assign("Volcanic", regions[6].name);
  regions[7].image[0]:=320; regions[7].image[1]:=280; regions[7].image[2]:=240; regions[7].image[3]:=200;
  regions[7].terra1:=5; regions[7].terra2:=6; regions[7].sector:=32; regions[7].region:=184;
  Assign("Forest Wilderness", regions[7].name);
  regions[8].image[0]:=680; regions[8].image[1]:=720; regions[8].image[2]:=800; regions[8].image[3]:=840;
  regions[8].terra1:=8; regions[8].terra2:=9; regions[8].sector:=96; regions[8].region:=192;
  Assign("Inside Buildings", regions[8].name);
  regions[9].image[0]:=680; regions[9].image[1]:=760; regions[9].image[2]:=800; regions[9].image[3]:=840;
  regions[9].terra1:=10; regions[9].terra2:=9; regions[9].sector:=96; regions[9].region:=192;
  Assign("Dungeons Caves", regions[9].name)
END InitRegionTable;

PROCEDURE InitAssets;
VAR i: INTEGER;
BEGIN
  currentRegion := -1;
  hudTex := NIL;
  xReg := 0; yReg := 0;
  activeSect := 0; activeMap := 0;
  cachedCount := 0;
  ovlCount := 0;
  shadowPB := NIL;
  pbCount := 0;
  FOR i := 0 TO 3 DO tilePB[i] := NIL END;
  palRef := PBCreate(1, 1);
  IF palRef # NIL THEN SetAmigaPal(palRef) END;
  InitRegionTable;
  Assign("rb", modeBuf);
  FOR i := 0 TO 3 DO tileTex[i] := NIL END;
  FOR i := 0 TO 3 DO tileOverlay[i] := NIL END;
  FOR i := 0 TO 2 DO brotherTex[i] := NIL END;
  FOR i := 0 TO 4 DO enemyTex[i] := NIL END;
  FOR i := 0 TO 4 DO npcTex[i] := NIL END;
  dragonTex := NIL;
  IF m2sys_file_exists(ADR("assets/hiscreen.bmp")) = 1 THEN
    Assign("assets/", basePath)
  ELSIF m2sys_file_exists(ADR("../../assets/hiscreen.bmp")) = 1 THEN
    Assign("../../assets/", basePath)
  ELSE
    Assign("assets/", basePath)
  END
END InitAssets;

PROCEDURE AssetPath(name: ARRAY OF CHAR; VAR result: ARRAY OF CHAR);
BEGIN
  Assign(basePath, result);
  Concat(result, name, result)
END AssetPath;

PROCEDURE MakePath(prefix: ARRAY OF CHAR; num, digits: INTEGER;
                   ext: ARRAY OF CHAR);
BEGIN
  Assign(basePath, pathBuf);
  Concat(pathBuf, prefix, pathBuf);
  IF digits = 3 THEN IntToStr3(num, numBuf)
  ELSE IntToStr2(num, numBuf)
  END;
  Concat(pathBuf, numBuf, pathBuf);
  Concat(pathBuf, ext, pathBuf)
END MakePath;

PROCEDURE LoadBin(path: ARRAY OF CHAR; buf: ADDRESS; size: INTEGER): BOOLEAN;
VAR fd, n: INTEGER;
BEGIN
  fd := m2sys_fopen(ADR(path), ADR(modeBuf));
  IF fd < 0 THEN
    WriteString("  FAILED: "); WriteString(path); WriteLn;
    RETURN FALSE
  END;
  n := m2sys_fread_bytes(fd, buf, size);
  m2sys_fclose(fd);
  RETURN n >= size
END LoadBin;

PROCEDURE LoadImgCached(num: INTEGER): ADDRESS;
VAR i: INTEGER;
    tex: ADDRESS;
BEGIN
  FOR i := 0 TO cachedCount - 1 DO
    IF cachedNum[i] = num THEN RETURN cachedTex[i] END
  END;
  MakePath("image_", num, 3, ".bmp");
  WriteString("  "); WriteString(pathBuf); WriteLn;
  tex := LoadBMPTexture(pathBuf);
  IF (tex # NIL) AND (cachedCount < MaxImgs) THEN
    cachedNum[cachedCount] := num;
    cachedTex[cachedCount] := tex;
    INC(cachedCount)
  END;
  RETURN tex
END LoadImgCached;

PROCEDURE LoadOvlCached(num: INTEGER): ADDRESS;
VAR i: INTEGER;
    tex: ADDRESS;
BEGIN
  FOR i := 0 TO ovlCount - 1 DO
    IF ovlNum[i] = num THEN RETURN ovlTex[i] END
  END;
  MakePath("image_", num, 3, ".bmp");
  (* Load with black (0,0,0) as transparent color key *)
  tex := LoadBMPKeyedTexture(pathBuf, 0, 0, 0);
  IF (tex # NIL) AND (ovlCount < MaxImgs) THEN
    ovlNum[ovlCount] := num;
    ovlTex[ovlCount] := tex;
    INC(ovlCount)
  END;
  RETURN tex
END LoadOvlCached;

VAR
  pbNum: ARRAY [0..23] OF INTEGER;
  pbBuf: ARRAY [0..23] OF ADDRESS;
  pbCount: INTEGER;
  palRef: PBuf;  (* 1x1 PixBuf holding the Amiga palette *)

PROCEDURE SetAmigaPal(pb: PBuf);
BEGIN
  PBSetPal(pb,  0,   0,   0,   0);
  PBSetPal(pb,  1, 255, 255, 255);
  PBSetPal(pb,  2, 238, 153, 102);
  PBSetPal(pb,  3, 187, 102,  51);
  PBSetPal(pb,  4, 102,  51,  17);
  PBSetPal(pb,  5, 119, 187, 255);
  PBSetPal(pb,  6,  51,  51,  51);
  PBSetPal(pb,  7, 221, 187, 136);
  PBSetPal(pb,  8,  34,  34,  51);
  PBSetPal(pb,  9,  68,  68,  85);
  PBSetPal(pb, 10, 136, 136, 153);
  PBSetPal(pb, 11, 187, 187, 204);
  PBSetPal(pb, 12,  85,  34,  17);
  PBSetPal(pb, 13, 153,  68,  17);
  PBSetPal(pb, 14, 255, 136,  34);
  PBSetPal(pb, 15, 255, 204, 119);
  PBSetPal(pb, 16,   0,  68,   0);
  PBSetPal(pb, 17,   0, 119,   0);
  PBSetPal(pb, 18,   0, 187,   0);
  PBSetPal(pb, 19, 102, 255, 102);
  PBSetPal(pb, 20,   0,   0,  85);
  PBSetPal(pb, 21,   0,   0, 153);
  PBSetPal(pb, 22,   0,   0, 221);
  PBSetPal(pb, 23,  51, 119, 255);
  PBSetPal(pb, 24, 204,   0,   0);
  PBSetPal(pb, 25, 255,  85,   0);
  PBSetPal(pb, 26, 255, 170,   0);
  PBSetPal(pb, 27, 255, 255, 102);
  PBSetPal(pb, 28, 238, 187, 102);
  PBSetPal(pb, 29, 238, 170,  85);
  PBSetPal(pb, 30,   0,   0, 255);
  PBSetPal(pb, 31, 187, 221, 255)
END SetAmigaPal;

PROCEDURE LoadPBCached(num: INTEGER): ADDRESS;
VAR i: INTEGER;
    pb: ADDRESS;
BEGIN
  FOR i := 0 TO pbCount - 1 DO
    IF pbNum[i] = num THEN RETURN pbBuf[i] END
  END;
  MakePath("image_", num, 3, ".png");
  IF palRef # NIL THEN
    pb := PBLoadPNGPal(pathBuf, palRef, 32)
  ELSE
    pb := PBLoadPNG(pathBuf, 32)
  END;
  IF (pb # NIL) AND (pbCount < MaxImgs) THEN
    pbNum[pbCount] := num;
    pbBuf[pbCount] := pb;
    INC(pbCount)
  END;
  RETURN pb
END LoadPBCached;

PROCEDURE PreloadAll(): BOOLEAN;
VAR i, j: INTEGER;
BEGIN
  WriteString("Preloading all game assets..."); WriteLn;

  MakePath("sector_", 32, 3, ".bin");
  WriteString("  "); WriteString(pathBuf); WriteLn;
  IF NOT LoadBin(pathBuf, ADR(sect032), SectorSize) THEN RETURN FALSE END;
  MakePath("sector_", 96, 3, ".bin");
  WriteString("  "); WriteString(pathBuf); WriteLn;
  IF NOT LoadBin(pathBuf, ADR(sect096), SectorSize) THEN RETURN FALSE END;

  MakePath("map_", 160, 3, ".bin"); WriteString("  "); WriteString(pathBuf); WriteLn;
  IF NOT LoadBin(pathBuf, ADR(map160), MapSize) THEN RETURN FALSE END;
  MakePath("map_", 168, 3, ".bin"); WriteString("  "); WriteString(pathBuf); WriteLn;
  IF NOT LoadBin(pathBuf, ADR(map168), MapSize) THEN RETURN FALSE END;
  MakePath("map_", 176, 3, ".bin"); WriteString("  "); WriteString(pathBuf); WriteLn;
  IF NOT LoadBin(pathBuf, ADR(map176), MapSize) THEN RETURN FALSE END;
  MakePath("map_", 184, 3, ".bin"); WriteString("  "); WriteString(pathBuf); WriteLn;
  IF NOT LoadBin(pathBuf, ADR(map184), MapSize) THEN RETURN FALSE END;
  MakePath("map_", 192, 3, ".bin"); WriteString("  "); WriteString(pathBuf); WriteLn;
  IF NOT LoadBin(pathBuf, ADR(map192), MapSize) THEN RETURN FALSE END;

  FOR i := 0 TO 10 DO
    MakePath("terrain_", i, 2, ".bin"); WriteString("  "); WriteString(pathBuf); WriteLn;
    IF NOT LoadBin(pathBuf, ADR(allTerr[i]), TerrainSize) THEN RETURN FALSE END
  END;

  (* Preload ALL image textures across all regions — normal + overlay *)
  FOR i := 0 TO NumRegions - 1 DO
    FOR j := 0 TO 3 DO
      IF LoadImgCached(regions[i].image[j]) = NIL THEN RETURN FALSE END;
      IF LoadOvlCached(regions[i].image[j]) = NIL THEN
        WriteString("overlay load failed"); WriteLn
      END
    END
  END;

  (* Load shadow mask PixBuf *)
  AssetPath("shadow_mem.png", pathBuf);
  shadowPB := PBLoadPNG(pathBuf, 256);
  IF shadowPB = NIL THEN
    WriteString("*** Shadow mask failed ***"); WriteLn
  ELSE
    WriteString("Shadow mask loaded"); WriteLn
  END;

  (* Load brother sprite sheets with magenta color key *)
  AssetPath("julian.bmp", pathBuf);
  brotherTex[0] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("phillip.bmp", pathBuf);
  brotherTex[1] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("kevin.bmp", pathBuf);
  brotherTex[2] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  IF brotherTex[0] = NIL THEN
    WriteString("*** Brother sprites failed ***"); WriteLn
  ELSE
    WriteString("Brother sprites loaded"); WriteLn
  END;

  (* Load enemy sprite sheets *)
  AssetPath("shape_6_Ogre_16x32_x64.bmp", pathBuf);
  enemyTex[0] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("shape_7_Ghost_16x32_x64.bmp", pathBuf);
  enemyTex[1] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("shape_8_DKnight-Spiders_16x32_x64.bmp", pathBuf);
  enemyTex[2] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("shape_9_Necro-Farmer-Loraii_16x32_x64.bmp", pathBuf);
  enemyTex[3] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("shape_12_Snake-Salamander_16x32_x64.bmp", pathBuf);
  enemyTex[4] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  IF enemyTex[0] # NIL THEN
    WriteString("Enemy sprites loaded"); WriteLn
  END;

  (* Load NPC sprite sheets *)
  AssetPath("shape_13_Wizard-Priest_16x32_x8.bmp", pathBuf);
  npcTex[0] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("shape_14_Royal-Set_16x32_x8.bmp", pathBuf);
  npcTex[1] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("shape_15_Bartender_16x32_x8.bmp", pathBuf);
  npcTex[2] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("shape_16_Witch_16x32_x8.bmp", pathBuf);
  npcTex[3] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("shape_17_Ranger-Beggar_16x32_x8.bmp", pathBuf);
  npcTex[4] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  AssetPath("scroll_priest_16x32.bmp", pathBuf);
  npcTex[5] := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  IF npcTex[0] # NIL THEN
    WriteString("NPC sprites loaded"); WriteLn
  END;

  (* Dragon sprite: 48x40 × 5 frames *)
  AssetPath("shape_10_Dragon_48x40_x5.bmp", pathBuf);
  dragonTex := LoadBMPKeyedTexture(pathBuf, 255, 0, 255);
  IF dragonTex # NIL THEN
    WriteString("Dragon sprite loaded"); WriteLn
  END;

  WriteString("Done: "); WriteInt(cachedCount, 1);
  WriteString(" textures cached"); WriteLn;
  RETURN TRUE
END PreloadAll;

(* Instant region switch — zero disk I/O *)
PROCEDURE SwitchRegion(regionIdx: INTEGER);
VAR i: INTEGER;
BEGIN
  IF (regionIdx < 0) OR (regionIdx >= NumRegions) THEN RETURN END;
  IF regionIdx = currentRegion THEN RETURN END;

  (* Select active sector buffer *)
  IF regions[regionIdx].sector = 96 THEN activeSect := 1
  ELSE activeSect := 0
  END;

  (* Select active map buffer *)
  CASE regions[regionIdx].region OF
    160: activeMap := 0 |
    168: activeMap := 1 |
    176: activeMap := 2 |
    184: activeMap := 3 |
    192: activeMap := 4
  ELSE
    activeMap := 0
  END;

  (* Copy terrain into working buffer (1KB, trivial) *)
  FOR i := 0 TO TerrainSize - 1 DO
    terraMem[i] := allTerr[regions[regionIdx].terra1][i];
    terraMem[512 + i] := allTerr[regions[regionIdx].terra2][i]
  END;

  (* Point tileTex to cached textures — instant *)
  FOR i := 0 TO 3 DO
    tileTex[i] := LoadImgCached(regions[regionIdx].image[i]);
    tileOverlay[i] := LoadOvlCached(regions[regionIdx].image[i]);
    tilePB[i] := LoadPBCached(regions[regionIdx].image[i])
  END;

  currentRegion := regionIdx;
  IF regionIdx < 8 THEN
    xReg := (regionIdx MOD 2) * 64;
    yReg := (regionIdx DIV 2) * 32
  ELSE
    xReg := 0;
    yReg := (regionIdx DIV 2) * 32
  END;

  (* Copy active data into exported arrays so DebugMap can read them *)
  IF activeSect = 1 THEN
    sectorMem := sect096
  ELSE
    sectorMem := sect032
  END;
  CASE activeMap OF
    0: mapMem := map160 |
    1: mapMem := map168 |
    2: mapMem := map176 |
    3: mapMem := map184 |
    4: mapMem := map192
  ELSE
  END
END SwitchRegion;

PROCEDURE LoadHUD(targetW, targetH: INTEGER): BOOLEAN;
BEGIN
  Assign(basePath, pathBuf);
  Concat(pathBuf, "hiscreen.bmp", pathBuf);
  (* Pre-render the BMP into an RGBA texture at the target size.
     This avoids SDL issues with source rects on paletted textures. *)
  hudTex := LoadBMPScaled(pathBuf, targetW, targetH);
  RETURN hudTex # NIL
END LoadHUD;

PROCEDURE GetSectorByteForRegion(x, y, regIdx: INTEGER): INTEGER;
VAR imx, imy, secx, secy, secNum, offset: INTEGER;
    rxr, ryr: INTEGER;
BEGIN
  IF (regIdx < 0) OR (regIdx >= NumRegions) THEN RETURN 0 END;
  imx := x DIV TileW;
  imy := y DIV TileH;

  (* Compute xReg/yReg for the target region *)
  IF regIdx < 8 THEN
    rxr := (regIdx MOD 2) * 64;
    ryr := (regIdx DIV 2) * 32
  ELSE
    rxr := 0;
    ryr := (regIdx DIV 2) * 32
  END;

  secx := (imx DIV 16) - rxr;
  IF (secx < 0) OR (secx >= 64) THEN
    IF BAND(CARDINAL(secx), 32) # 0 THEN
      secx := 0
    ELSE
      secx := 63
    END
  END;
  secy := (imy DIV 8) - ryr;
  IF secy < 0 THEN secy := 0 END;
  IF secy >= 32 THEN secy := 31 END;

  offset := secy * 128 + secx + rxr;
  IF (offset < 0) OR (offset >= MapSize) THEN RETURN 0 END;
  CASE regions[regIdx].region OF
    160: secNum := ORD(map160[offset]) |
    168: secNum := ORD(map168[offset]) |
    176: secNum := ORD(map176[offset]) |
    184: secNum := ORD(map184[offset]) |
    192: secNum := ORD(map192[offset])
  ELSE
    secNum := 0
  END;
  offset := secNum * 128 + (imy MOD 8) * 16 + (imx MOD 16);
  IF (offset < 0) OR (offset >= SectorSize) THEN RETURN 0 END;
  IF regions[regIdx].sector = 96 THEN
    RETURN ORD(sect096[offset])
  ELSE
    RETURN ORD(sect032[offset])
  END
END GetSectorByteForRegion;

PROCEDURE GetSectorByte(x, y: INTEGER): INTEGER;
VAR imx, imy, secx, secy, secNum, offset: INTEGER;
BEGIN
  imx := x DIV TileW;
  imy := y DIV TileH;

  (* Original px_to_im uses xReg/yReg offsets:
     secx = (imx/16) - xReg, then wrap to 0-127
     secy = (imy/8) - yReg, then clamp to 0-31
     sec_num = secy*128 + secx + xReg *)
  secx := (imx DIV 16) - xReg;
  (* Wrapping from original px_to_im asm:
     if bit6 set (out of range): bit5=0 → 63, bit5=1 → 0 *)
  IF (secx < 0) OR (secx >= 64) THEN
    IF BAND(CARDINAL(secx), 32) # 0 THEN
      secx := 0
    ELSE
      secx := 63
    END
  END;

  secy := (imy DIV 8) - yReg;
  IF secy < 0 THEN secy := 0 END;
  IF secy >= 32 THEN secy := 31 END;

  offset := secy * 128 + secx + xReg;
  IF (offset < 0) OR (offset >= MapSize) THEN RETURN 0 END;
  (* Read from active map buffer *)
  CASE activeMap OF
    0: secNum := ORD(map160[offset]) |
    1: secNum := ORD(map168[offset]) |
    2: secNum := ORD(map176[offset]) |
    3: secNum := ORD(map184[offset]) |
    4: secNum := ORD(map192[offset])
  ELSE
    secNum := 0
  END;
  offset := secNum * 128 + (imy MOD 8) * 16 + (imx MOD 16);
  IF (offset < 0) OR (offset >= SectorSize) THEN RETURN 0 END;
  (* Read from active sector buffer *)
  IF activeSect = 1 THEN
    RETURN ORD(sect096[offset])
  ELSE
    RETURN ORD(sect032[offset])
  END
END GetSectorByte;

PROCEDURE SetSectorByte(x, y, val: INTEGER);
VAR imx, imy, secx, secy, secNum, offset: INTEGER;
BEGIN
  imx := x DIV TileW;
  imy := y DIV TileH;
  secx := (imx DIV 16) - xReg;
  IF (secx < 0) OR (secx >= 64) THEN
    IF BAND(CARDINAL(secx), 32) # 0 THEN secx := 0
    ELSE secx := 63
    END
  END;
  secy := (imy DIV 8) - yReg;
  IF secy < 0 THEN secy := 0 END;
  IF secy >= 32 THEN secy := 31 END;
  offset := secy * 128 + secx + xReg;
  IF (offset < 0) OR (offset >= MapSize) THEN RETURN END;
  CASE activeMap OF
    0: secNum := ORD(map160[offset]) |
    1: secNum := ORD(map168[offset]) |
    2: secNum := ORD(map176[offset]) |
    3: secNum := ORD(map184[offset]) |
    4: secNum := ORD(map192[offset])
  ELSE RETURN
  END;
  offset := secNum * 128 + (imy MOD 8) * 16 + (imx MOD 16);
  IF (offset < 0) OR (offset >= SectorSize) THEN RETURN END;
  IF activeSect = 1 THEN
    sect096[offset] := CHR(val)
  ELSE
    sect032[offset] := CHR(val)
  END
END SetSectorByte;

PROCEDURE GetMapSector(x, y: INTEGER): INTEGER;
VAR imx, imy, secx, secy, offset, secNum: INTEGER;
BEGIN
  imx := x DIV TileW;
  imy := y DIV TileH;
  secx := (imx DIV 16) - xReg;
  IF (secx < 0) OR (secx >= 64) THEN
    IF BAND(CARDINAL(secx), 32) # 0 THEN
      secx := 0
    ELSE
      secx := 63
    END
  END;
  secy := (imy DIV 8) - yReg;
  IF secy < 0 THEN secy := 0 END;
  IF secy >= 32 THEN secy := 31 END;
  offset := secy * 128 + secx + xReg;
  IF (offset < 0) OR (offset >= MapSize) THEN RETURN 0 END;
  CASE activeMap OF
    0: secNum := ORD(map160[offset]) |
    1: secNum := ORD(map168[offset]) |
    2: secNum := ORD(map176[offset]) |
    3: secNum := ORD(map184[offset]) |
    4: secNum := ORD(map192[offset])
  ELSE
    secNum := 0
  END;
  RETURN secNum
END GetMapSector;

PROCEDURE SetMapSector(x, y, val: INTEGER);
VAR imx, imy, secx, secy, offset: INTEGER;
BEGIN
  imx := x DIV TileW;
  imy := y DIV TileH;
  secx := (imx DIV 16) - xReg;
  IF (secx < 0) OR (secx >= 64) THEN
    IF BAND(CARDINAL(secx), 32) # 0 THEN secx := 0
    ELSE secx := 63
    END
  END;
  secy := (imy DIV 8) - yReg;
  IF secy < 0 THEN secy := 0 END;
  IF secy >= 32 THEN secy := 31 END;
  offset := secy * 128 + secx + xReg;
  IF (offset < 0) OR (offset >= MapSize) THEN RETURN END;
  CASE activeMap OF
    0: map160[offset] := CHR(val) |
    1: map168[offset] := CHR(val) |
    2: map176[offset] := CHR(val) |
    3: map184[offset] := CHR(val) |
    4: map192[offset] := CHR(val)
  ELSE
  END
END SetMapSector;

PROCEDURE GetTerrainAt(x, y: INTEGER): INTEGER;
VAR secByte, cm, tilesMask, subBit: INTEGER;
BEGIN
  secByte := GetSectorByte(x, y);
  cm := secByte * 4;
  IF (cm + 2 < 0) OR (cm + 2 >= 1024) THEN RETURN 0 END;

  (* Sub-tile bitmask: 2 columns x 4 rows = 8 sub-regions per tile.
     Matches original px_to_im logic in fsubs.asm. *)
  subBit := 128;  (* start at bit 7 = top-left *)
  IF (x MOD 16) >= 8 THEN  (* right half *)
    subBit := subBit DIV 16
  END;
  IF (y MOD 32) >= 16 THEN  (* bottom half *)
    subBit := subBit DIV 4
  END;
  IF ((y MOD 32) MOD 16) >= 8 THEN  (* lower quarter *)
    subBit := subBit DIV 2
  END;

  (* Check tiles bitmask (byte 2) against sub-region bit.
     If the bit is not set, this sub-region is passable. *)
  tilesMask := ORD(terraMem[cm + 2]);
  IF (tilesMask DIV subBit) MOD 2 = 0 THEN
    RETURN 0  (* sub-region is passable *)
  END;

  (* High nibble of byte 1 = terrain category *)
  RETURN (ORD(terraMem[cm + 1]) DIV 16) MOD 16
END GetTerrainAt;

PROCEDURE GetTilesBits(secByte: INTEGER): INTEGER;
VAR cm: INTEGER;
BEGIN
  cm := secByte * 4;
  IF (cm + 2 < 0) OR (cm + 2 >= 1024) THEN RETURN 0 END;
  RETURN ORD(terraMem[cm + 2])
END GetTilesBits;

PROCEDURE GetMapTag(secByte: INTEGER): INTEGER;
VAR cm: INTEGER;
BEGIN
  cm := secByte * 4;
  IF (cm < 0) OR (cm >= 1024) THEN RETURN 0 END;
  RETURN ORD(terraMem[cm])
END GetMapTag;

PROCEDURE GetMaskType(secByte: INTEGER): INTEGER;
VAR cm: INTEGER;
BEGIN
  cm := secByte * 4;
  IF (cm + 1 < 0) OR (cm + 1 >= 1024) THEN RETURN 0 END;
  (* Low nibble of byte 1 = mask application type *)
  RETURN ORD(terraMem[cm + 1]) MOD 16
END GetMaskType;

PROCEDURE IsBlocked(x, y: INTEGER): BOOLEAN;
VAR t: INTEGER;
BEGIN
  t := GetTerrainAt(x, y);
  (* Original prox: terrain 1 = blocked, terrain >= 10 = blocked.
     Player ignores 8,9 (swamp/palace).
     Terrain 15 = door — passable so player can reach door trigger. *)
  IF t = 15 THEN RETURN FALSE END;
  RETURN (t = 1) OR (t >= 10)
END IsBlocked;

PROCEDURE TerrainSpeedAt(x, y: INTEGER): INTEGER;
VAR t: INTEGER;
BEGIN
  t := GetTerrainAt(x, y);
  (* Original speeds: normal=2, slow=1, fast=4, blocked=0 *)
  CASE t OF
    0:  RETURN 2 |  (* grass: normal *)
    1:  RETURN 0 |  (* wall: blocked *)
    2:  RETURN 4 |  (* road: fast *)
    3:  RETURN 2 |  (* shore: normal *)
    4:  RETURN 2 |  (* misc: normal *)
    5:  RETURN 1 |  (* water: slow *)
    10: RETURN 1 |  (* rough edge: slow *)
    15: RETURN 1    (* forest: slow *)
  ELSE
    RETURN 2
  END
END TerrainSpeedAt;

PROCEDURE DetectRegion(mapX, mapY: INTEGER): INTEGER;
VAR xs, ys, xr, yr: INTEGER;
BEGIN
  (* Original uses map_x/map_y (camera position), not hero position.
     xs = (map_x + 151) >> 8, xr = (xs >> 6) & 1
     ys = (map_y + 64) >> 8,  yr = (ys >> 5) & 3 *)
  xs := (mapX + 151) DIV 256;
  ys := (mapY + 64) DIV 256;
  xr := (xs DIV 64) MOD 2;
  yr := (ys DIV 32) MOD 8;  (* original: & 7, NOT & 3 *)
  RETURN xr + yr * 2
END DetectRegion;

PROCEDURE CheckRegionSwitch(px, py: INTEGER);
VAR newReg: INTEGER;
BEGIN
  IF currentRegion >= 8 THEN RETURN END;
  newReg := DetectRegion(px, py);
  IF (newReg # currentRegion) AND (newReg >= 0) AND (newReg < 8) THEN
    SwitchRegion(newReg)
  END
END CheckRegionSwitch;

END Assets.
