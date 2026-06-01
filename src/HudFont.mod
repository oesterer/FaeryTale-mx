IMPLEMENTATION MODULE HudFont;

(* HUD font renderer for the 640x57 hi-res text area.
   Amber (9px proportional) for messages/stats.
   Topaz (8px fixed-width) for menu labels.
   All coordinates in 640x57 HUD space, mapped to screen. *)

FROM SYSTEM IMPORT ADDRESS;
FROM Gfx IMPORT Renderer;
FROM Texture IMPORT LoadBMPKeyed, DrawRegion AS TexDrawRegion, Tex,
                    SetBlendMode, SetColorMod;
FROM Canvas IMPORT BLEND_ALPHA, SetColor, FillRect;
FROM BinaryIO IMPORT OpenRead, Close, ReadByte, Done;
FROM InOut IMPORT WriteString, WriteInt, WriteLn;
FROM Platform IMPORT ScreenW, PlayH, Scale;
FROM Assets IMPORT AssetPath;

CONST
  MaxAmberGlyphs = 96;
  AmberH    = 9;    (* amber source glyph height *)
  TopazW    = 8;    (* topaz fixed glyph width and spacing *)
  TopazH    = 8;    (* topaz source glyph height *)
  TopazNum  = 224;  (* topaz glyph count *)
  TopazLo   = 32;   (* topaz first char *)
  HudW      = 640;  (* original HUD pixel width *)

TYPE
  GlyphRec = RECORD
    locStart: INTEGER;
    bitLen:   INTEGER;
    spacing:  INTEGER
  END;

VAR
  amberTex:    Tex;
  amberLo:     INTEGER;
  amberNum:    INTEGER;
  amberBase:   INTEGER;
  amberGlyphs: ARRAY [0..95] OF GlyphRec;

  topazTex:    Tex;

  scrW:    INTEGER;  (* ScreenW * Scale *)
  hudY0:   INTEGER;  (* PlayH * Scale *)

(* --- Coordinate mapping from 640x57 HUD space to screen --- *)

PROCEDURE MapX(hx: INTEGER): INTEGER;
BEGIN RETURN hx * scrW DIV HudW END MapX;

PROCEDURE MapY(hy: INTEGER): INTEGER;
BEGIN RETURN hudY0 + hy * Scale END MapY;

PROCEDURE MapW(w: INTEGER): INTEGER;
BEGIN RETURN w * scrW DIV HudW END MapW;

PROCEDURE MapH(h: INTEGER): INTEGER;
BEGIN RETURN h * Scale END MapH;

(* --- Font loading --- *)

PROCEDURE LoadAmber(ren: Renderer): BOOLEAN;
VAR fh: CARDINAL;
    i: INTEGER;
    b, lo, hi: CARDINAL;
    p: ARRAY [0..127] OF CHAR;
BEGIN
  amberTex := NIL;
  amberNum := 0;

  AssetPath("amber_9.fnt", p);
  OpenRead(p, fh);
  IF NOT Done THEN
    WriteString("HudFont: cannot open amber_9.fnt"); WriteLn;
    RETURN FALSE
  END;

  ReadByte(fh, b); amberLo := INTEGER(b);
  ReadByte(fh, b); amberNum := INTEGER(b);
  ReadByte(fh, b); amberBase := INTEGER(b);

  IF amberNum > MaxAmberGlyphs THEN amberNum := MaxAmberGlyphs END;

  FOR i := 0 TO amberNum - 1 DO
    ReadByte(fh, lo);
    ReadByte(fh, hi);
    amberGlyphs[i].locStart := INTEGER(lo) + INTEGER(hi) * 256;
    ReadByte(fh, b); amberGlyphs[i].bitLen := INTEGER(b);
    ReadByte(fh, b); amberGlyphs[i].spacing := INTEGER(b)
  END;
  Close(fh);

  AssetPath("amber_9.bmp", p);
  amberTex := LoadBMPKeyed(ren, p, 0, 0, 0);
  IF amberTex = NIL THEN
    WriteString("HudFont: cannot load amber_9.bmp"); WriteLn;
    RETURN FALSE
  END;
  SetBlendMode(amberTex, BLEND_ALPHA);
  (* textcolors pen 10 = 0xA50 → RGB (170, 85, 0) *)
  SetColorMod(amberTex, 170, 85, 0);

  WriteString("HudFont: amber_9 loaded (");
  WriteInt(amberNum, 1); WriteString(" glyphs)"); WriteLn;
  RETURN TRUE
END LoadAmber;

PROCEDURE LoadTopaz(ren: Renderer): BOOLEAN;
VAR p: ARRAY [0..127] OF CHAR;
BEGIN
  AssetPath("topaz_8.bmp", p);
  topazTex := LoadBMPKeyed(ren, p, 0, 0, 0);
  IF topazTex = NIL THEN
    WriteString("HudFont: cannot load topaz_8.bmp"); WriteLn;
    RETURN FALSE
  END;
  SetBlendMode(topazTex, BLEND_ALPHA);
  WriteString("HudFont: topaz_8 loaded"); WriteLn;
  RETURN TRUE
END LoadTopaz;

PROCEDURE LoadHudFont(ren: Renderer): BOOLEAN;
VAR ok: BOOLEAN;
BEGIN
  scrW := ScreenW * Scale;
  hudY0 := PlayH * Scale;
  ok := LoadAmber(ren);
  IF NOT LoadTopaz(ren) THEN ok := FALSE END;
  RETURN ok
END LoadHudFont;

(* --- Amber: message log and stats --- *)

PROCEDURE DrawHudStr(ren: Renderer; s: ARRAY OF CHAR;
                     hx, hy: INTEGER);
VAR i, cx, idx, dw, dh: INTEGER;
BEGIN
  IF amberTex = NIL THEN RETURN END;
  (* Screen-space intro and victory text share this texture and can change its
     modulation. HUD messages and stats always use the original brown pen. *)
  SetColorMod(amberTex, 170, 85, 0);
  cx := hx;
  dh := MapH(AmberH);
  i := 0;
  WHILE (i <= HIGH(s)) AND (s[i] # 0C) DO
    idx := ORD(s[i]) - amberLo;
    IF (idx >= 0) AND (idx < amberNum) THEN
      IF amberGlyphs[idx].bitLen > 0 THEN
        dw := MapW(amberGlyphs[idx].bitLen);
        IF dw < 1 THEN dw := 1 END;
        TexDrawRegion(ren, amberTex,
                      amberGlyphs[idx].locStart, 0,
                      amberGlyphs[idx].bitLen, AmberH,
                      MapX(cx), MapY(hy), dw, dh)
      END;
      INC(cx, amberGlyphs[idx].spacing)
    END;
    INC(i)
  END
END DrawHudStr;

PROCEDURE HudStrWidth(s: ARRAY OF CHAR): INTEGER;
VAR i, w, idx: INTEGER;
BEGIN
  w := 0;
  i := 0;
  WHILE (i <= HIGH(s)) AND (s[i] # 0C) DO
    idx := ORD(s[i]) - amberLo;
    IF (idx >= 0) AND (idx < amberNum) THEN
      INC(w, amberGlyphs[idx].spacing)
    END;
    INC(i)
  END;
  RETURN w
END HudStrWidth;

(* --- Screen-space text (for credits, not HUD) --- *)

PROCEDURE DrawScreenStr(ren: Renderer; s: ARRAY OF CHAR;
                        sx, sy, sc: INTEGER);
VAR i, cx, idx, dw, dh: INTEGER;
BEGIN
  IF amberTex = NIL THEN RETURN END;
  (* Caller is responsible for SetColorMod on amberTex if needed *)
  cx := sx;
  dh := AmberH * sc;
  i := 0;
  WHILE (i <= HIGH(s)) AND (s[i] # 0C) DO
    idx := ORD(s[i]) - amberLo;
    IF (idx >= 0) AND (idx < amberNum) THEN
      IF amberGlyphs[idx].bitLen > 0 THEN
        dw := amberGlyphs[idx].bitLen * sc;
        TexDrawRegion(ren, amberTex,
                      amberGlyphs[idx].locStart, 0,
                      amberGlyphs[idx].bitLen, AmberH,
                      cx, sy, dw, dh)
      END;
      INC(cx, amberGlyphs[idx].spacing * sc)
    END;
    INC(i)
  END
END DrawScreenStr;

PROCEDURE SetFontColor(r, g, b: INTEGER);
BEGIN
  IF amberTex # NIL THEN SetColorMod(amberTex, r, g, b) END
END SetFontColor;

PROCEDURE ResetFontColor;
BEGIN
  (* Original textcolors[10] = 0xA50 = dark brown *)
  IF amberTex # NIL THEN SetColorMod(amberTex, 170, 85, 0) END
END ResetFontColor;

PROCEDURE ScreenStrWidth(s: ARRAY OF CHAR; sc: INTEGER): INTEGER;
VAR i, w, idx: INTEGER;
BEGIN
  w := 0;
  i := 0;
  WHILE (i <= HIGH(s)) AND (s[i] # 0C) DO
    idx := ORD(s[i]) - amberLo;
    IF (idx >= 0) AND (idx < amberNum) THEN
      INC(w, amberGlyphs[idx].spacing * sc)
    END;
    INC(i)
  END;
  RETURN w
END ScreenStrWidth;

(* --- Topaz: menu labels --- *)

PROCEDURE DrawMenuStr(ren: Renderer; s: ARRAY OF CHAR;
                      hx, hy, nchars, textOff: INTEGER;
                      fgR, fgG, fgB, bgR, bgG, bgB: INTEGER);
VAR i, cx, idx, dw, dh, bx, bw, bh: INTEGER;
BEGIN
  IF topazTex = NIL THEN RETURN END;

  (* Draw background rect at button origin *)
  bx := MapX(hx);
  bw := MapW(nchars * TopazW);
  dh := MapH(TopazH);
  bh := dh;
  SetColor(ren, bgR, bgG, bgB, 255);
  FillRect(ren, bx, MapY(hy), bw, bh);

  (* Draw text glyphs at button origin + text inset *)
  SetColorMod(topazTex, fgR, fgG, fgB);
  cx := hx + textOff;
  dw := MapW(TopazW);
  i := 0;
  WHILE (i <= HIGH(s)) AND (s[i] # 0C) DO
    idx := ORD(s[i]) - TopazLo;
    IF (idx >= 0) AND (idx < TopazNum) THEN
      TexDrawRegion(ren, topazTex,
                    idx * TopazW, 0, TopazW, TopazH,
                    MapX(cx), MapY(hy), dw, dh)
    END;
    INC(cx, TopazW);
    INC(i)
  END
END DrawMenuStr;

END HudFont.
