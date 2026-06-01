# Faery Tale Adventure

Changes in this fork:


Enemies

  - Increased actor capacity from 20 to 48.
  - Increased simultaneous enemies from 3 to 30.
  - Expanded combat cooldown and rendering arrays so additional enemies can attack and
    render correctly.

  - Increased random encounter likelihood with a 20x multiplier, capped at the maximum
    roll threshold.

  - Wraiths now always leave a collectible item if their normal loot roll is empty.

  Player Setup

  - Julian now starts with:
      - 200 vitality
      - stronger weapon configuration
      - 150 bravery
      - 200 gold
      - 6 glass vials
      - 4 Bird Totems
      - initial weapon inventory entries

  Apples

  - Added a stash of 10 apples at Julian’s spawn location.
  - Added 1,000 apples across the outdoor map: 125 per outdoor region.
  - Apple placement prefers grass next to trees.
  - Apples are consumable using the food action.
  - Eating an apple reduces hunger by 30.

  Camping

  - Added Camp to the Items menu.
  - Camping is allowed outdoors when the player is tired and not fighting.
  - Camping consumes 2 apples.
  - Camp sleep no longer applies the bed-specific wake-up position adjustment.

  Prayer Skeletons

  - Added 4 non-hostile skeleton NPCs at the nearby stone circle.
  - They render using the skeleton enemy sprites.
  - Talking to them displays "Ohm Ohm!".

  Doors And Saves

  - Doors unlocked with keys remain unlocked when reopened.
  - Up to 128 unlocked doors are tracked.
  - Unlocked-door state is saved and loaded.
  - Older saves remain compatible because the new door data is appended optionally.

  Selling Items

  - Added Sell to the Items menu.
  - Selling is only available near a tavern bartender.
  - Apples sell for 100 gold.
  - Grey keys sell for 50 gold.

  Controls

  - Added E as a direct shortcut for taking nearby items.

  Bird Totem

  - Implemented the Bird Totem properly.
  - It now opens a dedicated overhead region map instead of incorrectly opening inventory.
  - The map displays sampled terrain, living actors in red, and the player as a blinking
    white/yellow marker.

  - Any key, click, or movement closes the view.




