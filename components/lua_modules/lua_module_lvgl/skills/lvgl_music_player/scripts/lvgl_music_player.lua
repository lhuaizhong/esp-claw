-- 320x320 LVGL Lua music player UI demo.
-- This is a visual/touch demo only. It does not play, decode, or load audio.

local board_manager = require("board_manager")
local lvgl = require("lvgl")

local panel_handle, io_handle, width, height, panel_if =
    board_manager.get_display_lcd_params("display_lcd")

lvgl.init(panel_handle, io_handle, width, height, panel_if, {
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local touch_registered = false

local function log_ui(event, value)
    if value == nil then
        print("[music_player] " .. event)
    else
        print("[music_player] " .. event .. ": " .. tostring(value))
    end
end

local function add_label(parent, text, x, y, color)
    return lvgl.label(parent, {
        text = text,
        x = x,
        y = y,
        text_color = color or "#f8fafc",
    })
end

local function make_button(parent, text, x, y, w, h, bg_color)
    return lvgl.button(parent, {
        text = text,
        x = x,
        y = y,
        w = w,
        h = h,
        radius = 10,
        bg_color = bg_color,
        bg_opa = 255,
        border_width = 0,
        text_color = "#ffffff",
    })
end

local ok, err = pcall(function()
    local touch_handle, touch_err = board_manager.get_lcd_touch_handle("lcd_touch")
    if touch_handle == nil then
        print("no touch handle on this board, music player demo will show UI only:", touch_err)
    else
        touch_registered = lvgl.indev_register("touch", touch_handle)
        print("touch indev registered:", touch_registered)
    end

    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#07111f" })

    local root_w = 320
    local root_h = 320
    local root = lvgl.container(scr, {
        align = "center",
        w = root_w,
        h = root_h,
        bg_color = "#07111f",
        bg_opa = 255,
        border_width = 0,
        pad = 0,
    })

    local glow = lvgl.container(root, {
        x = 18,
        y = 12,
        w = 284,
        h = 296,
        bg_color = "#13243a",
        bg_opa = 255,
        border_color = "#25415f",
        border_width = 1,
        radius = 18,
        pad = 0,
    })

    add_label(glow, "LUA MUSIC", 18, 12, "#8bd3ff")
    local status = add_label(glow, "Ready", 221, 12, "#94a3b8")

    local art = lvgl.container(glow, {
        x = 84,
        y = 42,
        w = 116,
        h = 116,
        bg_color = "#0b1726",
        bg_opa = 255,
        border_color = "#38bdf8",
        border_width = 2,
        radius = 58,
        pad = 0,
    })

    lvgl.container(art, {
        align = "center",
        w = 70,
        h = 70,
        bg_color = "#193958",
        bg_opa = 255,
        border_color = "#5eead4",
        border_width = 1,
        radius = 35,
        pad = 0,
    })

    lvgl.container(art, {
        align = "center",
        w = 24,
        h = 24,
        bg_color = "#e2e8f0",
        bg_opa = 255,
        border_width = 0,
        radius = 12,
        pad = 0,
    })

    add_label(glow, "Night Drive", 82, 166, "#f8fafc")
    add_label(glow, "ESP Lua Session", 85, 187, "#94a3b8")

    local progress = lvgl.slider(glow, {
        x = 32,
        y = 211,
        w = 220,
        h = 12,
        min = 0,
        max = 100,
        value = 38,
        bg_color = "#0f2134",
        border_width = 0,
    })
    progress:set_value(38)

    add_label(glow, "1:14", 32, 229, "#94a3b8")
    add_label(glow, "3:08", 224, 229, "#94a3b8")

    local prev_btn = make_button(glow, "PREV", 30, 238, 66, 36, "#1f3348")
    local play_btn = make_button(glow, "PLAY", 108, 232, 68, 48, "#0ea5e9")
    local next_btn = make_button(glow, "NEXT", 188, 238, 66, 36, "#1f3348")

    add_label(glow, "VOL", 30, 280, "#94a3b8")
    local volume = lvgl.slider(glow, {
        x = 70,
        y = 288,
        w = 170,
        h = 8,
        min = 0,
        max = 100,
        value = 62,
        bg_color = "#102033",
        border_width = 0,
        radius = 4,
    })
    volume:set_value(62)

    local is_playing = false
    play_btn:on("clicked", function()
        is_playing = not is_playing
        if is_playing then
            play_btn:set_text("PAUSE")
            play_btn:set_style({ bg_color = "#14b8a6" })
            status:set_text("Playing")
            log_ui("play_state", "Playing")
        else
            play_btn:set_text("PLAY")
            play_btn:set_style({ bg_color = "#0ea5e9" })
            status:set_text("Ready")
            log_ui("play_state", "Ready")
        end
    end)

    prev_btn:on("clicked", function()
        status:set_text("Previous")
        log_ui("track_action", "Previous")
    end)

    next_btn:on("clicked", function()
        status:set_text("Next")
        log_ui("track_action", "Next")
    end)

    progress:on("value_changed", function()
        log_ui("progress", progress:get_value())
    end)

    volume:on("value_changed", function()
        log_ui("volume", volume:get_value())
    end)

    scr:load()
    log_ui("screen_loaded", "ready")
    lvgl.run()
end)

if touch_registered then
    local unreg_ok, unreg_err = pcall(lvgl.indev_unregister, "touch")
    if not unreg_ok then
        print("touch indev unregister failed: " .. tostring(unreg_err))
    end
end

lvgl.deinit()

if not ok then
    error(err)
end
