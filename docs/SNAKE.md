# Snake (the easter egg)

A small hidden game in the launcher, a nod to the snake every old phone had. It is never listed anywhere in the UI.

**The way in:** on Home, tap the battery in the upper screen's status bar five times quickly (each tap within
0.65 s of the last). The third and fourth taps give a faint haptic detent, so a curious finger feels something stir.

**Playing:** the board fills the upper screen under the status bar, edge to edge: 27 x 18 cells of 24 px (648 px
wide, so the outer columns run 4 px past each side and there is no margin). The controls are on the lower screen.
Before a game starts the snake plays by itself behind the title card (an attract mode, `snake::Game::autopilot`).

- D-pad, stick or a swipe on either screen steers. Two quick presses inside one step are both kept, in order, so a
  tight U-turn works; a diagonal on the D-pad takes the way that turns the snake.
- A or Start starts, pauses and carries on. B pauses, then leaves. Menu or the Home gesture leaves.
- L and R pick the speed (1 to 9), Y Walls (the edges kill instead of wrapping), X Classic screen (the pale green
  phone display). Speed and Walls change only between games.
- An apple is worth the speed. After every fifth apple a beetle appears for 60 steps and wanders a cell every third
  step; it is worth more the sooner it is caught, and its countdown shows faintly at the top right (the score at the
  top left).

**How it looks and moves:**

- Normal look: a soft checkerboard (one square a cell) shows the grid without lines. The snake glides: the screen
  draws the last step in progress, the head travelling from the cell it left to the one the game moved it into, so
  a turn shows the instant the game takes it and nothing lags. The body is a tube of anti-aliased disc stamps
  (one cached texture, at most ~500 stamps a frame) with a soft shadow and a shine along its back, full-bodied and
  narrowing to a point over the last three cells; a swallowed apple slides down it as a bulge.
- The head's eyes follow the apple and blink; its tongue flicks now and then, constantly when an apple is close.
  An apple is eaten when the head reaches it on screen (it shrinks into the mouth), with crumbs, a floating score
  and the beep and buzz at that moment, not a step early. New apples pop in.
- A crash: the head lunges into what it hit, eyes turn to crosses, the board shakes, then the snake bursts piece
  by piece from the head down before the Game over card.
- Classic screen: the old way, cell by cell on a dot-matrix LCD (4 px dots with glass between them, a faint shadow
  of every lit dot on the glass, cells that fade in and out a little late), and the snake blinks three times at the
  end.
- Best scores (one with walls, one without) and the switches are kept in `userdata/snake.ini`.

**Files:** `src/launcher/snake_game.hpp` (the game, pure C++), `snake_ui.hpp` (screen state and layout),
`snake_screen.hpp` (drawing, keys, touch, sounds). Sounds are square-wave blips played through MenuAudio's sound toy
channel and follow the Menu effects setting; haptics use the existing patterns. While anything moves the loop draws
every frame (at most 16 ms apart, sooner for the next step); a paused game, or the card after a game, draws nothing
until a press. Textures (disc, board, LCD filter) are baked per renderer and dropped with the renderers.

**Tests:** `tests/test_snake.py` (builds `tests/snake_test.cpp` under ASan/UBSan, then drives the preview: the five
taps, four taps and slow taps, the switches, a wall death).
