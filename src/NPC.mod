IMPLEMENTATION MODULE NPC;

(* NPC / set-figure system matching original FTA.
   Data from setfig_table[] and speeches[] in the original. *)

FROM Strings IMPORT Assign;
FROM Actor IMPORT actors, actorCount, MaxActors,
                  TypeSetfig, StStill, StDead, GoalWait, GoalStand;
FROM WorldObj IMPORT objects, objCount, AddObj, MaxWorldObjs;
FROM Assets IMPORT currentRegion;
FROM Brothers IMPORT brothers, activeBrother,
                    StWrit, StBone, StShard, StStatue, StSunStone,
                    HasStuff, GiveStuff, SetStuff, AddWealth, IncKind;

TYPE
  SetfigDef = RECORD
    spriteBank: INTEGER;
    imageBase:  INTEGER;
    canTalk:    BOOLEAN
  END;

VAR
  sfTable: ARRAY [0..18] OF SetfigDef;

  (* Track which WorldObj indices are currently materialized as actors *)
  materialized: ARRAY [0..MaxWorldObjs - 1] OF BOOLEAN;

  (* One-time reward flags *)
  priestStatueGiven:    BOOLEAN;
  sorceressVisited: BOOLEAN;
  darkChant: INTEGER;
  rng: INTEGER;

  (* Speech table — transcribed from original narr.c speeches[] *)
  speeches: ARRAY [0..68] OF ARRAY [0..255] OF CHAR;

(* --- Setfig table init --- *)

PROCEDURE InitSetfigTable;
BEGIN
  sfTable[0].spriteBank := 0; sfTable[0].imageBase := 0; sfTable[0].canTalk := TRUE;
  sfTable[1].spriteBank := 0; sfTable[1].imageBase := 4; sfTable[1].canTalk := TRUE;
  sfTable[2].spriteBank := 1; sfTable[2].imageBase := 0; sfTable[2].canTalk := FALSE;
  sfTable[3].spriteBank := 1; sfTable[3].imageBase := 1; sfTable[3].canTalk := FALSE;
  sfTable[4].spriteBank := 1; sfTable[4].imageBase := 2; sfTable[4].canTalk := FALSE;
  sfTable[5].spriteBank := 1; sfTable[5].imageBase := 4; sfTable[5].canTalk := TRUE;
  sfTable[6].spriteBank := 1; sfTable[6].imageBase := 6; sfTable[6].canTalk := FALSE;
  sfTable[7].spriteBank := 1; sfTable[7].imageBase := 7; sfTable[7].canTalk := FALSE;
  sfTable[8].spriteBank := 2; sfTable[8].imageBase := 0; sfTable[8].canTalk := FALSE;
  sfTable[9].spriteBank := 3; sfTable[9].imageBase := 0; sfTable[9].canTalk := FALSE;
  sfTable[10].spriteBank := 3; sfTable[10].imageBase := 6; sfTable[10].canTalk := FALSE;
  sfTable[11].spriteBank := 3; sfTable[11].imageBase := 7; sfTable[11].canTalk := FALSE;
  sfTable[12].spriteBank := 4; sfTable[12].imageBase := 0; sfTable[12].canTalk := TRUE;
  sfTable[13].spriteBank := 4; sfTable[13].imageBase := 4; sfTable[13].canTalk := TRUE;
  (* Prayer skeletons render from the enemy sprite sheet in Render.mod. *)
  sfTable[14].spriteBank := 0; sfTable[14].imageBase := 0; sfTable[14].canTalk := TRUE;
  (* The circle's dark priest reuses the temple-priest sprite. *)
  sfTable[15].spriteBank := 0; sfTable[15].imageBase := 4; sfTable[15].canTalk := TRUE;
  (* The herb merchant reuses the wizard sprite. *)
  sfTable[16].spriteBank := 0; sfTable[16].imageBase := 0; sfTable[16].canTalk := TRUE;
  sfTable[17].spriteBank := 0; sfTable[17].imageBase := 4; sfTable[17].canTalk := TRUE;
  sfTable[18].spriteBank := 4; sfTable[18].imageBase := 0; sfTable[18].canTalk := TRUE
END InitSetfigTable;

PROCEDURE GetSetfigSprite(race: INTEGER; VAR bank, frame: INTEGER);
BEGIN
  IF (race >= 0) AND (race < MaxNPCs) THEN
    bank := sfTable[race].spriteBank;
    frame := sfTable[race].imageBase
  ELSE
    bank := 0;
    frame := 0
  END
END GetSetfigSprite;

(* --- Speech table init --- *)

PROCEDURE InitSpeeches;
BEGIN
  (* Exact text from original narr.c speeches[] *)
  Assign("% attempted to communicate with the Ogre but a guttural snarl was the only response.", speeches[0]);
  Assign('"Human must die!" said the goblin-man.', speeches[1]);
  Assign('"Doom!" wailed the wraith.', speeches[2]);
  Assign("A clattering of bones was the only reply.", speeches[3]);
  Assign("% knew that it is a waste of time to talk to a snake.", speeches[4]);
  Assign("...", speeches[5]);
  Assign("There was no reply.", speeches[6]);
  Assign('"Die, foolish mortal!" he said.', speeches[7]);
  Assign('"No need to shout, son!" he said.', speeches[8]);
  Assign("Nice weather we're having, isn't it? queried the ranger.", speeches[9]);
  Assign('"Good luck, sonny!" said the ranger. "Hope you win!"', speeches[10]);
  Assign('"If you need to cross the lake" said the ranger, "There is a raft just north of here."', speeches[11]);
  Assign('"Would you like to buy something?" said the tavern keeper. "Or do you just need lodging for the night?"', speeches[12]);
  Assign('"Good Morning." said the tavern keeper. "Hope you slept well."', speeches[13]);
  Assign('"Have a drink!" said the tavern keeper.', speeches[14]);
  Assign('"State your business!" said the guard. "My business is with the king." stated %, respectfully.', speeches[15]);
  Assign('"Please, sir, rescue me from this horrible prison!" pleaded the princess.', speeches[16]);
  Assign('"I cannot help you, young man." said the king. "My armies are decimated, and I fear that with the loss of my children, I have lost all hope."', speeches[17]);
  Assign('"Here is a writ designating you as my official agent. Be sure and show this to the Priest before you leave Marheim."', speeches[18]);
  Assign('"I am afraid I cannot help you, young man. I already gave the golden statue to the other young man."', speeches[19]);
  Assign("If you could rescue the king's daughter, said Lord Trane, The King's courage would be restored.", speeches[20]);
  Assign('"Sorry, I have no use for it."', speeches[21]);
  Assign("The dragon's cave is directly north of here. said the ranger.", speeches[22]);
  Assign('"Alms! Alms for the poor!"', speeches[23]);
  Assign("I have a prophecy for you, m'lord. said the beggar. You must seek two women, one Good, one Evil.", speeches[24]);
  Assign('"Lovely Jewels, glint in the night - give to us the gift of Sight!" he said.', speeches[25]);
  Assign('"Where is the hidden city? How can you find it when you cannot even see it?" said the beggar.', speeches[26]);
  Assign('"Kind deeds could gain thee a friend from the sea."', speeches[27]);
  Assign('"Seek the place that is darker than night - There you shall find your goal in sight!" said the wizard, cryptically.', speeches[28]);
  Assign('"Like the eye itself, a crystal Orb can help to find things concealed."', speeches[29]);
  Assign('"The Witch lives in the dim forest of Grimwood, where the very trees are warped to her will. Her gaze is Death!"', speeches[30]);
  Assign("Only the light of the Sun can destroy the Witch's Evil.", speeches[31]);
  Assign('"The maiden you seek lies imprisoned in an unreachable castle surrounded by unclimbable mountains."', speeches[32]);
  Assign('"Tame the golden beast and no mountain may deny you! But what rope could hold such a creature?"', speeches[33]);
  Assign('"Just what I needed!" he said.', speeches[34]);
  Assign('"Away with you, young ruffian!" said the Wizard. "Perhaps you can find some small animal to torment if that pleases you!"', speeches[35]);
  Assign('"You must seek your enemy on the spirit plane. It is hazardous in the extreme. Space may twist, and time itself may run backwards!"', speeches[36]);
  Assign('"When you wish to travel quickly, seek the power of the Stones." he said.', speeches[37]);
  Assign('"Since you are brave of heart, I shall Heal all your wounds." Instantly % felt much better.', speeches[38]);
  Assign("Ah! You have a writ from the king. Here is one of the golden statues of Azal-Car-Ithil. Find all five and you'll find the vanishing city.", speeches[39]);
  Assign('"Repent, Sinner! Thou art an uncouth brute and I have no interest in your conversation!"', speeches[40]);
  Assign('"Ho there, young traveler!" said the black figure. "None may enter the sacred shrine of the People who came Before!"', speeches[41]);
  Assign('"Your prowess in battle is great." said the Knight of Dreams. "You have earned the right to enter and claim the prize."', speeches[42]);
  Assign('"So this is the so-called Hero who has been sent to hinder my plans. Simply Pathetic. Well, try this, young Fool!"', speeches[43]);
  Assign("% gasped. The Necromancer had been transformed into a normal man. All of his evil was gone.", speeches[44]);
  Assign('"Welcome. Here is one of the five golden figurines you will need." "Thank you." said %.', speeches[45]);
  Assign('"Look into my eyes and Die!!" hissed the witch. "Not a chance!" replied %', speeches[46]);
  Assign("The Spectre spoke. HE has usurped my place as lord of undead. Bring me bones of the ancient King and I'll help you destroy him.", speeches[47]);
  Assign('% gave him the ancient bones. "Good! That spirit now rests quietly in my halls. Take this crystal shard."', speeches[48]);
  Assign('"%..." said the apparition. "I am the ghost of your dead brother. Find my bones -- there you will find some things you need.', speeches[49]);
  Assign('% gave him some gold coins. "Why, thank you, young sir!"', speeches[50]);
  Assign('"Sorry, but I have nothing to sell."', speeches[51]);
  speeches[52][0] := 0C;
  Assign("The dragon's cave is east of here. said the ranger.", speeches[53]);
  Assign("The dragon's cave is west of here. said the ranger.", speeches[54]);
  Assign("The dragon's cave is south of here. said the ranger.", speeches[55]);
  Assign('"Oh, thank you for saving my eggs, kind man!" said the turtle. "Take this seashell as a token of my gratitude."', speeches[56]);
  Assign('"Just hop on my back if you need a ride somewhere." said the turtle.', speeches[57]);
  Assign("Stupid fool, you can't hurt me with that!", speeches[58]);
  Assign("Your magic won't work here, fool!", speeches[59]);
  Assign("The Sunstone has made the witch vulnerable!", speeches[60]);
  Assign('"Ohm Ohm!"', speeches[61]);
  Assign('"Umbra, bind the wandering soul."', speeches[62]);
  Assign('"Blood of night, awaken beneath the stones."', speeches[63]);
  Assign('"Let the hollow stars drink the fading light."', speeches[64]);
  Assign('"By bone and shadow, the sealed gate stirs."', speeches[65]);
  Assign('"Roots that dream, leaves that whisper, blood that remembers. My little garden has answers for those carrying gold."', speeches[66]);
  Assign('"Ink and parchment preserve powers that the cautious may purchase."', speeches[67]);
  Assign('"Fresh apples for the road, traveler."', speeches[68])
END InitSpeeches;

(* --- Materialization --- *)

PROCEDURE MaterializeNPCs(heroX, heroY, region: INTEGER);
VAR i, dx, dy, idx, race, seq: INTEGER;
BEGIN
  seq := 0;  (* sequential index per NPC — used as goal for speech variation *)
  FOR i := 0 TO objCount - 1 DO
    IF (objects[i].status = 3) AND
       ((objects[i].region = region) OR (objects[i].region = -1)) THEN
      dx := heroX - objects[i].x;
      dy := heroY - objects[i].y;
      IF dx < 0 THEN dx := -dx END;
      IF dy < 0 THEN dy := -dy END;

      IF (dx < 400) AND (dy < 400) THEN
        (* Close enough — materialize if not already *)
        IF NOT materialized[i] THEN
          IF actorCount < MaxActors THEN
            idx := actorCount;
            race := objects[i].objId;
            IF race >= MaxNPCs THEN race := 0 END;  (* clamp to setfig table *)
            actors[idx].absX := objects[i].x;
            actors[idx].absY := objects[i].y;
            actors[idx].actorType := TypeSetfig;
            actors[idx].race := race;
            actors[idx].state := StStill;
            actors[idx].goal := seq;
            actors[idx].vitality := 2 + race + race;  (* original: 2+id+id *)
            actors[idx].weapon := 0;
            actors[idx].facing := 4;  (* south by default *)
            actors[idx].visible := TRUE;
            actors[idx].environ := 0;
            actors[idx].tactic := 0;
            actors[idx].velX := 0;
            actors[idx].velY := 0;
            INC(actorCount);
            materialized[i] := TRUE;
          END
        END
      END;
      INC(seq)
    END
  END
END MaterializeNPCs;

(* --- Interaction --- *)

PROCEDURE FindNearestNPC(heroX, heroY: INTEGER): INTEGER;
VAR i, dx, dy, bestDist, dist, bestIdx: INTEGER;
BEGIN
  bestDist := 9999;
  bestIdx := -1;
  FOR i := 1 TO actorCount - 1 DO
    IF (actors[i].actorType = TypeSetfig) AND
       (actors[i].state # StDead) THEN
      dx := heroX - actors[i].absX;
      dy := heroY - actors[i].absY;
      IF dx < 0 THEN dx := -dx END;
      IF dy < 0 THEN dy := -dy END;
      dist := dx + dy;
      IF (dx < 40) AND (dy < 40) AND (dist < bestDist) THEN
        bestDist := dist;
        bestIdx := i;
        (* Face toward player *)
        IF heroX - actors[i].absX > 5 THEN actors[i].facing := 2
        ELSIF heroX - actors[i].absX < -5 THEN actors[i].facing := 6
        ELSIF heroY - actors[i].absY > 5 THEN actors[i].facing := 4
        ELSIF heroY - actors[i].absY < -5 THEN actors[i].facing := 0
        END
      END
    END
  END;
  RETURN bestIdx
END FindNearestNPC;

PROCEDURE NpcName(race: INTEGER; VAR name: ARRAY OF CHAR);
BEGIN
  CASE race OF
    0: Assign("a wizard", name) |
    1: Assign("a priest", name) |
    2, 3: Assign("a guard", name) |
    4: Assign("the princess", name) |
    5: Assign("the king", name) |
    6: Assign("a noble", name) |
    7: Assign("the sorceress", name) |
    8: Assign("the tavern keeper", name) |
    9: Assign("the witch", name) |
   10: Assign("a spectre", name) |
   11: Assign("a ghost", name) |
   12: Assign("a ranger", name) |
   13: Assign("a beggar", name) |
   14: Assign("a praying skeleton", name) |
   15: Assign("a dark priest", name) |
   16: Assign("a mysterious herb wizard", name) |
   17: Assign("a scroll priest", name) |
   18: Assign("an apple ranger", name)
  ELSE
    Assign("someone", name)
  END
END NpcName;

PROCEDURE LookAtNPC(heroX, heroY: INTEGER; VAR desc: ARRAY OF CHAR): BOOLEAN;
VAR idx: INTEGER;
BEGIN
  idx := FindNearestNPC(heroX, heroY);
  IF idx >= 0 THEN
    NpcName(actors[idx].race, desc);
    RETURN TRUE
  END;
  RETURN FALSE
END LookAtNPC;

(* Select speech and apply side effects based on NPC race.
   Returns speech index. Modifies inventory for one-time rewards. *)
PROCEDURE SelectSpeech(actorIdx: INTEGER): INTEGER;
VAR race, goal, kind, i: INTEGER;
BEGIN
  race := actors[actorIdx].race;
  goal := actors[actorIdx].goal;
  kind := brothers[activeBrother].kind;

  CASE race OF
    0:  (* wizard — goal-based hints if kind enough *)
      IF kind < 10 THEN RETURN 35
      ELSE RETURN 27 + (goal MOD 7)
      END |
    1:  (* priest — Writ gates statue reward *)
      IF HasStuff(StWrit) AND (NOT priestStatueGiven) THEN
        priestStatueGiven := TRUE;
        GiveStuff(StStatue);
        RETURN 39
      ELSIF priestStatueGiven THEN
        RETURN 37
      ELSIF kind < 10 THEN
        RETURN 40  (* "Repent, Sinner!" *)
      ELSE
        actors[0].vitality := 15 + brothers[activeBrother].brave DIV 4;
        RETURN 36 + (goal MOD 3)
      END |
    2, 3: RETURN 15 |  (* guard *)
    4:  RETURN 16 |    (* princess *)
    5:  RETURN 17 |    (* king *)
    6:  RETURN 20 |    (* noble *)
    7:  (* sorceress — first visit: speech 45 + set flag.
          Repeat visits: luck += 5 if luck < rand(64). *)
      IF NOT sorceressVisited THEN
        sorceressVisited := TRUE;
        (* Original: ob_listg[9].ob_stat = 1 — makes gold statue
           appear on ground near sorceress for player to Take *)
        FOR i := 0 TO objCount - 1 DO
          IF (objects[i].x = 12025) AND (objects[i].y = 37639) AND
             (objects[i].objId = 149) THEN
            objects[i].status := 1
          END
        END;
        RETURN 45
      ELSE
        rng := rng * 1103515245 + 12345;
        IF rng < 0 THEN rng := -rng END;
        IF brothers[activeBrother].luck < (rng DIV 65536) MOD 64 THEN
          INC(brothers[activeBrother].luck, 5)
        END;
        RETURN -1  (* no speech, just luck boost *)
      END |
    8:  RETURN 12 |    (* bartender — "buy something?" *)
    9:  RETURN 46 |    (* witch *)
   10:  RETURN 47 |    (* spectre *)
   11:  RETURN 49 |    (* ghost *)
   12:  (* ranger — region-based *)
      IF currentRegion = 2 THEN RETURN 22
      ELSE RETURN 53 + (goal MOD 3)
      END |
   13:  RETURN 23 |    (* beggar *)
   14:  RETURN 61 |    (* praying skeleton *)
   15:  i := 62 + (darkChant MOD 4);
        INC(darkChant);
        RETURN i |     (* dark priest *)
   16:  RETURN 66 |    (* herb merchant *)
   17:  RETURN 67 |    (* scroll merchant *)
   18:  RETURN 68      (* apple merchant *)
  ELSE
    RETURN 49
  END
END SelectSpeech;

PROCEDURE TalkToNPC(heroX, heroY: INTEGER; VAR speech: ARRAY OF CHAR): BOOLEAN;
VAR idx, race, speechIdx: INTEGER;
BEGIN
  idx := FindNearestNPC(heroX, heroY);
  IF idx < 0 THEN RETURN FALSE END;
  speechIdx := SelectSpeech(idx);
  IF speechIdx < 0 THEN
    speech[0] := 0C;  (* no speech — silent interaction like luck boost *)
  ELSIF speechIdx < MaxSpeeches THEN
    Assign(speeches[speechIdx], speech)
  ELSE
    Assign("...", speech)
  END;
  RETURN TRUE
END TalkToNPC;

PROCEDURE GiveToNPC(heroX, heroY, itemIdx: INTEGER;
                    VAR response: ARRAY OF CHAR): BOOLEAN;
VAR idx, race: INTEGER;
BEGIN
  idx := FindNearestNPC(heroX, heroY);
  IF idx < 0 THEN RETURN FALSE END;
  race := actors[idx].race;

  (* Gold (itemIdx 0 = menu slot 5 "Gold") *)
  IF itemIdx = 0 THEN
    IF brothers[activeBrother].wealth <= 2 THEN
      Assign("Not enough gold.", response);
      RETURN TRUE
    END;
    AddWealth(-2);
    IncKind;
    IF race = 13 THEN  (* beggar — prophecy based on goal *)
      GetSpeech(24 + actors[idx].goal MOD 4, response)
    ELSE
      GetSpeech(49, response)   (* generic response *)
    END;
    RETURN TRUE
  END;

  (* Bone (itemIdx 3 = menu slot 8 "Bone") *)
  IF (itemIdx = 3) AND HasStuff(StBone) THEN
    IF race = 10 THEN  (* spectre *)
      SetStuff(StBone, 0);
      AddObj(actors[idx].absX, actors[idx].absY, 140, 1, -1);
      GetSpeech(48, response);  (* "% gave him the ancient bones." *)
    ELSE
      GetSpeech(21, response)   (* "Sorry, I have no use for it." *)
    END;
    RETURN TRUE
  END;

  (* Book and Writ: not implemented in original either *)
  Assign("", response);
  RETURN TRUE
END GiveToNPC;

PROCEDURE GetSpeech(idx: INTEGER; VAR text: ARRAY OF CHAR);
BEGIN
  IF (idx >= 0) AND (idx < MaxSpeeches) THEN
    Assign(speeches[idx], text)
  ELSE
    Assign("...", text)
  END
END GetSpeech;

PROCEDURE ResetMaterialized;
VAR i: INTEGER;
BEGIN
  FOR i := 0 TO MaxWorldObjs - 1 DO materialized[i] := FALSE END
END ResetMaterialized;

PROCEDURE InitNPCs;
VAR i: INTEGER;
BEGIN
  ResetMaterialized;
  priestStatueGiven := FALSE;
  sorceressVisited := FALSE;
  darkChant := 0;
  rng := 99991;
  InitSetfigTable;
  InitSpeeches
END InitNPCs;

END NPC.
