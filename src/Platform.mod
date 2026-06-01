IMPLEMENTATION MODULE Platform;

FROM SYSTEM IMPORT ADDRESS;
FROM Gfx IMPORT Init AS GfxInit, Quit AS GfxQuit,
                CreateWindow, DestroyWindow,
                CreateRenderer, DestroyRenderer,
                Present, Ticks, Delay, SetFullscreen,
                GetWindowWidth, GetWindowHeight, UpdateLogicalSize,
                WIN_CENTERED, WIN_RESIZABLE, FULLSCREEN_OFF,
                FULLSCREEN_DESKTOP, RENDER_ACCELERATED, RENDER_VSYNC;
FROM Events IMPORT Poll, QUIT_EVENT, KEYDOWN, MOUSEDOWN, WINDOW_EVENT,
                   KeyCode, KeyMod, WindowEvent, WEVT_RESIZED,
                   IsKeyPressed, MouseX, MouseY, MouseButton,
                   SCAN_UP, SCAN_DOWN, SCAN_LEFT, SCAN_RIGHT,
                   SCAN_W, SCAN_A, SCAN_S, SCAN_D,
                   SCAN_SPACE, SCAN_P, SCAN_F, SCAN_T,
                   KEY_RETURN, KEY_F11, MOD_ALT;
FROM Canvas IMPORT Clear AS CanvasClear, SetColor;
FROM Texture IMPORT LoadBMP, LoadBMPKeyed, DrawRegion, Tex,
                    Draw AS TexDraw2,
                    Create AS TexCreate, Destroy AS TexDestroy,
                    SetTarget, ResetTarget,
                    Width AS TexWidth, Height AS TexHeight;

CONST
  FrameW = ScreenW * Scale;
  FrameH = ScreenH * Scale;

VAR
  frameTex: Tex;
  fullscreen: BOOLEAN;
  viewX, viewY, viewW, viewH: INTEGER;

PROCEDURE UpdateViewport;
VAR winW, winH: INTEGER;
BEGIN
  winW := GetWindowWidth(win);
  winH := GetWindowHeight(win);
  IF (winW <= 0) OR (winH <= 0) THEN
    viewX := 0; viewY := 0; viewW := FrameW; viewH := FrameH;
    RETURN
  END;
  IF winW * FrameH > winH * FrameW THEN
    viewH := winH;
    viewW := winH * FrameW DIV FrameH
  ELSE
    viewW := winW;
    viewH := winW * FrameH DIV FrameW
  END;
  viewX := (winW - viewW) DIV 2;
  viewY := (winH - viewH) DIV 2
END UpdateViewport;

PROCEDURE ToggleFullscreen;
BEGIN
  fullscreen := NOT fullscreen;
  IF fullscreen THEN
    SetFullscreen(win, FULLSCREEN_DESKTOP)
  ELSE
    SetFullscreen(win, FULLSCREEN_OFF)
  END;
  UpdateLogicalSize(ren, win);
  UpdateViewport
END ToggleFullscreen;

PROCEDURE Init(): BOOLEAN;
VAR ok: BOOLEAN;
BEGIN
  ok := GfxInit();
  IF NOT ok THEN RETURN FALSE END;
  win := CreateWindow("Faery Tale Adventure",
                      ScreenW * Scale, ScreenH * Scale,
                      WIN_CENTERED + WIN_RESIZABLE);
  IF win = NIL THEN GfxQuit; RETURN FALSE END;
  ren := CreateRenderer(win, RENDER_ACCELERATED + RENDER_VSYNC);
  IF ren = NIL THEN DestroyWindow(win); GfxQuit; RETURN FALSE END;
  frameTex := TexCreate(ren, FrameW, FrameH);
  IF frameTex = NIL THEN
    DestroyRenderer(ren); DestroyWindow(win); GfxQuit; RETURN FALSE
  END;
  fullscreen := FALSE;
  UpdateViewport;
  RETURN TRUE
END Init;

PROCEDURE Shutdown;
BEGIN
  TexDestroy(frameTex);
  DestroyRenderer(ren);
  DestroyWindow(win);
  GfxQuit
END Shutdown;

PROCEDURE PollInput(VAR inp: InputState);
VAR evt, kc, mx, my: INTEGER;
BEGIN
  inp.menuKey := 0C;
  inp.mouseClick := FALSE;
  LOOP
    evt := Poll();
    IF evt = 0 THEN EXIT END;
    IF evt = QUIT_EVENT THEN inp.quit := TRUE
    ELSIF evt = KEYDOWN THEN
      kc := KeyCode();
      IF (kc = KEY_F11) OR
         ((kc = KEY_RETURN) AND (((KeyMod() DIV MOD_ALT) MOD 2) = 1)) THEN
        ToggleFullscreen
      ELSIF kc = ORD('m') THEN inp.toggleMap := TRUE
      ELSIF kc = ORD('0') THEN inp.menuKey := '0'
      ELSIF kc = ORD('9') THEN inp.menuKey := '9'
      ELSIF kc = ORD('8') THEN inp.menuKey := '8'
      ELSIF kc = 27 THEN inp.menuKey := CHR(27)  (* ESC *)
      ELSIF (kc >= ORD('a')) AND (kc <= ORD('z')) THEN
        inp.menuKey := CAP(CHR(kc))
      END
    ELSIF evt = MOUSEDOWN THEN
      IF MouseButton() = 1 THEN  (* left button *)
        mx := MouseX(); my := MouseY();
        IF (mx >= viewX) AND (mx < viewX + viewW) AND
           (my >= viewY) AND (my < viewY + viewH) THEN
          inp.mouseClick := TRUE;
          inp.mouseX := (mx - viewX) * FrameW DIV viewW;
          inp.mouseY := (my - viewY) * FrameH DIV viewH
        END
      END
    ELSIF (evt = WINDOW_EVENT) AND (WindowEvent() = WEVT_RESIZED) THEN
      UpdateLogicalSize(ren, win);
      UpdateViewport
    END
  END;

  IF IsKeyPressed(SCAN_UP) OR IsKeyPressed(SCAN_W) THEN
    IF IsKeyPressed(SCAN_RIGHT) OR IsKeyPressed(SCAN_D) THEN
      inp.dirKey := DirNE
    ELSIF IsKeyPressed(SCAN_LEFT) OR IsKeyPressed(SCAN_A) THEN
      inp.dirKey := DirNW
    ELSE
      inp.dirKey := DirN
    END
  ELSIF IsKeyPressed(SCAN_DOWN) OR IsKeyPressed(SCAN_S) THEN
    IF IsKeyPressed(SCAN_RIGHT) OR IsKeyPressed(SCAN_D) THEN
      inp.dirKey := DirSE
    ELSIF IsKeyPressed(SCAN_LEFT) OR IsKeyPressed(SCAN_A) THEN
      inp.dirKey := DirSW
    ELSE
      inp.dirKey := DirS
    END
  ELSIF IsKeyPressed(SCAN_RIGHT) OR IsKeyPressed(SCAN_D) THEN
    inp.dirKey := DirE
  ELSIF IsKeyPressed(SCAN_LEFT) OR IsKeyPressed(SCAN_A) THEN
    inp.dirKey := DirW
  ELSE
    inp.dirKey := DirNone
  END;

  inp.attack := IsKeyPressed(SCAN_SPACE);
  inp.usePotion := IsKeyPressed(SCAN_P);
  inp.useFood := IsKeyPressed(SCAN_F);
  inp.talk := IsKeyPressed(SCAN_T)
END PollInput;

PROCEDURE BeginFrame;
BEGIN
  SetTarget(ren, frameTex);
  SetColor(ren, 0, 0, 0, 255);
  CanvasClear(ren)
END BeginFrame;

PROCEDURE EndFrame;
BEGIN
  ResetTarget(ren);
  UpdateViewport;
  SetColor(ren, 0, 0, 0, 255);
  CanvasClear(ren);
  DrawRegion(ren, frameTex, 0, 0, FrameW, FrameH,
             viewX, viewY, viewW, viewH);
  Present(ren)
END EndFrame;

PROCEDURE GetTicks(): INTEGER;
BEGIN
  RETURN Ticks()
END GetTicks;

PROCEDURE DelayMs(ms: INTEGER);
BEGIN
  Delay(ms)
END DelayMs;

PROCEDURE LoadBMPTexture(path: ARRAY OF CHAR): ADDRESS;
BEGIN
  RETURN LoadBMP(ren, path)
END LoadBMPTexture;

PROCEDURE LoadBMPKeyedTexture(path: ARRAY OF CHAR;
                               kr, kg, kb: INTEGER): ADDRESS;
BEGIN
  RETURN LoadBMPKeyed(ren, path, kr, kg, kb)
END LoadBMPKeyedTexture;

PROCEDURE LoadBMPScaled(path: ARRAY OF CHAR; dw, dh: INTEGER): ADDRESS;
VAR src, scaled: Tex;
    sw, sh: INTEGER;
BEGIN
  src := LoadBMP(ren, path);
  IF src = NIL THEN RETURN NIL END;
  sw := TexWidth(src);
  sh := TexHeight(src);
  (* Pre-render source into an RGBA target texture at desired size *)
  scaled := TexCreate(ren, dw, dh);
  IF scaled = NIL THEN RETURN src END;
  SetTarget(ren, scaled);
  SetColor(ren, 0, 0, 0, 0);
  CanvasClear(ren);
  (* Stretch source into the RGBA target at desired size *)
  DrawRegion(ren, src, 0, 0, sw, sh, 0, 0, dw, dh);
  ResetTarget(ren);
  TexDestroy(src);
  RETURN scaled
END LoadBMPScaled;

PROCEDURE DrawTexRegion(tex: ADDRESS;
                        sx, sy, sw, sh: INTEGER;
                        dx, dy, dw, dh: INTEGER);
BEGIN
  DrawRegion(ren, tex, sx, sy, sw, sh, dx, dy, dw, dh)
END DrawTexRegion;

END Platform.
