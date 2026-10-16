# tests

Tests that need game data take the dump location from the environment
variable `SA1_DUMP` (e.g. `..\private\dump\sa1_us_1005`) and skip cleanly when
it is unset. No fixture in this directory may be derived from the game.
