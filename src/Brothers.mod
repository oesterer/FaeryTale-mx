IMPLEMENTATION MODULE Brothers;

FROM Strings IMPORT Assign;
FROM Actor IMPORT actors;
FROM World IMPORT TileSize;

PROCEDURE ClearInventory(VAR b: BrotherData);
VAR i: INTEGER;
BEGIN
  FOR i := 0 TO 5 DO b.weaponInv[i] := 0 END;
  FOR i := 0 TO 34 DO b.stuff[i] := 0 END
END ClearInventory;

PROCEDURE HasStuff(idx: INTEGER): BOOLEAN;
BEGIN
  IF (idx >= 0) AND (idx <= 34) THEN
    RETURN brothers[activeBrother].stuff[idx] > 0
  END;
  RETURN FALSE
END HasStuff;

PROCEDURE HasWeapon(idx: INTEGER): BOOLEAN;
BEGIN
  (* weaponInv indices 1-5 map to stuff[0-4] *)
  IF (idx >= 1) AND (idx <= 5) THEN
    RETURN brothers[activeBrother].stuff[idx - 1] > 0
  END;
  RETURN FALSE
END HasWeapon;

PROCEDURE GiveStuff(idx: INTEGER);
BEGIN
  IF (idx >= 0) AND (idx <= 34) THEN
    INC(brothers[activeBrother].stuff[idx])
  END
END GiveStuff;

PROCEDURE AddStuffN(idx, n: INTEGER);
BEGIN
  IF (idx >= 0) AND (idx <= 34) THEN
    INC(brothers[activeBrother].stuff[idx], n)
  END
END AddStuffN;

PROCEDURE SetStuff(idx, val: INTEGER);
BEGIN
  IF (idx >= 0) AND (idx <= 34) THEN
    brothers[activeBrother].stuff[idx] := val
  END
END SetStuff;

PROCEDURE AddWealth(amount: INTEGER);
BEGIN
  INC(brothers[activeBrother].wealth, amount)
END AddWealth;

PROCEDURE IncBrave;
BEGIN
  INC(brothers[activeBrother].brave)
END IncBrave;

PROCEDURE DecLuck(amount: INTEGER);
BEGIN
  DEC(brothers[activeBrother].luck, amount);
  IF brothers[activeBrother].luck < 0 THEN
    brothers[activeBrother].luck := 0
  END
END DecLuck;

PROCEDURE DecKind(amount: INTEGER);
BEGIN
  DEC(brothers[activeBrother].kind, amount);
  IF brothers[activeBrother].kind < 0 THEN
    brothers[activeBrother].kind := 0
  END
END DecKind;

PROCEDURE IncKind;
BEGIN
  INC(brothers[activeBrother].kind)
END IncKind;

PROCEDURE InitBrothers;
BEGIN
  activeBrother := Julian;

  Assign("Julian", brothers[Julian].name);
  brothers[Julian].vitality := 200;  (* 15 + 35/4 *)
  brothers[Julian].weapon := 3;
  brothers[Julian].brave := 150;
  brothers[Julian].luck := 20;
  brothers[Julian].kind := 15;
  brothers[Julian].wealth := 200;
  ClearInventory(brothers[Julian]);
  brothers[Julian].stuff[0] := 1;  (* starts with dirk *)
  brothers[Julian].stuff[2] := 1;  (* starts with dirk *)
  brothers[Julian].stuff[11] := 6; (* Glass Vials *)
  brothers[Julian].stuff[13] := 4; (* Bird Totems *)
  brothers[Julian].startX := 19036;
  brothers[Julian].startY := 15755;
  brothers[Julian].alive := TRUE;

  Assign("Philip", brothers[Philip].name);
  brothers[Philip].vitality := 20;  (* 15 + 20/4 *)
  brothers[Philip].weapon := 0;
  brothers[Philip].brave := 20;
  brothers[Philip].luck := 35;
  brothers[Philip].kind := 15;
  brothers[Philip].wealth := 15;
  ClearInventory(brothers[Philip]);
  brothers[Philip].startX := 19036;
  brothers[Philip].startY := 15755;
  brothers[Philip].alive := TRUE;

  Assign("Kevin", brothers[Kevin].name);
  brothers[Kevin].vitality := 18;  (* 15 + 15/4 *)
  brothers[Kevin].weapon := 0;
  brothers[Kevin].brave := 15;
  brothers[Kevin].luck := 20;
  ClearInventory(brothers[Kevin]);
  brothers[Kevin].kind := 35;
  brothers[Kevin].wealth := 10;
  brothers[Kevin].startX := 19036;
  brothers[Kevin].startY := 15755;
  brothers[Kevin].alive := TRUE
END InitBrothers;

PROCEDURE SaveBrotherState;
BEGIN
  brothers[activeBrother].vitality := actors[0].vitality;
  brothers[activeBrother].weapon := actors[0].weapon
END SaveBrotherState;

PROCEDURE RestoreBrotherState;
BEGIN
  actors[0].absX := brothers[activeBrother].startX;
  actors[0].absY := brothers[activeBrother].startY;
  actors[0].vitality := brothers[activeBrother].vitality;
  actors[0].weapon := brothers[activeBrother].weapon;
  actors[0].state := 13; (* StStill *)
  actors[0].facing := 4  (* south *)
END RestoreBrotherState;

PROCEDURE SwitchToNext(): BOOLEAN;
VAR i, next: INTEGER;
BEGIN
  brothers[activeBrother].alive := FALSE;

  (* Find next living brother *)
  FOR i := 1 TO NumBrothers DO
    next := (activeBrother + i) MOD NumBrothers;
    IF brothers[next].alive THEN
      activeBrother := next;
      (* New brother starts fresh — clear items, give dirk *)
      ClearInventory(brothers[next]);
      brothers[next].stuff[0] := 1;  (* dirk *)
      brothers[next].weapon := 1;
      RestoreBrotherState;
      RETURN TRUE
    END
  END;
  (* All brothers dead *)
  RETURN FALSE
END SwitchToNext;

PROCEDURE ActiveName(VAR name: ARRAY OF CHAR);
BEGIN
  Assign(brothers[activeBrother].name, name)
END ActiveName;

END Brothers.
