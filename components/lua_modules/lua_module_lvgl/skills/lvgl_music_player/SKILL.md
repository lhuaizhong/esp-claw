---
{
  "name": "lvgl_music_player",
  "description": "Show a LVGL Lua music player demo UI with touch controls. Requires board_hardware_info skill.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "web"
  }
}
---

# LVGL Music Player

Use this skill when the user asks to show, run, preview, or demonstrate a LVGL Lua music player page on the board display.

Run exactly one script with `lua_run_script_async` after reading `board_hardware_info`.
Use `timeout_ms: 0` because this is a long-running touch UI. Do not use synchronous `lua_run_script` for this demo.

If `lua_run_script_async` returns an error, report that error directly to the user.
Do not retry with changed arguments or run another LVGL script in the same turn unless the user explicitly asks.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {}
}
```

The script does not accept arguments. It reads the display parameters from `board_manager.get_display_lcd_params("display_lcd")` and registers touch input from `board_manager.get_lcd_touch_handle("lcd_touch")` when available.

## Tool Call Inputs

Run the demo UI until cancelled:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/lvgl_music_player.lua",
  "args": {},
  "timeout_ms": 0,
  "name": "lvgl_music_player",
  "exclusive": "lvgl",
  "replace": true
}
```

## Recommended Flow

1. Activate the `board_hardware_info` skill and confirm that a `display_lcd` device is listed.
2. If no display is listed, tell the user that the board does not declare a display and stop.
3. Run `{CUR_SKILL_DIR}/scripts/lvgl_music_player.lua` with `lua_run_script_async`, `timeout_ms: 0`, `exclusive: "lvgl"`, and `replace: true`.Avoid use `lua_run_script` run.
4. Tell the user the async job id and that the UI stays active until the job is stopped.
5. If the user asks to stop, cancel, quit, or close the UI, call `lua_stop_async_job` for the running job.

## Runtime Behavior

- Touching `PLAY` toggles between `PLAY` and `PAUSE`.
- Touching `PREV` or `NEXT` updates the status text.
- Moving the progress or volume sliders prints logs like `[lvgl_music_player] progress: 42`.
- The demo does not play, decode, scan, or load audio files.
