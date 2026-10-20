# Icon styles, scrollbar and header refinement

Theme & display now offers Simple pixel and Rich icon styles. Pixel silhouettes
use whole-pixel scaling, the generated assets remain selectable, and the setting
persists independently of typography. It applies to Home, Settings and the game
menu. Generated category textures are not preloaded in pixel mode.

The horizontal rail has three separate elements: a grey visible-range window, a
wide blue selection thumb (118px on the four-item Home screen), and a narrow grey
position line. Thumb travel and line travel use their respective track extents.
The rail still reaches the bottom and both sides, with shared hit geometry.

The status wordmark is reduced from 192×28 to 116×16. Sound and Wi-Fi sit together
to its left, with aligned two-pixel native glyphs. Clock and battery remain clear.

Italian joins the six existing languages. All seven catalogs contain the same
528 keys. The language picker now fits seven choices without drawing outside the
screen, and larger choice lists use a window that follows controller focus.

Validation and live screenshots: `build/visual-options/`.

The 95-test focused run passed 93 cases and found two outdated touch coordinates
in existing tests after the Appearance layout change. Both coordinates were
updated and the two tests passed again alongside the four new visual-option
checks. Catalog checks passed for all seven complete catalogs and their shipped
font glyphs. Host previews verified both icon styles, the resized status bar,
the blue thumb/grey marker separation, Italian Theme settings and the game menu.

Final build: `0.0.1-dev-2dd9a0877b25`. A final picker hit-bound check prevents
selecting an unseen row in the bottom gap; all four visual-option tests passed
after that change. Deployed to the handheld with the previous release retained.
