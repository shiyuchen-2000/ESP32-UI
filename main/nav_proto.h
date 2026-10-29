/*
 * MotoNav display protocol (phone -> watch), text navigation subset.
 *
 * The Android app sends one message per BLE write:
 *   R,300 / L,120 / S,800 / SL,420 / SR,420 / U,300 / A,0
 *                                              turn + distance (m), optional
 *                                              third field = turn angle
 *   C                                          clear / standby
 *   P                                          heartbeat (ignored)
 *   #v,42                                      speed km/h
 *   #e,remain_m,eta_hhmm,lights  / #e          trip info / clear
 *   #p,cs,x,y,heading,speed                    position (reserved for the map)
 *   B... / M...                                images / bulk (map, route)
 *
 * Unknown messages are ignored: the app is developed independently and may
 * add commands at any time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NAV_TURN_NONE = 0,
    NAV_TURN_LEFT,
    NAV_TURN_RIGHT,
    NAV_TURN_STRAIGHT,
    NAV_TURN_SLIGHT_LEFT,
    NAV_TURN_SLIGHT_RIGHT,
    NAV_TURN_UTURN,
    NAV_TURN_ARRIVE,
} nav_turn_t;

typedef struct {
    bool connected;       /* BLE link up */
    bool subscribed;      /* phone enabled notifications (TX CCCD) */
    bool active;          /* a navigation session is running */
    nav_turn_t turn;
    int distance_m;
    int turn_angle;       /* optional third field of R/L/S */
    int speed_kmh;
    bool has_trip;
    int remain_m;
    int eta_hhmm;
    int lights;
    /* #p position: decimetres in the phone's map frame */
    int pos_frame;
    int pos_x_dm, pos_y_dm;
    int heading;
    /* display options pushed by the app's own style picker */
    int  theme;           /* #t,0..11 */
    bool night;           /* #n,1/0 */
    bool map_on;          /* #m,1/0 */
    int  trail;           /* #r,1 = grey done part, 2 = erase */
    /* #Z music state (phone MusicBridge) */
    bool music_have;      /* a controllable media session exists */
    bool music_playing;
    int  music_actions;   /* 1 = prev, 2 = play/pause, 4 = next */
    int  music_ack;       /* last #ZACK code: 2 = accepted, 5 = no player */
    int  music_ack_seq;   /* bumped on every #ZACK so the UI can react once */
} nav_state_t;

void nav_proto_init(void);
void nav_proto_set_link(bool connected, bool subscribed);
void nav_proto_on_rx(const uint8_t *data, uint16_t len);

/* Thread-safe snapshot for the UI task. */
void nav_proto_get(nav_state_t *out);

/* ms timestamp (esp_timer) of the last received message, 0 if none yet. */
uint32_t nav_proto_last_rx_ms(void);

/* Implemented by nav_ble.c: notify the phone (ACK "K,type,id", pairing "U,1"). */
void nav_ble_notify(const char *text);
