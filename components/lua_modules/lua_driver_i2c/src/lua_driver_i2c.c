/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_i2c.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/i2c_master.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_bus.h"
#include "lauxlib.h"

#define LUA_DRIVER_I2C_BUS_METATABLE    "i2c.bus"
#define LUA_DRIVER_I2C_DEVICE_METATABLE "i2c.device"
#define LUA_DRIVER_I2C_DEFAULT_FREQ_HZ  400000U
#define LUA_DRIVER_I2C_SCAN_MAX         128
#define LUA_DRIVER_I2C_RW_MAX_LEN       1024
#define LUA_DRIVER_I2C_PERIPH_NAME_MAX  64

typedef struct {
    i2c_bus_handle_t bus;
    int port;
    bool owns_bus;
    bool shared_ref;
    bool board_periph_ref;
    char board_periph_name[LUA_DRIVER_I2C_PERIPH_NAME_MAX];
} lua_driver_i2c_bus_ud_t;

typedef struct {
    i2c_bus_device_handle_t dev;
    uint8_t addr;
    int bus_ref;
} lua_driver_i2c_device_ud_t;

typedef struct {
    i2c_bus_handle_t bus;
    i2c_config_t conf;
    uint32_t ref_count;
    bool owns_driver;
} lua_driver_i2c_shared_bus_t;

static SemaphoreHandle_t s_i2c_shared_lock;
static lua_driver_i2c_shared_bus_t s_i2c_shared_buses[I2C_NUM_MAX];

static esp_err_t lua_driver_i2c_ensure_shared_lock(void)
{
    if (s_i2c_shared_lock != NULL) {
        return ESP_OK;
    }

    s_i2c_shared_lock = xSemaphoreCreateMutex();
    return s_i2c_shared_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool lua_driver_i2c_config_matches(const i2c_config_t *a, const i2c_config_t *b)
{
    return a && b &&
           a->mode == b->mode &&
           a->sda_io_num == b->sda_io_num &&
           a->scl_io_num == b->scl_io_num &&
           a->sda_pullup_en == b->sda_pullup_en &&
           a->scl_pullup_en == b->scl_pullup_en &&
           a->master.clk_speed == b->master.clk_speed &&
           a->clk_flags == b->clk_flags;
}

static esp_err_t lua_driver_i2c_acquire_shared_bus(i2c_port_t port,
                                                   const i2c_config_t *conf,
                                                   bool owns_driver,
                                                   i2c_bus_handle_t *out_bus)
{
    lua_driver_i2c_shared_bus_t *slot = NULL;
    i2c_bus_handle_t bus = NULL;
    esp_err_t err;

    if (port < 0 || port >= I2C_NUM_MAX || conf == NULL || out_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_bus = NULL;

    err = lua_driver_i2c_ensure_shared_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_i2c_shared_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    slot = &s_i2c_shared_buses[port];
    if (slot->bus != NULL) {
        if (!lua_driver_i2c_config_matches(&slot->conf, conf)) {
            xSemaphoreGive(s_i2c_shared_lock);
            return ESP_ERR_INVALID_STATE;
        }
        slot->ref_count++;
        *out_bus = slot->bus;
        xSemaphoreGive(s_i2c_shared_lock);
        return ESP_OK;
    }

    bus = i2c_bus_create(port, conf);
    if (bus == NULL) {
        xSemaphoreGive(s_i2c_shared_lock);
        return ESP_FAIL;
    }

    slot->bus = bus;
    slot->conf = *conf;
    slot->ref_count = 1;
    slot->owns_driver = owns_driver;
    *out_bus = bus;
    xSemaphoreGive(s_i2c_shared_lock);
    return ESP_OK;
}

static esp_err_t lua_driver_i2c_release_shared_bus(i2c_port_t port, i2c_bus_handle_t bus)
{
    lua_driver_i2c_shared_bus_t *slot = NULL;
    esp_err_t err;

    if (port < 0 || port >= I2C_NUM_MAX || bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = lua_driver_i2c_ensure_shared_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_i2c_shared_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    slot = &s_i2c_shared_buses[port];
    if (slot->bus != bus || slot->ref_count == 0) {
        xSemaphoreGive(s_i2c_shared_lock);
        return ESP_ERR_INVALID_STATE;
    }

    slot->ref_count--;
    if (slot->ref_count > 0 || !slot->owns_driver) {
        xSemaphoreGive(s_i2c_shared_lock);
        return ESP_OK;
    }

    if (i2c_bus_get_created_device_num(slot->bus) > 0) {
        slot->ref_count++;
        xSemaphoreGive(s_i2c_shared_lock);
        return ESP_ERR_INVALID_STATE;
    }

    i2c_bus_handle_t delete_bus = slot->bus;
    err = i2c_bus_delete(&delete_bus);
    if (err == ESP_OK) {
        memset(slot, 0, sizeof(*slot));
    } else {
        slot->ref_count++;
    }
    xSemaphoreGive(s_i2c_shared_lock);
    return err;
}

static lua_driver_i2c_bus_ud_t *lua_driver_i2c_bus_get_ud(lua_State *L, int idx)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_I2C_BUS_METATABLE);
    if (!ud || !ud->bus) {
        luaL_error(L, "i2c bus: invalid or closed handle");
    }
    return ud;
}

static lua_driver_i2c_device_ud_t *lua_driver_i2c_device_get_ud(lua_State *L, int idx)
{
    lua_driver_i2c_device_ud_t *ud = (lua_driver_i2c_device_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_I2C_DEVICE_METATABLE);
    if (!ud || !ud->dev) {
        luaL_error(L, "i2c device: invalid or closed handle");
    }
    return ud;
}

static uint8_t lua_driver_i2c_mem_addr(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) {
        return NULL_I2C_MEM_ADDR;
    }
    lua_Integer v = luaL_checkinteger(L, idx);
    if (v < 0 || v > 0xFF) {
        luaL_error(L, "mem_addr must be in range 0-255");
    }
    return (uint8_t)v;
}

static esp_err_t lua_driver_i2c_bus_release(lua_driver_i2c_bus_ud_t *ud)
{
    esp_err_t err = ESP_OK;

    if (ud == NULL || ud->bus == NULL) {
        return ESP_OK;
    }
    if (ud->shared_ref) {
        err = lua_driver_i2c_release_shared_bus((i2c_port_t)ud->port, ud->bus);
        if (err == ESP_OK) {
            ud->bus = NULL;
            ud->shared_ref = false;
        }
    } else if (ud->owns_bus) {
        err = i2c_bus_delete(&ud->bus);
    } else {
        ud->bus = NULL;
    }

    if (err == ESP_OK && ud->bus == NULL && ud->board_periph_ref) {
        esp_err_t unref_err = esp_board_periph_unref_handle(ud->board_periph_name);
        ud->board_periph_ref = false;
        ud->board_periph_name[0] = '\0';
        err = unref_err;
    }

    return err;
}

static int lua_driver_i2c_bus_gc(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_BUS_METATABLE);
    if (ud && ud->bus) {
        (void)lua_driver_i2c_bus_release(ud);
    }
    return 0;
}

static int lua_driver_i2c_bus_close(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2C_BUS_METATABLE);
    if (ud->bus) {
        esp_err_t err = lua_driver_i2c_bus_release(ud);
        if (err != ESP_OK) {
            return luaL_error(L, "i2c bus close failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

static int lua_driver_i2c_bus_scan(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = lua_driver_i2c_bus_get_ud(L, 1);
    uint8_t buf[LUA_DRIVER_I2C_SCAN_MAX];
    uint8_t count = i2c_bus_scan(ud->bus, buf, LUA_DRIVER_I2C_SCAN_MAX);
    lua_createtable(L, count, 0);
    for (uint8_t i = 0; i < count; i++) {
        lua_pushinteger(L, buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int lua_driver_i2c_bus_device(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = lua_driver_i2c_bus_get_ud(L, 1);
    lua_Integer addr = luaL_checkinteger(L, 2);
    uint32_t clk_speed = (uint32_t)luaL_optinteger(L, 3, 0);

    if (addr < 0 || addr > 0x7F) {
        return luaL_error(L, "i2c address must be in range 0-127");
    }

    i2c_bus_device_handle_t dev = i2c_bus_device_create(ud->bus, (uint8_t)addr, clk_speed);
    if (!dev) {
        return luaL_error(L, "i2c device create failed");
    }

    lua_driver_i2c_device_ud_t *dud = (lua_driver_i2c_device_ud_t *)lua_newuserdata(
        L, sizeof(*dud));
    dud->dev = dev;
    dud->addr = (uint8_t)addr;
    dud->bus_ref = LUA_NOREF;
    luaL_getmetatable(L, LUA_DRIVER_I2C_DEVICE_METATABLE);
    lua_setmetatable(L, -2);

    lua_pushvalue(L, 1);
    dud->bus_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    return 1;
}

static int lua_driver_i2c_device_gc(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = (lua_driver_i2c_device_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_DEVICE_METATABLE);
    if (ud) {
        if (ud->dev) {
            i2c_bus_device_delete(&ud->dev);
        }
        if (ud->bus_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->bus_ref);
            ud->bus_ref = LUA_NOREF;
        }
    }
    return 0;
}

static int lua_driver_i2c_device_close(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = (lua_driver_i2c_device_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2C_DEVICE_METATABLE);
    if (ud->dev) {
        esp_err_t err = i2c_bus_device_delete(&ud->dev);
        if (err != ESP_OK) {
            return luaL_error(L, "i2c device close failed: %s", esp_err_to_name(err));
        }
    }
    if (ud->bus_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->bus_ref);
        ud->bus_ref = LUA_NOREF;
    }
    return 0;
}

static int lua_driver_i2c_device_address(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    lua_pushinteger(L, ud->addr);
    return 1;
}

static int lua_driver_i2c_device_read_byte(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 2);
    uint8_t data = 0;
    esp_err_t err = i2c_bus_read_byte(ud->dev, mem_addr, &data);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c read_byte failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, data);
    return 1;
}

static int lua_driver_i2c_device_read(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    lua_Integer len = luaL_checkinteger(L, 2);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 3);

    if (len <= 0 || len > LUA_DRIVER_I2C_RW_MAX_LEN) {
        return luaL_error(L, "i2c read length must be 1-%d", LUA_DRIVER_I2C_RW_MAX_LEN);
    }

    luaL_Buffer b;
    uint8_t *buf = (uint8_t *)luaL_buffinitsize(L, &b, (size_t)len);
    esp_err_t err = i2c_bus_read_bytes(ud->dev, mem_addr, (size_t)len, buf);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c read failed: %s", esp_err_to_name(err));
    }
    luaL_pushresultsize(&b, (size_t)len);
    return 1;
}

static int lua_driver_i2c_device_write_byte(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    lua_Integer value = luaL_checkinteger(L, 2);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 3);

    if (value < 0 || value > 0xFF) {
        return luaL_error(L, "i2c write_byte value must be 0-255");
    }

    esp_err_t err = i2c_bus_write_byte(ud->dev, mem_addr, (uint8_t)value);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c write_byte failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_i2c_device_write(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 3);

    const uint8_t *data = NULL;
    size_t data_len = 0;

    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        data = (const uint8_t *)lua_tolstring(L, 2, &data_len);
    } else if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, 2);
        if (n < 0 || n > LUA_DRIVER_I2C_RW_MAX_LEN) {
            return luaL_error(L, "i2c write table length must be 0-%d",
                              LUA_DRIVER_I2C_RW_MAX_LEN);
        }
        uint8_t *tmp = (uint8_t *)lua_newuserdata(L, (size_t)(n > 0 ? n : 1));
        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, 2, i + 1);
            lua_Integer byte = luaL_checkinteger(L, -1);
            if (byte < 0 || byte > 0xFF) {
                return luaL_error(L, "i2c write byte #%d out of range 0-255",
                                  (int)(i + 1));
            }
            tmp[i] = (uint8_t)byte;
            lua_pop(L, 1);
        }
        data = tmp;
        data_len = (size_t)n;
    } else {
        return luaL_error(L, "i2c write expects a string or table");
    }

    if (data_len == 0) {
        return 0;
    }
    if (data_len > LUA_DRIVER_I2C_RW_MAX_LEN) {
        return luaL_error(L, "i2c write length must be 1-%d", LUA_DRIVER_I2C_RW_MAX_LEN);
    }

    esp_err_t err = i2c_bus_write_bytes(ud->dev, mem_addr, data_len, data);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c write failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static void lua_driver_i2c_push_bus(lua_State *L,
                                    i2c_bus_handle_t bus,
                                    int port,
                                    bool owns_bus,
                                    bool shared_ref,
                                    const char *board_periph_name)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)lua_newuserdata(
        L, sizeof(*ud));

    ud->bus = bus;
    ud->port = port;
    ud->owns_bus = owns_bus;
    ud->shared_ref = shared_ref;
    ud->board_periph_ref = board_periph_name != NULL && board_periph_name[0] != '\0';
    ud->board_periph_name[0] = '\0';
    if (ud->board_periph_ref) {
        strlcpy(ud->board_periph_name, board_periph_name, sizeof(ud->board_periph_name));
    }
    luaL_getmetatable(L, LUA_DRIVER_I2C_BUS_METATABLE);
    lua_setmetatable(L, -2);
}

static esp_err_t lua_driver_i2c_open_board_periph(const char *peripheral_name,
                                                  uint32_t freq_hz,
                                                  i2c_bus_handle_t *out_bus,
                                                  int *out_port)
{
    i2c_master_bus_handle_t i2c_master_handle = NULL;
    i2c_master_bus_config_t *i2c_master_cfg = NULL;

    if (!peripheral_name || !peripheral_name[0] || !out_bus || !out_port) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(esp_board_periph_ref_handle(peripheral_name, (void **)&i2c_master_handle),
                        "lua_i2c", "Failed to reference board I2C bus '%s'", peripheral_name);

    esp_err_t err = esp_board_periph_get_config(peripheral_name, (void **)&i2c_master_cfg);
    if (err != ESP_OK) {
        esp_board_periph_unref_handle(peripheral_name);
        return err;
    }
    if (i2c_master_cfg == NULL) {
        esp_board_periph_unref_handle(peripheral_name);
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = i2c_master_cfg->sda_io_num,
        .scl_io_num = i2c_master_cfg->scl_io_num,
        .sda_pullup_en = i2c_master_cfg->flags.enable_internal_pullup,
        .scl_pullup_en = i2c_master_cfg->flags.enable_internal_pullup,
        .master.clk_speed = freq_hz ? freq_hz : LUA_DRIVER_I2C_DEFAULT_FREQ_HZ,
        .clk_flags = 0,
    };

    (void)i2c_master_handle;
    err = lua_driver_i2c_acquire_shared_bus(i2c_master_cfg->i2c_port, &conf, false, out_bus);
    if (err != ESP_OK) {
        esp_board_periph_unref_handle(peripheral_name);
        return err;
    }
    *out_port = (int)i2c_master_cfg->i2c_port;
    return ESP_OK;
}

static const char *lua_driver_i2c_find_board_periph(int port, int sda, int scl)
{
    extern const esp_board_periph_desc_t g_esp_board_peripherals[];
    const esp_board_periph_desc_t *desc = g_esp_board_peripherals;

    while (desc && desc->name) {
        if (desc->type && strcmp(desc->type, "i2c") == 0 &&
                desc->role == ESP_BOARD_PERIPH_ROLE_MASTER &&
                desc->cfg != NULL &&
                desc->cfg_size >= (int)sizeof(i2c_master_bus_config_t)) {
            const i2c_master_bus_config_t *cfg = (const i2c_master_bus_config_t *)desc->cfg;
            if ((int)cfg->i2c_port == port &&
                    (int)cfg->sda_io_num == sda &&
                    (int)cfg->scl_io_num == scl) {
                return desc->name;
            }
        }
        desc = desc->next;
    }

    return NULL;
}

static int lua_driver_i2c_from_peripheral(lua_State *L)
{
    const char *peripheral_name = luaL_checkstring(L, 1);
    lua_Integer freq = luaL_optinteger(L, 2, LUA_DRIVER_I2C_DEFAULT_FREQ_HZ);
    i2c_bus_handle_t bus = NULL;
    int port = 0;

    if (freq <= 0) {
        return luaL_error(L, "i2c freq must be positive");
    }

    esp_err_t err = lua_driver_i2c_open_board_periph(peripheral_name,
                                                     (uint32_t)freq,
                                                     &bus,
                                                     &port);
    if (err != ESP_OK) {
        return luaL_error(L,
                          "i2c board peripheral '%s' open failed: %s",
                          peripheral_name,
                          esp_err_to_name(err));
    }

    lua_driver_i2c_push_bus(L, bus, port, false, true, peripheral_name);
    return 1;
}

static int lua_driver_i2c_new(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    lua_Integer sda = luaL_checkinteger(L, 2);
    lua_Integer scl = luaL_checkinteger(L, 3);
    lua_Integer freq = luaL_optinteger(L, 4, LUA_DRIVER_I2C_DEFAULT_FREQ_HZ);
    const char *board_periph_name = NULL;
    i2c_bus_handle_t bus = NULL;
    esp_err_t err;
    int resolved_port = 0;

    if (freq <= 0) {
        return luaL_error(L, "i2c freq must be positive");
    }

    board_periph_name = lua_driver_i2c_find_board_periph((int)port, (int)sda, (int)scl);
    if (board_periph_name != NULL) {
        err = lua_driver_i2c_open_board_periph(board_periph_name,
                                               (uint32_t)freq,
                                               &bus,
                                               &resolved_port);
        if (err != ESP_OK) {
            return luaL_error(L,
                              "i2c board peripheral '%s' open failed: %s",
                              board_periph_name,
                              esp_err_to_name(err));
        }
        lua_driver_i2c_push_bus(L, bus, resolved_port, false, true, board_periph_name);
        return 1;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = (int)sda,
        .scl_io_num = (int)scl,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .master.clk_speed = (uint32_t)freq,
    };

    i2c_master_bus_handle_t existing_bus = NULL;
    bool owns_bus = i2c_master_get_bus_handle((i2c_port_t)port, &existing_bus) != ESP_OK;

    err = lua_driver_i2c_acquire_shared_bus((i2c_port_t)port,
                                            &conf,
                                            owns_bus,
                                            &bus);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c bus create failed on port %d: %s",
                          (int)port, esp_err_to_name(err));
    }

    lua_driver_i2c_push_bus(L, bus, (int)port, owns_bus, true, NULL);
    return 1;
}

int luaopen_i2c(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_I2C_BUS_METATABLE)) {
        lua_pushcfunction(L, lua_driver_i2c_bus_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_i2c_bus_scan);
        lua_setfield(L, -2, "scan");
        lua_pushcfunction(L, lua_driver_i2c_bus_device);
        lua_setfield(L, -2, "device");
        lua_pushcfunction(L, lua_driver_i2c_bus_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, LUA_DRIVER_I2C_DEVICE_METATABLE)) {
        lua_pushcfunction(L, lua_driver_i2c_device_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_i2c_device_read_byte);
        lua_setfield(L, -2, "read_byte");
        lua_pushcfunction(L, lua_driver_i2c_device_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_i2c_device_write_byte);
        lua_setfield(L, -2, "write_byte");
        lua_pushcfunction(L, lua_driver_i2c_device_write);
        lua_setfield(L, -2, "write");
        lua_pushcfunction(L, lua_driver_i2c_device_address);
        lua_setfield(L, -2, "address");
        lua_pushcfunction(L, lua_driver_i2c_device_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_i2c_new);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, lua_driver_i2c_from_peripheral);
    lua_setfield(L, -2, "from_peripheral");
    return 1;
}

esp_err_t lua_driver_i2c_register(void)
{
    ESP_RETURN_ON_ERROR(lua_driver_i2c_ensure_shared_lock(),
                        "lua_i2c", "Failed to create shared I2C lock");
    return cap_lua_register_module("i2c", luaopen_i2c);
}
