IMPLEMENTATION MODULE Combat;

(* Combat system matching original FTA dohit/checkdead. *)

FROM Actor IMPORT actors, actorCount,
                  TypeEnemy, TypeSetfig, TypeDragon,
                  StFighting, StDying, StDead, StStill, StShoot1,
                  GoalDeath;
FROM Brothers IMPORT brothers, activeBrother, StSunStone, HasStuff,
                    IncBrave, DecLuck, DecKind;
FROM SFX IMPORT PlayEffect, SfxEnemyHit, SfxPlayerHit;
FROM Movement IMPORT MoveActor;
FROM InOut IMPORT WriteString, WriteInt, WriteLn;

VAR
  hitCooldown: ARRAY [0..47] OF INTEGER;  (* per-actor attack timer *)
  rng: INTEGER;

PROCEDURE Rand(limit: INTEGER): INTEGER;
BEGIN
  rng := rng * 1103515245 + 12345;
  IF rng < 0 THEN rng := -rng END;
  IF limit <= 0 THEN RETURN 0 END;
  RETURN (rng DIV 65536) MOD limit
END Rand;

(* --- Central hit function matching original dohit --- *)

PROCEDURE DoHit(attacker, defender: INTEGER);
VAR damage, wt: INTEGER;
    kb: BOOLEAN;
BEGIN
  (* Necromancer (enemy race 9): immune to melee weapons.
     Original: weapon < 4 && race == 9 → blocked *)
  IF (actors[defender].actorType = TypeEnemy) AND
     (actors[defender].race = 9) AND
     (actors[attacker].weapon < 4) THEN
    RETURN  (* must use bow or wand *)
  END;

  (* Witch (setfig race 9): any weapon can damage her. *)

  (* Damage: weapon + bitrand(2) matching original *)
  wt := actors[attacker].weapon;
  IF wt >= 8 THEN wt := 5 END;  (* cap touch attacks *)
  damage := wt + Rand(2);  (* 0 or 1 random bonus *)
  IF damage < 1 THEN damage := 1 END;
  IF (defender = 0) AND (wardTimer > 0) THEN
    damage := (damage + 1) DIV 2
  END;

  DEC(actors[defender].vitality, damage);

  IF defender = 0 THEN
    PlayEffect(SfxPlayerHit)
  ELSE
    PlayEffect(SfxEnemyHit)
  END;

  (* Knockback: push defender 2px in attacker's facing direction.
     Original: move_figure(j,fc,2) + move_figure(i,fc,2) *)
  IF (actors[defender].actorType # TypeSetfig) AND
     (actors[defender].actorType # TypeDragon) THEN
    kb := MoveActor(defender, actors[attacker].facing, 2);
    IF kb AND (attacker >= 0) THEN
      kb := MoveActor(attacker, actors[attacker].facing, 2)
    END
  END;

  (* Check death — matches original checkdead() *)
  IF actors[defender].vitality <= 0 THEN
    actors[defender].vitality := 0;
    actors[defender].state := StDying;
    actors[defender].goal := GoalDeath;
    actors[defender].tactic := 7;  (* death countdown init *)

    IF defender > 0 THEN
      (* Enemy killed: brave++ *)
      IncBrave
    ELSE
      DecLuck(5)
    END;

    IF actors[defender].actorType = TypeSetfig THEN
      DecKind(3)
    END
  END
END DoHit;

(* --- Distance check --- *)

PROCEDURE Dist(a, b: INTEGER; VAR xd, yd: INTEGER);
BEGIN
  xd := actors[a].absX - actors[b].absX;
  yd := actors[a].absY - actors[b].absY;
  IF xd < 0 THEN xd := -xd END;
  IF yd < 0 THEN yd := -yd END
END Dist;

(* --- Facing check --- *)

PROCEDURE IsFacing(attacker, target: INTEGER): BOOLEAN;
VAR dx, dy, f: INTEGER;
BEGIN
  (* Generous facing check — target must be in the forward 180 degrees *)
  dx := actors[target].absX - actors[attacker].absX;
  dy := actors[target].absY - actors[attacker].absY;
  f := actors[attacker].facing;
  CASE f OF
    0: RETURN dy <= 0 |          (* N: anything north *)
    1: RETURN (dx >= 0) OR (dy <= 0) |  (* NE: north or east half *)
    2: RETURN dx >= 0 |          (* E *)
    3: RETURN (dx >= 0) OR (dy >= 0) |  (* SE *)
    4: RETURN dy >= 0 |          (* S *)
    5: RETURN (dx <= 0) OR (dy >= 0) |  (* SW *)
    6: RETURN dx <= 0 |          (* W *)
    7: RETURN (dx <= 0) OR (dy <= 0)    (* NW *)
  ELSE
    RETURN TRUE
  END
END IsFacing;

(* --- Main combat update --- *)

PROCEDURE UpdateCombat;
VAR i, xd, yd, bv: INTEGER;
BEGIN
  (* Decrement all cooldowns *)
  FOR i := 0 TO actorCount - 1 DO
    IF hitCooldown[i] > 0 THEN DEC(hitCooldown[i]) END
  END;

  (* Enemy → Player attacks — skip if player is dead/dying or airborne *)
  IF (actors[0].state # StDead) AND (actors[0].state # StDying) AND
     (actors[0].environ >= 0) THEN
  FOR i := 1 TO actorCount - 1 DO
    IF (actors[i].state = StFighting) AND (hitCooldown[i] = 0) THEN
      Dist(i, 0, xd, yd);
      (* Original: enemy hits if rand256() > brave AND within range *)
      bv := brothers[activeBrother].brave;
      IF (xd < 14) AND (yd < 14) AND (Rand(256) > bv) THEN
        DoHit(i, 0);
        hitCooldown[i] := 12
      ELSIF (xd < 14) AND (yd < 14) THEN
        hitCooldown[i] := 8  (* missed — brief cooldown *)
      END
    END
  END;
  END;  (* player dead/dying check *)

  (* Player → Enemy melee attacks *)
  IF (actors[0].state = StFighting) AND (hitCooldown[0] = 0) THEN
    FOR i := 1 TO actorCount - 1 DO
      IF (actors[i].state # StDead) AND (actors[i].state # StDying) THEN
        Dist(0, i, xd, yd);
        IF (xd < 20) AND (yd < 20) AND IsFacing(0, i) THEN
          DoHit(0, i);
          hitCooldown[0] := 8;
          i := actorCount  (* break *)
        END
      END
    END
  END
END UpdateCombat;

BEGIN
  wardTimer := 0;
  rng := 77777;
  FOR rng := 0 TO 47 DO hitCooldown[rng] := 0 END;
  rng := 77777
END Combat.
