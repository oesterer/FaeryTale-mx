MODULE Main;

FROM InOut IMPORT WriteString, WriteLn;
FROM Platform IMPORT Init, Shutdown, BeginFrame, EndFrame,
                    GetTicks, DelayMs, ren;
FROM GameState IMPORT InitGame, UpdateGame, running, FrameTime,
                      mapToggled, viewStatus, ViewInventory, ViewBird;
FROM Render IMPORT InitOverlay, DrawWorld, DrawItems, DrawActors,
                   DrawHUD, DrawMenu, DrawMessage,
                   DrawInventory, DrawBirdView, DrawFairy, DrawWitchBeam,
                   LoadCompass;
FROM DebugMap IMPORT InitDebugMap, ToggleDebugMap, UpdateDebugMap;
FROM Menu IMPORT InitMenus;
FROM HudFont IMPORT LoadHudFont;
FROM Compass IMPORT InitCompass, DrawCompass;
FROM Actor IMPORT actors;
FROM Music IMPORT InitMusic, UpdateMusic, ShutdownMusic;
FROM Intro IMPORT RunIntro;
FROM WorldObj IMPORT InitWorldObjects, LoadObjectSprites, DrawWorldObjects;
FROM Missile IMPORT DrawMissiles;
FROM SFX IMPORT InitSFX, ShutdownSFX;

VAR
  frameStart, elapsed: INTEGER;

BEGIN
  WriteString("Faery Tale Adventure - Modula-2 reimplementation"); WriteLn;
  WriteString("  WASD/Arrows=move  Space=attack  M=debug map"); WriteLn;

  IF NOT Init() THEN
    WriteString("Failed to initialize platform"); WriteLn;
    HALT
  END;

  InitMenus;
  InitOverlay;
  InitGame;
  InitCompass(ren);
  LoadCompass;
  IF NOT LoadHudFont(ren) THEN
    WriteString("Warning: HUD font load failed"); WriteLn
  END;
  InitWorldObjects;
  LoadObjectSprites;
  IF NOT InitMusic() THEN
    WriteString("Warning: music init failed"); WriteLn
  END;
  IF NOT InitSFX() THEN
    WriteString("Warning: SFX init failed"); WriteLn
  END;
  InitDebugMap;

  RunIntro;

  WHILE running DO
    frameStart := GetTicks();

    UpdateGame;
    UpdateMusic;

    IF mapToggled THEN ToggleDebugMap END;

    BeginFrame;
    IF viewStatus = ViewInventory THEN
      DrawInventory
    ELSIF viewStatus = ViewBird THEN
      DrawBirdView
    ELSE
      DrawWorld;
      DrawWorldObjects;
      DrawItems;
      DrawActors;
      DrawFairy;
      DrawWitchBeam;
      DrawMissiles
    END;
    DrawHUD;
    IF actors[0].state = 12 THEN  (* StWalking *)
      DrawCompass(ren, actors[0].facing)
    ELSE
      DrawCompass(ren, -1)  (* neutral — no direction highlight *)
    END;
    DrawMenu;
    DrawMessage;
    EndFrame;

    UpdateDebugMap;

    elapsed := GetTicks() - frameStart;
    IF elapsed < FrameTime THEN
      DelayMs(FrameTime - elapsed)
    END
  END;

  ShutdownSFX;
  ShutdownMusic;
  Shutdown
END Main.
