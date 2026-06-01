IMPLEMENTATION MODULE DebugMap;

(* Full world debug map: shows ALL regions combined.
   World = 4 rows of map files × 128 columns = 2048×1024 tiles.
   Each pixel = one tile, colored by sampling tile palette index. *)

FROM Gfx IMPORT Window, Renderer,
                CreateWindow, DestroyWindow,
                CreateRenderer, DestroyRenderer,
                Present, WIN_CENTERED, RENDER_ACCELERATED;
FROM Canvas IMPORT SetColor, FillRect, Clear AS CanvasClear;
FROM Assets IMPORT currentRegion, AssetPath;
FROM Actor IMPORT actors;
FROM GameState IMPORT cycle;
FROM SYSTEM IMPORT ADDRESS, ADR;
FROM Sys IMPORT m2sys_fopen, m2sys_fclose, m2sys_fread_bytes;
FROM Strings IMPORT Assign;

CONST
  FullW = 2048;  (* 128 sectors × 16 tiles *)
  FullH = 1024;  (* 4 rows × 32 sectors × 8 tiles *)
  WinW  = 768;
  WinH  = 768;

VAR
  dbgWin: Window;
  dbgRen: Renderer;
  open: BOOLEAN;
  needRedraw: BOOLEAN;
  lastDrawnRegion: INTEGER;

  palR, palG, palB: ARRAY [0..31] OF INTEGER;
  tc: ARRAY [0..255] OF INTEGER; (* tile byte -> dominant palette index *)

  (* All sector and map data for the full world *)
  wSector: ARRAY [0..32767] OF CHAR;
  wMap: ARRAY [0..3] OF ARRAY [0..4095] OF CHAR;
  worldLoaded: BOOLEAN;

PROCEDURE InitPalette;
BEGIN
  palR[0]:=0;palG[0]:=0;palB[0]:=0;palR[1]:=255;palG[1]:=255;palB[1]:=255;
  palR[2]:=238;palG[2]:=153;palB[2]:=102;palR[3]:=187;palG[3]:=102;palB[3]:=51;
  palR[4]:=102;palG[4]:=51;palB[4]:=17;palR[5]:=119;palG[5]:=187;palB[5]:=255;
  palR[6]:=51;palG[6]:=51;palB[6]:=51;palR[7]:=221;palG[7]:=187;palB[7]:=136;
  palR[8]:=34;palG[8]:=34;palB[8]:=51;palR[9]:=68;palG[9]:=68;palB[9]:=85;
  palR[10]:=136;palG[10]:=136;palB[10]:=153;palR[11]:=187;palG[11]:=187;palB[11]:=204;
  palR[12]:=85;palG[12]:=34;palB[12]:=17;palR[13]:=153;palG[13]:=68;palB[13]:=17;
  palR[14]:=255;palG[14]:=136;palB[14]:=34;palR[15]:=255;palG[15]:=204;palB[15]:=119;
  palR[16]:=0;palG[16]:=68;palB[16]:=0;palR[17]:=0;palG[17]:=119;palB[17]:=0;
  palR[18]:=0;palG[18]:=187;palB[18]:=0;palR[19]:=102;palG[19]:=255;palB[19]:=102;
  palR[20]:=0;palG[20]:=0;palB[20]:=85;palR[21]:=0;palG[21]:=0;palB[21]:=153;
  palR[22]:=0;palG[22]:=0;palB[22]:=221;palR[23]:=51;palG[23]:=119;palB[23]:=255;
  palR[24]:=204;palG[24]:=0;palB[24]:=0;palR[25]:=255;palG[25]:=85;palB[25]:=0;
  palR[26]:=255;palG[26]:=170;palB[26]:=0;palR[27]:=255;palG[27]:=255;palB[27]:=102;
  palR[28]:=238;palG[28]:=187;palB[28]:=102;palR[29]:=238;palG[29]:=170;palB[29]:=85;
  palR[30]:=0;palG[30]:=0;palB[30]:=255;palR[31]:=187;palG[31]:=221;palB[31]:=255
END InitPalette;

PROCEDURE LoadWorldData;
VAR fd, n: INTEGER;
    modeBuf: ARRAY [0..3] OF CHAR;
    p: ARRAY [0..127] OF CHAR;
BEGIN
  Assign("rb", modeBuf);

  AssetPath("sector_032.bin", p);
  fd := m2sys_fopen(ADR(p), ADR(modeBuf));
  IF fd >= 0 THEN
    n := m2sys_fread_bytes(fd, ADR(wSector), 32768);
    m2sys_fclose(fd)
  END;

  AssetPath("map_160.bin", p);
  fd := m2sys_fopen(ADR(p), ADR(modeBuf));
  IF fd >= 0 THEN n := m2sys_fread_bytes(fd, ADR(wMap[0]), 4096); m2sys_fclose(fd) END;

  AssetPath("map_168.bin", p);
  fd := m2sys_fopen(ADR(p), ADR(modeBuf));
  IF fd >= 0 THEN n := m2sys_fread_bytes(fd, ADR(wMap[1]), 4096); m2sys_fclose(fd) END;

  AssetPath("map_176.bin", p);
  fd := m2sys_fopen(ADR(p), ADR(modeBuf));
  IF fd >= 0 THEN n := m2sys_fread_bytes(fd, ADR(wMap[2]), 4096); m2sys_fclose(fd) END;

  AssetPath("map_184.bin", p);
  fd := m2sys_fopen(ADR(p), ADR(modeBuf));
  IF fd >= 0 THEN n := m2sys_fread_bytes(fd, ADR(wMap[3]), 4096); m2sys_fclose(fd) END;

  worldLoaded := TRUE
END LoadWorldData;

PROCEDURE InitTileColors;
BEGIN
  tc[0]:=11;tc[1]:=14;tc[2]:=12;tc[3]:=14;tc[4]:=12;tc[5]:=14;tc[6]:=12;tc[7]:=17;
  tc[8]:=12;tc[9]:=14;tc[10]:=17;tc[11]:=17;tc[12]:=12;tc[13]:=14;tc[14]:=17;tc[15]:=8;
  tc[16]:=8;tc[17]:=17;tc[18]:=21;tc[19]:=21;tc[20]:=21;tc[21]:=20;tc[22]:=23;tc[23]:=17;
  tc[24]:=15;tc[25]:=17;tc[26]:=23;tc[27]:=5;tc[28]:=15;tc[29]:=17;tc[30]:=23;tc[31]:=23;
  tc[32]:=15;tc[33]:=5;tc[34]:=21;tc[35]:=15;tc[36]:=15;tc[37]:=17;tc[38]:=16;tc[39]:=17;
  tc[40]:=17;tc[41]:=17;tc[42]:=16;tc[43]:=17;tc[44]:=16;tc[45]:=16;tc[46]:=16;tc[47]:=16;
  tc[48]:=16;tc[49]:=17;tc[50]:=16;tc[51]:=16;tc[52]:=16;tc[53]:=17;tc[54]:=16;tc[55]:=17;
  tc[56]:=17;tc[57]:=16;tc[58]:=16;tc[59]:=16;tc[60]:=16;tc[61]:=17;tc[62]:=17;tc[63]:=17;
  tc[64]:=8;tc[65]:=8;tc[66]:=0;tc[67]:=8;tc[68]:=16;tc[69]:=16;tc[70]:=18;tc[71]:=16;
  tc[72]:=16;tc[73]:=16;tc[74]:=8;tc[75]:=0;tc[76]:=17;tc[77]:=17;tc[78]:=17;tc[79]:=17;
  tc[80]:=16;tc[81]:=17;tc[82]:=17;tc[83]:=1;tc[84]:=23;tc[85]:=17;tc[86]:=5;tc[87]:=1;
  tc[88]:=5;tc[89]:=17;tc[90]:=1;tc[91]:=16;tc[92]:=31;tc[93]:=29;tc[94]:=28;tc[95]:=29;
  tc[96]:=15;tc[97]:=3;tc[98]:=31;tc[99]:=31;tc[100]:=31;tc[101]:=29;tc[102]:=31;tc[103]:=17;
  tc[104]:=16;tc[105]:=3;tc[106]:=31;tc[107]:=31;tc[108]:=17;tc[109]:=17;tc[110]:=8;tc[111]:=9;
  tc[112]:=10;tc[113]:=10;tc[114]:=10;tc[115]:=17;tc[116]:=17;tc[117]:=10;tc[118]:=9;tc[119]:=23;
  tc[120]:=23;tc[121]:=9;tc[122]:=10;tc[123]:=15;tc[124]:=8;tc[125]:=9;tc[126]:=10;tc[127]:=15;
  tc[128]:=5;tc[129]:=7;tc[130]:=6;tc[131]:=7;tc[132]:=6;tc[133]:=7;tc[134]:=6;tc[135]:=8;
  tc[136]:=6;tc[137]:=7;tc[138]:=8;tc[139]:=8;tc[140]:=6;tc[141]:=7;tc[142]:=8;tc[143]:=4;
  tc[144]:=4;tc[145]:=8;tc[146]:=10;tc[147]:=10;tc[148]:=10;tc[149]:=10;tc[150]:=11;tc[151]:=11;
  tc[152]:=11;tc[153]:=8;tc[154]:=11;tc[155]:=2;tc[156]:=7;tc[157]:=8;tc[158]:=11;tc[159]:=11;
  tc[160]:=7;tc[161]:=2;tc[162]:=11;tc[163]:=7;tc[164]:=7;tc[165]:=8;tc[166]:=8;tc[167]:=8;
  tc[168]:=8;tc[169]:=8;tc[170]:=8;tc[171]:=8;tc[172]:=8;tc[173]:=8;tc[174]:=8;tc[175]:=8;
  tc[176]:=8;tc[177]:=8;tc[178]:=8;tc[179]:=8;tc[180]:=8;tc[181]:=8;tc[182]:=8;tc[183]:=8;
  tc[184]:=8;tc[185]:=8;tc[186]:=8;tc[187]:=8;tc[188]:=8;tc[189]:=8;tc[190]:=8;tc[191]:=8;
  tc[192]:=5;tc[193]:=4;tc[194]:=1;tc[195]:=14;tc[196]:=14;tc[197]:=1;tc[198]:=8;tc[199]:=8;
  tc[200]:=8;tc[201]:=8;tc[202]:=8;tc[203]:=8;tc[204]:=8;tc[205]:=5;tc[206]:=8;tc[207]:=8;
  tc[208]:=5;tc[209]:=3;tc[210]:=3;tc[211]:=3;tc[212]:=3;tc[213]:=3;tc[214]:=3;tc[215]:=3;
  tc[216]:=3;tc[217]:=3;tc[218]:=5;tc[219]:=4;tc[220]:=8;tc[221]:=6;tc[222]:=8;tc[223]:=1;
  tc[224]:=6;tc[225]:=1;tc[226]:=8;tc[227]:=3;tc[228]:=1;tc[229]:=6;tc[230]:=3;tc[231]:=8;
  tc[232]:=7;tc[233]:=8;tc[234]:=8;tc[235]:=3;tc[236]:=3;tc[237]:=7;tc[238]:=8;tc[239]:=8;
  tc[240]:=8;tc[241]:=8;tc[242]:=8;tc[243]:=8;tc[244]:=8;tc[245]:=4;tc[246]:=8;tc[247]:=5;
  tc[248]:=4;tc[249]:=5;tc[250]:=6;tc[251]:=1;tc[252]:=6;tc[253]:=6;tc[254]:=6;tc[255]:=1
END InitTileColors;

PROCEDURE GetTileMapColor(tileByte: INTEGER; VAR r, g, b: INTEGER);
VAR idx: INTEGER;
BEGIN
  IF (tileByte < 0) OR (tileByte > 255) THEN
    r := 0; g := 0; b := 0;
    RETURN
  END;
  idx := tc[tileByte];
  r := palR[idx]; g := palG[idx]; b := palB[idx]
END GetTileMapColor;

PROCEDURE InitDebugMap;
BEGIN
  dbgWin := NIL;
  dbgRen := NIL;
  open := FALSE;
  needRedraw := TRUE;
  lastDrawnRegion := -1;
  worldLoaded := FALSE;
  InitPalette;
  InitTileColors
END InitDebugMap;

PROCEDURE IsOpen(): BOOLEAN;
BEGIN
  RETURN open
END IsOpen;

PROCEDURE ToggleDebugMap;
BEGIN
  IF open THEN
    IF dbgRen # NIL THEN DestroyRenderer(dbgRen); dbgRen := NIL END;
    IF dbgWin # NIL THEN DestroyWindow(dbgWin); dbgWin := NIL END;
    open := FALSE
  ELSE
    IF NOT worldLoaded THEN LoadWorldData END;
    dbgWin := CreateWindow("Debug Map — Full World",
                           WinW, WinH, WIN_CENTERED);
    IF dbgWin # NIL THEN
      dbgRen := CreateRenderer(dbgWin, RENDER_ACCELERATED);
      IF dbgRen # NIL THEN
        open := TRUE;
        needRedraw := TRUE
      ELSE
        DestroyWindow(dbgWin); dbgWin := NIL
      END
    END
  END
END ToggleDebugMap;

PROCEDURE GetWorldTile(tileX, tileY: INTEGER): INTEGER;
VAR mapRow, secx, secy, secNum, offset: INTEGER;
BEGIN
  (* Which map row? 4 rows of 256 tiles tall each *)
  mapRow := tileY DIV 256;
  IF (mapRow < 0) OR (mapRow > 3) THEN RETURN 19 END; (* water *)

  secx := (tileX DIV 16) MOD 128;
  secy := (tileY DIV 8) MOD 32;
  offset := secy * 128 + secx;
  IF (offset < 0) OR (offset >= 4096) THEN RETURN 19 END;

  secNum := ORD(wMap[mapRow][offset]);
  offset := secNum * 128 + (tileY MOD 8) * 16 + (tileX MOD 16);
  IF (offset < 0) OR (offset >= 32768) THEN RETURN 19 END;
  RETURN ORD(wSector[offset])
END GetWorldTile;

PROCEDURE DrawFullMap;
VAR tx, ty, tileByte, idx, px, py, r, g, b: INTEGER;
BEGIN
  IF dbgRen = NIL THEN RETURN END;

  SetColor(dbgRen, 0, 0, 85, 255); (* dark blue background = ocean *)
  CanvasClear(dbgRen);

  FOR ty := 0 TO FullH - 1 DO
    FOR tx := 0 TO FullW - 1 DO
      tileByte := GetWorldTile(tx, ty);
      idx := tc[tileByte];
      r := palR[idx]; g := palG[idx]; b := palB[idx];

      px := tx * WinW DIV FullW;
      py := ty * WinH DIV FullH;
      SetColor(dbgRen, r, g, b, 255);
      FillRect(dbgRen, px, py,
               WinW DIV FullW + 1, WinH DIV FullH + 1)
    END
  END;

  needRedraw := FALSE
END DrawFullMap;

PROCEDURE DrawPlayerDot;
VAR imx, imy, dotX, dotY: INTEGER;
BEGIN
  IF dbgRen = NIL THEN RETURN END;

  imx := actors[0].absX DIV 16;
  imy := actors[0].absY DIV 32;
  dotX := imx * WinW DIV FullW;
  dotY := imy * WinH DIV FullH;

  IF (cycle DIV 10) MOD 2 = 0 THEN
    SetColor(dbgRen, 255, 255, 255, 255)
  ELSE
    SetColor(dbgRen, 255, 0, 0, 255)
  END;
  FillRect(dbgRen, dotX - 3, dotY - 3, 7, 7)
END DrawPlayerDot;

PROCEDURE UpdateDebugMap;
BEGIN
  IF NOT open THEN RETURN END;
  IF dbgRen = NIL THEN RETURN END;

  IF needRedraw THEN
    DrawFullMap;
    DrawPlayerDot;
    Present(dbgRen)
  ELSIF (cycle MOD 20) = 0 THEN
    DrawFullMap;
    DrawPlayerDot;
    Present(dbgRen)
  END
END UpdateDebugMap;

END DebugMap.
