IMPLEMENTATION MODULE Actor;

PROCEDURE InitActor(VAR a: Actor);
BEGIN
  a.absX := 0; a.absY := 0;
  a.relX := 0; a.relY := 0;
  a.actorType := TypePlayer;
  a.race := 0;
  a.index := 0;
  a.visible := FALSE;
  a.looted := FALSE;
  a.weapon := 0;
  a.environ := 0;
  a.goal := GoalUser;
  a.tactic := 0;
  a.state := StStill;
  a.facing := 4; (* south *)
  a.vitality := 100;
  a.velX := 0;
  a.velY := 0
END InitActor;

PROCEDURE InitAll;
VAR i: INTEGER;
BEGIN
  actorCount := 1;
  FOR i := 0 TO MaxActors - 1 DO
    InitActor(actors[i])
  END
END InitAll;

END Actor.
